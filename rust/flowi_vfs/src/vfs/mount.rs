//! The mount surface: the factory, the borrowed and owning mounts, the worker view,
//! and the values a mount is opened with and reports.

use super::*;

/// What a mount is opened with. [`Default`] is what the C vfs_mount()
/// convenience macro passes: the driver's own cache location, no revalidation, no
/// file watching.
#[derive(Copy, Clone, Debug, Default)]
pub struct MountOptions<'a> {
    /// Where a caching driver writes its downloads. Empty selects the default
    /// cache; a directory given here must already exist.
    pub cache_dir: &'a str,
    /// Revalidate cached files against the server (ETag) before using them - for
    /// a mutable remote resource. false uses the cache as-is.
    pub cache_validation: bool,
    /// Start a file watcher for this mount (LocalFS only). Must be set here, at
    /// mount time; see [`Mount::enable_watching`] for the after-the-fact form.
    pub enable_file_watching: bool,
}

impl MountOptions<'_> {
    /// Lower to the ABI struct. cache_dir is borrowed for the duration of the
    /// mount call, which is all the C side reads it for.
    pub(super) fn to_sys(self) -> sys::VfsMountOptions {
        sys::VfsMountOptions {
            cache_dir: sys::RawStr::borrow(self.cache_dir),
            cache_validation: self.cache_validation,
            enable_file_watching: self.enable_file_watching,
        }
    }
}

/// Why a mount could not be opened - one variant per non-success
/// FlVfsMountErrorStatus.
///
/// This is the refusal: the request never became a mount. A mount that opens
/// and then fails while its driver resolves it is a live [`Mount`] reporting
/// [`MountStatus::Error`], not an error here.
///
/// The variants, their C statuses and their messages are one list below, which
/// generates all three, so a variant cannot exist without a status to arrive as and
/// something to say.
#[doc(inline)]
pub use self::mount_error_decl::MountError;

/// Generates the enum, its [`Display`](core::fmt::Display), the status mapping and the
/// table the test iterates, from one list of `Variant = Status, "message";`.
///
/// Both generated matches stay exhaustive, which is the point: a new
/// `FlVfsMountErrorStatus` from the C side fails to compile here until the list names
/// it, rather than quietly mapping to [`None`] and reading as a success.
macro_rules! mount_errors {
    ($( $(#[$doc:meta])* $variant:ident = $status:ident, $text:literal; )*) => {
        /// Why a mount could not be opened - one variant per non-success
        /// FlVfsMountErrorStatus.
        ///
        /// This is the refusal: the request never became a mount. A mount that opens
        /// and then fails while its driver resolves it is a live [`Mount`] reporting
        /// [`MountStatus::Error`], not an error here.
        #[derive(Copy, Clone, PartialEq, Eq, Debug)]
        pub enum MountError {
            $( $(#[$doc])* $variant, )*
        }

        impl core::fmt::Display for MountError {
            fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                f.write_str(match self {
                    $( MountError::$variant => $text, )*
                })
            }
        }

        impl MountError {
            /// Map a mount result's status. Success yields [`None`] - the caller has a
            /// mount pointer to use instead.
            pub(super) fn from_status(status: sys::VfsMountErrorStatus) -> Option<MountError> {
                match status {
                    sys::VfsMountErrorStatus::Success => None,
                    $( sys::VfsMountErrorStatus::$status => Some(MountError::$variant), )*
                }
            }
        }

        /// Every refusal as (status, variant, message), for the test that proves the
        /// mapping is total.
        #[cfg(test)]
        pub(crate) const MOUNT_ERRORS: &[(sys::VfsMountErrorStatus, MountError, &str)] = &[
            $( (sys::VfsMountErrorStatus::$status, MountError::$variant, $text), )*
        ];
    };
}

mod mount_error_decl {
    use super::*;

    mount_errors! {
        /// The source path is empty or malformed.
        InvalidPath = InvalidPath, "invalid source path";
        /// No VFS driver claimed the source.
        PluginNotFound = PluginNotFound, "no VFS driver for this source";
        /// A driver claimed it and failed to open it - missing, unreadable, denied.
        MountFailed = MountFailed, "the driver could not open the source";
        /// Allocation failed.
        OutOfMemory = OutOfMemory, "out of memory";
        /// The VFS has not been initialized.
        NotInitialized = NotInitialized, "the VFS is not initialized";
    }
}

#[cfg(test)]
pub(super) use self::mount_error_decl::MOUNT_ERRORS;

impl std::error::Error for MountError {}

/// Mount readiness - the coarse state [`Mount::status`] reports.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum MountStatus {
    /// The mount operation is still in flight.
    Pending,
    /// Ready for reads and listings.
    Ready,
    /// The mount failed.
    Error,
}

/// What a mount reports about itself, as owned values - the snapshot
/// [`Mount::info`] takes.
///
/// [`Mount::source_path`] is the zero-copy form of the path field, for a caller
/// that only wants to read it; this is for one that wants to keep it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct MountInfo {
    /// The source path the mount was created from.
    pub path: String,
    /// Whether the mount is ready for operations.
    pub is_ready: bool,
}

/// A borrowed VFS mount, and the surface every VFS operation lives on.
///
/// Host-owned and opaque. [`Vfs::mount`] hands back the owning [`OwnedMount`],
/// which derefs to this; a mount that came from C is wrapped with
/// [`Mount::from_raw`].
///
/// Deliberately not [`Clone`]. A clone would be a second handle onto a mount
/// whose lifetime the first one governs - clone it off an [`OwnedMount`], drop the
/// owner, and the copy is a dangling mount reachable from entirely safe code. A
/// caller that needs to hand the same mount to several consumers borrows it
/// (&Mount), or, for a mount meant to live for the whole run, takes the
/// &'static Mount [`OwnedMount::leak`] returns.
///
/// !Send + !Sync; [`Mount::worker_view`] is the way to a job body.
pub struct Mount {
    /// Crate-private rather than module-private because [`Mount::read_with`] lives in
    /// [`super::mapped`], with the slot machinery whose ownership rules it sets up.
    pub(crate) raw: *mut sys::FlVfsMount,
    _marker: PhantomData<*const ()>,
}

impl Mount {
    /// Wrap a raw `FlVfsMount*` (as an opaque pointer) obtained from the flowi VFS -
    /// a mount a C caller opened and still owns.
    ///
    /// # Safety
    /// raw must be a valid `FlVfsMount*` that outlives this Mount and every
    /// operation launched against it.
    #[inline]
    pub unsafe fn from_raw(raw: *mut c_void) -> Mount {
        Mount::from_raw_ptr(raw as *mut sys::FlVfsMount)
    }

    /// Wrap an already-typed mount pointer - the one place the struct is built.
    ///
    /// Not unsafe because it is crate-private and constructs only a borrow: the
    /// validity promise belongs to whoever hands the pointer in, which is either
    /// [`Mount::from_raw`]'s caller or [`Vfs::mount_with`].
    #[inline]
    fn from_raw_ptr(raw: *mut sys::FlVfsMount) -> Mount {
        Mount {
            raw,
            _marker: PhantomData,
        }
    }

    /// The raw `FlVfsMount*`, for handing to C that takes one - an application whose
    /// media, config or asset loading is still C, and the toolkit's own mount-borrowing
    /// entry points (Image::load_mount, Font::load_mount, Icon::load). Valid for as long
    /// as the mount is open.
    #[inline]
    pub fn as_raw(&self) -> *mut c_void {
        self.raw.cast()
    }

    /// Everything the mount reports about itself, copied out.
    pub fn info(&self) -> MountInfo {
        // SAFETY: a live mount (the from_raw / Vfs::mount contract).
        let info = unsafe { ffi::vfs_mount_get_info(self.raw) };
        MountInfo {
            path: info.path.into_string(),
            is_ready: info.is_ready,
        }
    }

    /// Where this mount was created from - the source path [`Vfs::mount`] was given.
    ///
    /// Borrowed, not copied: the C side hands back the mount's own source_path
    /// field, which lives exactly as long as the mount, so the &self borrow is the
    /// real lifetime and reading it costs nothing. [`Mount::info`] is the owning form.
    #[inline]
    pub fn source_path(&self) -> &str {
        // SAFETY: a live mount. vfs_mount_get_info returns the mount's own
        // source_path field - owned by the mount, immutable for its lifetime - so
        // tying the borrow to &self is exactly right.
        unsafe { ffi::vfs_mount_get_info(self.raw).path.as_str() }
    }

    /// Whether the mount has finished coming up, failed, or is still in flight.
    /// Mounting is asynchronous, so a fresh mount reports [`MountStatus::Pending`]
    /// until its driver has resolved it.
    pub fn status(&self) -> MountStatus {
        // SAFETY: a live mount.
        match unsafe { ffi::vfs_mount_get_status(self.raw) } {
            sys::VfsMountStatus::Ready => MountStatus::Ready,
            sys::VfsMountStatus::Error => MountStatus::Error,
            sys::VfsMountStatus::Pending => MountStatus::Pending,
        }
    }

    /// True once the mount is ready for reads and listings.
    #[inline]
    pub fn is_ready(&self) -> bool {
        // SAFETY: a live mount.
        unsafe { ffi::vfs_mount_is_ready(self.raw) }
    }

    /// The job the mount's own bring-up runs as, so work that reads through this
    /// mount can be scheduled after it rather than polling [`is_ready`] for it.
    ///
    /// That is the difference between a job body that starts when the mount is up
    /// and one that starts immediately and spins; [`crate::Jobs::add_after`] takes
    /// this handle directly.
    ///
    /// A driver that resolved the mount without scheduling anything reports
    /// [`JobHandle::INVALID`], and a handle whose job has already finished is a
    /// recycled one. add_after treats both as "no dependency" and schedules the
    /// job straight away, so the same call is correct whatever the mount's state -
    /// there is no readiness check to write around it.
    #[inline]
    pub fn job_handle(&self) -> crate::JobHandle {
        // SAFETY: a live mount.
        unsafe { ffi::vfs_mount_get_job_handle(self.raw) }
    }

    /// Turn on file watching for this mount (LocalFS only). Must be called before
    /// the mount becomes ready. False if watching could not be enabled.
    #[inline]
    pub fn enable_watching(&self) -> bool {
        // SAFETY: a live mount.
        unsafe { ffi::vfs_mount_enable_watching(self.raw) }
    }

    /// Launch an async read of path within this mount. [`None`] if the C side
    /// refused to start the operation; otherwise a [`Handle`] that polls to the
    /// file's bytes.
    pub fn read(&self, path: &str) -> Option<Handle<VfsData>> {
        // SAFETY: a live mount; path's bytes are borrowed only for the duration
        // of this call (the C side copies what it needs), and the invalid
        // reuse_handle requests a fresh ticket.
        let ticket = unsafe {
            ffi::vfs_mount_read_all_with_options(
                self.raw,
                sys::RawStr::borrow(path),
                None,
                ptr::null_mut(),
                None,
                FL_VFS_HANDLE_INVALID,
            )
        };
        self.ticket_handle(ticket)
    }

    /// Launch an async listing of path within this mount to depth (1 = immediate
    /// children). [`None`] if the C side refused to start it, otherwise a [`Handle`]
    /// that polls to the listing.
    pub fn listing(&self, path: &str, depth: i32) -> Option<Handle<FileList>> {
        // SAFETY: as read; vfs_get_listing borrows path for the call only.
        let ticket = unsafe { ffi::vfs_get_listing(self.raw, sys::RawStr::borrow(path), depth) };
        self.ticket_handle(ticket)
    }

    /// Wrap a launch's ticket, mapping the invalid sentinel to [`None`]. Crate-private
    /// for the same reason as [`Mount::raw`].
    #[inline]
    pub(crate) fn ticket_handle<T: Payload>(&self, ticket: FlVfsHandle) -> Option<Handle<T>> {
        (ticket != FL_VFS_HANDLE_INVALID).then(|| Handle::from_ticket(ticket))
    }

    /// A Send view of this mount, carrying the blocking form of the same
    /// operations - what a job body reads through.
    ///
    /// A worker must not poll vfs_is_ready instead: that burns a worker core and,
    /// worse, can deadlock - the VFS runs its own work on the same pool, so a worker
    /// spinning on vfs_is_ready may be spinning on a job that needs the slot it is
    /// occupying. [`WorkerMount`] avoids both: on a worker the job system runs a
    /// newly scheduled job inline, so each operation there is complete by the time
    /// its launch returns. The blocking calls live only here: on the frame loop they
    /// would be a stall.
    ///
    /// # Safety
    /// The mount must stay open until every holder of the returned view - and every
    /// job body reading through one - is finished with it. The view is Send and
    /// carries no lifetime, so nothing but this promise ties it to the mount.
    #[inline]
    pub unsafe fn worker_view(&self) -> WorkerMount {
        WorkerMount {
            mount: Mount::from_raw_ptr(self.raw),
        }
    }
}

/// A mount this process opened and is responsible for closing.
///
/// [`Mount`] is a borrow of a mount someone else owns; this is the owning form
/// [`Vfs::mount`] hands back. It derefs to Mount, so every operation works on it
/// directly, and closes the mount on drop.
pub struct OwnedMount {
    mount: Mount,
}

impl OwnedMount {
    /// Give up ownership: the mount stays open for the rest of the process, and the
    /// &'static Mount this returns is how everything reaches it from then on -
    /// the way several long-lived consumers share one mount.
    pub fn leak(self) -> &'static Mount {
        let mount = Mount::from_raw_ptr(self.mount.raw);
        core::mem::forget(self);
        Box::leak(Box::new(mount))
    }
}

impl Deref for OwnedMount {
    type Target = Mount;

    #[inline]
    fn deref(&self) -> &Mount {
        &self.mount
    }
}

impl Drop for OwnedMount {
    fn drop(&mut self) {
        // SAFETY: the mount came from Vfs::mount and is closed exactly once.
        unsafe { ffi::vfs_mount_close(self.mount.raw) };
    }
}

/// A mount view that may cross to a worker thread, carrying the blocking form
/// of the mount operations. Minted by [`Mount::worker_view`], which is where the
/// safety contract lives.
///
/// It derefs to [`Mount`], so the queries (info, status, source_path, ...) are
/// all reachable; the three operations below shadow their async namesakes, so the
/// handle you are holding decides which surface you get. Never use it on the frame
/// loop: every call here waits.
///
/// Each operation below launches the ticket or tickets it needs and closes them
/// before returning, which is what makes the surface safe on a job worker: the
/// job system runs a job scheduled from a worker inline, so a ticket is finished
/// by the time the wait and close reach it.
///
/// [`WorkerMount::read_prefix`] is the exception worth naming, because it chains:
/// it opens a file and then reads through that open handle, and a chained op does
/// not go through the job system at all. It is sound for a narrower reason - it
/// chains only onto the ticket it opened itself, in the same call, which is
/// therefore already complete - so the C side's refusal for a chain onto an
/// unfinished predecessor (`VFS_ERROR_WOULD_BLOCK`) cannot trigger here. A method
/// added below that chains onto a handle from anywhere else does not inherit that
/// and must handle the refusal.
///
/// A ticket launched on another thread is not usable through this view at all -
/// `vfs_wait` cannot block a worker, and `vfs_close` refuses there rather than
/// free an operation that is still running.
pub struct WorkerMount {
    mount: Mount,
}

// SAFETY: the C VFS is internally synchronized - it owns the locks over its mount,
// handle and tree state - so the operations below are callable from any thread, and
// each one only ever waits on a ticket it launched in that same call (see the type
// docs for why that is sound on a worker, read_prefix's chained pair included).
// What is not thread-safe about a mount is its lifetime, and that is exactly the
// promise Mount::worker_view extracts from its caller. Sync is deliberately not
// implemented: nothing needs to share one view between threads, and without it a
// &Mount (which is !Sync) cannot be reached from another thread through this.
unsafe impl Send for WorkerMount {}

impl Clone for WorkerMount {
    /// Duplicate the view. Sound under the same promise the mint took: the mount
    /// outlives every holder, so a second holder changes nothing.
    fn clone(&self) -> WorkerMount {
        WorkerMount {
            mount: Mount::from_raw_ptr(self.mount.raw),
        }
    }
}

impl Deref for WorkerMount {
    type Target = Mount;

    #[inline]
    fn deref(&self) -> &Mount {
        &self.mount
    }
}

impl WorkerMount {
    /// Read the whole of path, waiting for the bytes.
    pub fn read(&self, path: &str) -> Result<VfsData, FlError> {
        let mount = &self.mount;
        // SAFETY: a live mount (the worker_view promise); path is borrowed for
        // the launch only, and the ticket is closed exactly once below.
        unsafe {
            let ticket = ffi::vfs_mount_read_all_with_options(
                mount.raw,
                sys::RawStr::borrow(path),
                None,
                ptr::null_mut(),
                None,
                FL_VFS_HANDLE_INVALID,
            );
            self.drain_blocking::<VfsData>(ticket)
        }
    }

    /// Read at most max_bytes from the start of path, waiting for the bytes.
    ///
    /// The short read a file smaller than max_bytes gives is not an error - the
    /// returned buffer is however much was there. This is the header-sniff read:
    /// enough of a file to recognise it, without pulling a whole disk image
    /// through the VFS.
    pub fn read_prefix(&self, path: &str, max_bytes: usize) -> Result<Vec<u8>, FlError> {
        if max_bytes == 0 {
            return Ok(Vec::new());
        }
        let mount = &self.mount;
        let mut buffer = vec![0u8; max_bytes];
        // SAFETY: a live mount; path is borrowed for the open only. buffer
        // outlives the wait below, which is what vfs_read's no-copy contract
        // requires, and both tickets are closed on every path out.
        unsafe {
            let file = ffi::vfs_mount_open(mount.raw, sys::RawStr::borrow(path), VFS_OPEN_READ);
            if file == FL_VFS_HANDLE_INVALID {
                return Err(FlError::GenericError);
            }
            let read = ffi::vfs_read(file, buffer.as_mut_ptr().cast::<c_void>(), max_bytes as i64);
            if read == FL_VFS_HANDLE_INVALID {
                ffi::vfs_close(file);
                return Err(FlError::GenericError);
            }
            ffi::vfs_wait(read);
            let data = ffi::vfs_get_data(read);
            let read_bytes = if data.success && data.size > 0 {
                (data.size as usize).min(max_bytes)
            } else if data.success {
                0
            } else {
                ffi::vfs_close(read);
                ffi::vfs_close(file);
                return Err(FlError::GenericError);
            };
            ffi::vfs_close(read);
            ffi::vfs_close(file);
            buffer.truncate(read_bytes);
        }
        Ok(buffer)
    }

    /// List path to depth (1 = immediate children), waiting for the entries.
    pub fn listing(&self, path: &str, depth: i32) -> Result<FileList, FlError> {
        let mount = &self.mount;
        // SAFETY: a live mount; path is borrowed for the launch only, and the
        // ticket is closed exactly once below.
        unsafe {
            let ticket = ffi::vfs_get_listing(mount.raw, sys::RawStr::borrow(path), depth);
            self.drain_blocking::<FileList>(ticket)
        }
    }

    /// Wait for ticket, drain it, and close it.
    ///
    /// # Safety
    /// ticket is a ticket this view just launched, not yet closed.
    unsafe fn drain_blocking<T: Payload>(&self, ticket: FlVfsHandle) -> Result<T, FlError> {
        if ticket == FL_VFS_HANDLE_INVALID {
            return Err(FlError::GenericError);
        }
        // SAFETY: the caller's contract - ticket was just launched by this view and is not yet closed, so
        // it is live for the wait and the drain, and this is the one place it is closed.
        unsafe {
            ffi::vfs_wait(ticket);
            let drained = T::drain(ticket);
            ffi::vfs_close(ticket);
            drained
        }
    }
}

/// The mount factory - the whole of it.
///
/// There is one VFS per process, so there is nothing to construct: this is a
/// namespace, not a handle. Reads and listings are methods on the [`Mount`] it
/// hands back.
pub enum Vfs {}

impl Vfs {
    /// Bring the VFS up against arena and register the built-in LocalFS driver,
    /// reporting whether it came up.
    ///
    /// A host that runs a [`crate::Application`] never calls this - the app's own
    /// bring-up does it. It is here for the host that has none: a test binary or
    /// an embedded runner that wants mounts without a window. Calling it a second
    /// time is refused (false) rather than standing a second VFS over the first.
    ///
    /// arena must outlive every mount opened against it, which in practice means
    /// the process-wide arena [`crate::init`] returned.
    #[inline]
    pub fn init(arena: &crate::Arena) -> bool {
        // SAFETY: the arena outlives the call, and the VFS's own lifetime promise
        // over it is this function's documented contract.
        unsafe { ffi::vfs_init(arena.as_raw()) }
    }

    /// Mount source_path - a directory, an archive, or a URL; which driver handles
    /// it is the VFS's business, not the caller's. Uses the default
    /// [`MountOptions`].
    ///
    /// Mounting is asynchronous: this returns as soon as the request is accepted, and
    /// the mount reports [`MountStatus::Pending`] until its driver resolves it. A
    /// mount that fails later still comes back here as a live handle - poll
    /// [`Mount::status`] for that. The [`Err`] is the request being refused outright.
    pub fn mount(source_path: &str) -> Result<OwnedMount, MountError> {
        Vfs::mount_with(source_path, MountOptions::default())
    }

    /// Mount source_path with explicit options - a cache directory of your own,
    /// revalidation against the server, or file watching from the start.
    pub fn mount_with(
        source_path: &str,
        options: MountOptions<'_>,
    ) -> Result<OwnedMount, MountError> {
        // SAFETY: source_path and options.cache_dir are borrowed for the call
        // only; the C side copies what it keeps.
        let result = unsafe {
            ffi::vfs_mount_with_options(sys::RawStr::borrow(source_path), options.to_sys())
        };
        if let Some(error) = MountError::from_status(result.status) {
            return Err(error);
        }
        // A success status with no mount would be the C side contradicting itself;
        // treat it as the driver having failed rather than handing out a null mount.
        if result.mount.is_null() {
            return Err(MountError::MountFailed);
        }
        Ok(OwnedMount {
            mount: Mount::from_raw_ptr(result.mount),
        })
    }
}
