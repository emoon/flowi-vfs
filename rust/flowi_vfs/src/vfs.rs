//! Reading files and listing directories through flowi's virtual filesystem.
//!
//! [`Vfs`] is only the mount factory, and there is nothing to construct - one VFS
//! per process, so [`Vfs::mount`] is called straight: it opens a directory, archive
//! or URL and hands back an owning [`OwnedMount`]. Everything you then do with a
//! mount is a method on [`Mount`], which OwnedMount derefs to, so a caller writes
//! mount.read(...) whether it owns the mount or borrowed one.
//!
//! The C VFS is non-blocking: every read or listing hands back a recycled
//! FlVfsHandle (u32) you poll with vfs_is_ready and drain with vfs_get_data /
//! vfs_get_file_list. Two surfaces sit on it:
//!
//! - [`Mount`] is the frame-loop surface. [`Mount::read`], [`Mount::listing`] and
//!   [`Mount::read_with`] launch an operation and return an owning [`Handle`] you
//!   poll once per frame. A failed launch is [`None`]; dropping a handle closes the
//!   C ticket without blocking. See [`crate::handle`].
//! - [`WorkerMount`] is the job-body surface: a Send view of a mount whose read,
//!   read_prefix and listing block and return the finished value.
//!
//! Both hand back owned results. [`VfsData`] copies the file's bytes out and
//! [`FileList`] copies every entry's name out, because the C storage behind a
//! listing dies with its handle and any further VFS call may recycle it. Iterating
//! a FileList yields [`Entry`] views borrowed from the list, so entry names cost
//! nothing to read but cannot outlive the container - the same shape
//! [`crate::FileChanges`] uses.
//!
//! [`Mount`] and the [`Handle`]s it mints are !Send + !Sync; [`WorkerMount`] is the
//! way across.

use core::marker::PhantomData;
use core::ops::Deref;
use core::ptr;
use std::os::raw::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Mutex;

use flowi_vfs_sys as sys;
use flowi_vfs_sys::FlError;

use crate::handle::{Handle, Payload, Poll};
use crate::RawStr;

/// The C FlVfsHandle - a recycled u32 ticket for one async operation. 0 is
/// the invalid sentinel a failed launch returns (FL_VFS_HANDLE_INVALID); the
/// surface maps it to [`None`]. api_gen cannot emit the handle typedef/sentinel
/// (they live in the hand-written vfs_api.h leaf), so the ticket is named here.
type FlVfsHandle = u32;
const FL_VFS_HANDLE_INVALID: FlVfsHandle = 0;

/// FlVfsOpenFlags_Read - the one open mode this surface needs, for the bounded
/// prefix read.
const VFS_OPEN_READ: u32 = 1;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/// Every C VFS call this module makes, in one crate-private place.
///
/// In a normal build each function here is an `#[inline]` forward to the
/// `flowi_vfs_sys::vfs_*` symbol the shared library links - no table, no pointer, no
/// dispatch. Under cfg(test) the same functions route to a per-thread fake
/// instead ([`fake`]), which is what lets the mount/ticket plumbing be tested
/// without a live host - including the failure arms (OutOfMemory,
/// NotInitialized) a real VFS cannot be asked to produce.
#[cfg(not(test))]
pub(crate) mod ffi {
    use super::*;

    /// Forward one VFS slot to its flowi_vfs_sys symbol.
    ///
    /// The generated function is unsafe because the C symbol is, and it adds no invariant of its own -
    /// every argument is passed through untouched. Its callers in this module are the ones that establish
    /// mount/ticket liveness.
    macro_rules! host_call {
        ($name:ident($($arg:ident: $ty:ty),* $(,)?) $(-> $ret:ty)?) => {
            /// # Safety
            /// The caller must satisfy the C symbol's own contract; this forward imposes nothing further.
            #[inline]
            pub(crate) unsafe fn $name($($arg: $ty),*) $(-> $ret)? {
                // SAFETY: a pure forward - the caller's contract is the callee's, unchanged.
                unsafe { sys::$name($($arg),*) }
            }
        };
    }

    host_call!(vfs_init(arena: *mut sys::Arena) -> bool);
    host_call!(vfs_mount_with_options(path: sys::RawStr, options: sys::VfsMountOptions) -> sys::VfsMountResult);
    host_call!(vfs_mount_close(mount: *mut sys::FlVfsMount));
    host_call!(vfs_mount_get_job_handle(mount: *mut sys::FlVfsMount) -> sys::JobHandle);
    host_call!(vfs_mount_is_ready(mount: *mut sys::FlVfsMount) -> bool);
    host_call!(vfs_mount_get_status(mount: *mut sys::FlVfsMount) -> sys::VfsMountStatus);
    host_call!(vfs_mount_get_info(mount: *mut sys::FlVfsMount) -> sys::VfsMountInfo);
    host_call!(vfs_mount_enable_watching(mount: *mut sys::FlVfsMount) -> bool);
    host_call!(vfs_mount_read_all_with_options(mount: *mut sys::FlVfsMount, path: sys::RawStr, callback: sys::VfsReadCallback, user_data: *mut c_void, reuse: u32) -> u32);
    host_call!(vfs_mount_open(mount: *mut sys::FlVfsMount, path: sys::RawStr, flags: u32) -> u32);
    host_call!(vfs_read(ticket: u32, buffer: *mut c_void, size: i64) -> u32);
    host_call!(vfs_get_listing(mount: *mut sys::FlVfsMount, path: sys::RawStr, depth: i32) -> u32);
    host_call!(vfs_is_ready(ticket: u32) -> bool);
    host_call!(vfs_wait(ticket: u32));
    host_call!(vfs_get_data(ticket: u32) -> sys::VfsData);
    host_call!(vfs_get_file_list(ticket: u32) -> sys::VfsFileList);
    host_call!(vfs_close(ticket: u32));
}

/// The test build of [`ffi`]: the same calls, dispatched through the per-thread
/// table [`fake`] holds.
#[cfg(test)]
pub(crate) mod ffi {
    use super::*;

    /// Forward one VFS slot to the per-thread fake's slot of the same name. Mirrors [`host_call!`] - the
    /// generated function adds no invariant of its own.
    macro_rules! fake_call {
        ($name:ident($($arg:ident: $ty:ty),* $(,)?) $(-> $ret:ty)?) => {
            /// # Safety
            /// The caller must satisfy the slot's own contract; this forward imposes nothing further.
            #[inline]
            pub(crate) unsafe fn $name($($arg: $ty),*) $(-> $ret)? {
                // SAFETY: a pure forward - the caller's contract is the fake slot's, unchanged.
                unsafe { (fake::table().$name)($($arg),*) }
            }
        };
    }

    fake_call!(vfs_init(arena: *mut sys::Arena) -> bool);
    fake_call!(vfs_mount_with_options(path: sys::RawStr, options: sys::VfsMountOptions) -> sys::VfsMountResult);
    fake_call!(vfs_mount_close(mount: *mut sys::FlVfsMount));
    fake_call!(vfs_mount_get_job_handle(mount: *mut sys::FlVfsMount) -> sys::JobHandle);
    fake_call!(vfs_mount_is_ready(mount: *mut sys::FlVfsMount) -> bool);
    fake_call!(vfs_mount_get_status(mount: *mut sys::FlVfsMount) -> sys::VfsMountStatus);
    fake_call!(vfs_mount_get_info(mount: *mut sys::FlVfsMount) -> sys::VfsMountInfo);
    fake_call!(vfs_mount_enable_watching(mount: *mut sys::FlVfsMount) -> bool);
    fake_call!(vfs_mount_read_all_with_options(mount: *mut sys::FlVfsMount, path: sys::RawStr, callback: sys::VfsReadCallback, user_data: *mut c_void, reuse: u32) -> u32);
    fake_call!(vfs_mount_open(mount: *mut sys::FlVfsMount, path: sys::RawStr, flags: u32) -> u32);
    fake_call!(vfs_read(ticket: u32, buffer: *mut c_void, size: i64) -> u32);
    fake_call!(vfs_get_listing(mount: *mut sys::FlVfsMount, path: sys::RawStr, depth: i32) -> u32);
    fake_call!(vfs_is_ready(ticket: u32) -> bool);
    fake_call!(vfs_wait(ticket: u32));
    fake_call!(vfs_get_data(ticket: u32) -> sys::VfsData);
    fake_call!(vfs_get_file_list(ticket: u32) -> sys::VfsFileList);
    fake_call!(vfs_close(ticket: u32));
}

/// The per-thread stand-in for the C VFS that [`ffi`] dispatches to under
/// cfg(test). Test-only and crate-private in a cfg(test) build, so it does not
/// exist at all in a shipped one.
#[cfg(test)]
pub(crate) mod fake {
    use super::*;
    use std::cell::RefCell;

    /// One slot per C entry point. Plain fn pointers rather than Options - the
    /// unfilled slots panic instead.
    #[derive(Copy, Clone)]
    pub(crate) struct VfsApi {
        pub(crate) vfs_init: unsafe extern "C" fn(*mut sys::Arena) -> bool,
        pub(crate) vfs_mount_with_options:
            unsafe extern "C" fn(sys::RawStr, sys::VfsMountOptions) -> sys::VfsMountResult,
        pub(crate) vfs_mount_close: unsafe extern "C" fn(*mut sys::FlVfsMount),
        pub(crate) vfs_mount_get_job_handle:
            unsafe extern "C" fn(*mut sys::FlVfsMount) -> sys::JobHandle,
        pub(crate) vfs_mount_is_ready: unsafe extern "C" fn(*mut sys::FlVfsMount) -> bool,
        pub(crate) vfs_mount_get_status:
            unsafe extern "C" fn(*mut sys::FlVfsMount) -> sys::VfsMountStatus,
        pub(crate) vfs_mount_get_info:
            unsafe extern "C" fn(*mut sys::FlVfsMount) -> sys::VfsMountInfo,
        pub(crate) vfs_mount_enable_watching: unsafe extern "C" fn(*mut sys::FlVfsMount) -> bool,
        pub(crate) vfs_mount_read_all_with_options: unsafe extern "C" fn(
            *mut sys::FlVfsMount,
            sys::RawStr,
            sys::VfsReadCallback,
            *mut c_void,
            u32,
        ) -> u32,
        pub(crate) vfs_mount_open:
            unsafe extern "C" fn(*mut sys::FlVfsMount, sys::RawStr, u32) -> u32,
        pub(crate) vfs_read: unsafe extern "C" fn(u32, *mut c_void, i64) -> u32,
        pub(crate) vfs_get_listing:
            unsafe extern "C" fn(*mut sys::FlVfsMount, sys::RawStr, i32) -> u32,
        pub(crate) vfs_is_ready: unsafe extern "C" fn(u32) -> bool,
        pub(crate) vfs_wait: unsafe extern "C" fn(u32),
        pub(crate) vfs_get_data: unsafe extern "C" fn(u32) -> sys::VfsData,
        pub(crate) vfs_get_file_list: unsafe extern "C" fn(u32) -> sys::VfsFileList,
        pub(crate) vfs_close: unsafe extern "C" fn(u32),
    }

    /// Slot fillers for a table under construction: a call the scenario under test
    /// is not supposed to make fails loudly rather than being quietly stubbed out.
    mod unimplemented_slots {
        use super::*;

        macro_rules! unimplemented_slot {
            ($name:ident($($arg:ident: $ty:ty),*) $(-> $ret:ty)?) => {
                pub(super) unsafe extern "C" fn $name($(_: $ty),*) $(-> $ret)? {
                    unimplemented!(concat!("the test's VFS table has no ", stringify!($name)))
                }
            };
        }

        unimplemented_slot!(init(a: *mut sys::Arena) -> bool);
        unimplemented_slot!(mount_with_options(a: sys::RawStr, b: sys::VfsMountOptions) -> sys::VfsMountResult);
        unimplemented_slot!(mount_close(a: *mut sys::FlVfsMount));
        unimplemented_slot!(mount_get_job_handle(a: *mut sys::FlVfsMount) -> sys::JobHandle);
        unimplemented_slot!(mount_is_ready(a: *mut sys::FlVfsMount) -> bool);
        unimplemented_slot!(mount_get_status(a: *mut sys::FlVfsMount) -> sys::VfsMountStatus);
        unimplemented_slot!(mount_get_info(a: *mut sys::FlVfsMount) -> sys::VfsMountInfo);
        unimplemented_slot!(mount_enable_watching(a: *mut sys::FlVfsMount) -> bool);
        unimplemented_slot!(mount_read_all_with_options(a: *mut sys::FlVfsMount, b: sys::RawStr, c: sys::VfsReadCallback, d: *mut c_void, e: u32) -> u32);
        unimplemented_slot!(mount_open(a: *mut sys::FlVfsMount, b: sys::RawStr, c: u32) -> u32);
        unimplemented_slot!(read(a: u32, b: *mut c_void, c: i64) -> u32);
        unimplemented_slot!(get_listing(a: *mut sys::FlVfsMount, b: sys::RawStr, c: i32) -> u32);
        unimplemented_slot!(is_ready(a: u32) -> bool);
        unimplemented_slot!(wait(a: u32));
        unimplemented_slot!(get_data(a: u32) -> sys::VfsData);
        unimplemented_slot!(get_file_list(a: u32) -> sys::VfsFileList);
        unimplemented_slot!(close(a: u32));
    }

    impl VfsApi {
        /// A table every slot of which panics. The base a test builds its fake on,
        /// so it fills in only the calls its scenario actually drives.
        pub(crate) fn unimplemented() -> VfsApi {
            use unimplemented_slots as stub;
            VfsApi {
                vfs_init: stub::init,
                vfs_mount_with_options: stub::mount_with_options,
                vfs_mount_close: stub::mount_close,
                vfs_mount_get_job_handle: stub::mount_get_job_handle,
                vfs_mount_is_ready: stub::mount_is_ready,
                vfs_mount_get_status: stub::mount_get_status,
                vfs_mount_get_info: stub::mount_get_info,
                vfs_mount_enable_watching: stub::mount_enable_watching,
                vfs_mount_read_all_with_options: stub::mount_read_all_with_options,
                vfs_mount_open: stub::mount_open,
                vfs_read: stub::read,
                vfs_get_listing: stub::get_listing,
                vfs_is_ready: stub::is_ready,
                vfs_wait: stub::wait,
                vfs_get_data: stub::get_data,
                vfs_get_file_list: stub::get_file_list,
                vfs_close: stub::close,
            }
        }
    }

    thread_local! {
        /// The table this thread's VFS calls land in. Per-thread so tests running
        /// concurrently cannot see each other's fakes.
        static TABLE: RefCell<VfsApi> = RefCell::new(VfsApi::unimplemented());
    }

    /// Point this thread's VFS calls at api for the rest of the test.
    pub(crate) fn install(api: VfsApi) {
        TABLE.with(|table| *table.borrow_mut() = api);
    }

    /// The table [`ffi`] dispatches through. Copied out rather than borrowed so a
    /// slot is free to re-enter (a read callback that itself calls the VFS).
    pub(crate) fn table() -> VfsApi {
        TABLE.with(|table| *table.borrow())
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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
    fn to_sys(self) -> sys::VfsMountOptions {
        sys::VfsMountOptions {
            cache_dir: sys::RawStr::borrow(self.cache_dir),
            cache_validation: self.cache_validation,
            enable_file_watching: self.enable_file_watching,
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/// Why a mount could not be opened - one variant per non-success
/// FlVfsMountErrorStatus.
///
/// This is the refusal: the request never became a mount. A mount that opens
/// and then fails while its driver resolves it is a live [`Mount`] reporting
/// [`MountStatus::Error`], not an error here.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum MountError {
    /// The source path is empty or malformed.
    InvalidPath,
    /// No VFS driver claimed the source.
    PluginNotFound,
    /// A driver claimed it and failed to open it - missing, unreadable, denied.
    MountFailed,
    /// Allocation failed.
    OutOfMemory,
    /// The VFS has not been initialized.
    NotInitialized,
}

impl core::fmt::Display for MountError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let text = match self {
            MountError::InvalidPath => "invalid source path",
            MountError::PluginNotFound => "no VFS driver for this source",
            MountError::MountFailed => "the driver could not open the source",
            MountError::OutOfMemory => "out of memory",
            MountError::NotInitialized => "the VFS is not initialized",
        };
        f.write_str(text)
    }
}

impl std::error::Error for MountError {}

impl MountError {
    /// Map a mount result's status. Success yields [`None`] - the caller has a
    /// mount pointer to use instead.
    fn from_status(status: sys::VfsMountErrorStatus) -> Option<MountError> {
        match status {
            sys::VfsMountErrorStatus::Success => None,
            sys::VfsMountErrorStatus::InvalidPath => Some(MountError::InvalidPath),
            sys::VfsMountErrorStatus::PluginNotFound => Some(MountError::PluginNotFound),
            sys::VfsMountErrorStatus::MountFailed => Some(MountError::MountFailed),
            sys::VfsMountErrorStatus::OutOfMemory => Some(MountError::OutOfMemory),
            sys::VfsMountErrorStatus::NotInitialized => Some(MountError::NotInitialized),
        }
    }
}

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

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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
    raw: *mut sys::FlVfsMount,
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
                FL_VFS_HANDLE_INVALID,
            )
        };
        self.ticket_handle(ticket)
    }

    /// Launch an async read of path whose bytes are handed to transform on the
    /// worker that completes it, so decode or parse work happens off the frame
    /// loop. The returned [`MappedRead`] polls to whatever transform produced.
    ///
    /// The transform is an ordinary Rust closure, moved into a slot the returned
    /// [`MappedRead`] owns, so it is freed whether the read completes, fails, or is
    /// dropped mid-flight. A panic inside it is caught and surfaced as a failed
    /// read rather than unwinding across the C boundary.
    ///
    /// Reach for it only when the transform is worth moving off the frame loop;
    /// [`Mount::read`] is the plain-bytes read.
    pub fn read_with<T, F>(&self, path: &str, transform: F) -> Option<MappedRead<T>>
    where
        T: Send + 'static,
        F: FnOnce(&[u8]) -> T + Send + 'static,
    {
        let mut slot = Box::new(MapSlot {
            state: Mutex::new(MapState::Pending(Box::new(transform))),
        });
        let user_data: *mut c_void = core::ptr::addr_of_mut!(*slot).cast();
        // SAFETY: a live mount; path is borrowed for the call only. user_data
        // names the boxed slot, which MappedRead below keeps alive for at least
        // as long as the ticket - the trampoline is the only other reader, and it
        // runs before the ticket reports ready.
        let ticket = unsafe {
            ffi::vfs_mount_read_all_with_options(
                self.raw,
                sys::RawStr::borrow(path),
                Some(map_trampoline::<T>),
                user_data,
                FL_VFS_HANDLE_INVALID,
            )
        };
        let handle = self.ticket_handle::<Mapped>(ticket)?;
        Some(MappedRead { handle, slot })
    }

    /// Launch an async listing of path within this mount to depth (1 = immediate
    /// children). [`None`] if the C side refused to start it, otherwise a [`Handle`]
    /// that polls to the listing.
    pub fn listing(&self, path: &str, depth: i32) -> Option<Handle<FileList>> {
        // SAFETY: as read; vfs_get_listing borrows path for the call only.
        let ticket = unsafe { ffi::vfs_get_listing(self.raw, sys::RawStr::borrow(path), depth) };
        self.ticket_handle(ticket)
    }

    /// Wrap a launch's ticket, mapping the invalid sentinel to [`None`].
    #[inline]
    fn ticket_handle<T: Payload>(&self, ticket: FlVfsHandle) -> Option<Handle<T>> {
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

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/// The bytes of a completed file read - an owned copy, so it stays valid after the
/// handle (and thus the C-side buffer) is dropped.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VfsData {
    bytes: Vec<u8>,
}

impl VfsData {
    /// The file contents.
    #[inline]
    pub fn as_slice(&self) -> &[u8] {
        &self.bytes
    }

    /// Length in bytes.
    #[inline]
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    /// Whether the file was empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// Take ownership of the contents.
    #[inline]
    pub fn into_bytes(self) -> Vec<u8> {
        self.bytes
    }
}

impl Payload for VfsData {
    fn drain(ticket: u32) -> Result<VfsData, FlError> {
        // SAFETY: ticket is the live, ready ticket (Payload contract).
        let data = unsafe { ffi::vfs_get_data(ticket) };
        if !data.success {
            // FlVfsData carries only a message, no status code, so a read failure
            // maps to the generic error (message enrichment is a follow-up).
            return Err(FlError::GenericError);
        }
        let bytes = if data.data.is_null() || data.size <= 0 {
            Vec::new()
        } else {
            // SAFETY: on success the C side guarantees size valid bytes at data;
            // we copy them out immediately, before vfs_close frees the buffer.
            unsafe { std::slice::from_raw_parts(data.data, data.size as usize) }.to_vec()
        };
        Ok(VfsData { bytes })
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/// One entry of a completed listing, as owned values. The borrowing view over it is
/// [`Entry`]; this is the storage [`FileList`] keeps.
#[derive(Clone, PartialEq, Eq, Debug)]
struct OwnedEntry {
    name: String,
    size: i64,
    attributes: u32,
    is_directory: bool,
    is_archive: bool,
}

/// A completed directory listing, owning its entries.
///
/// The C listing's storage dies with its handle, and any further VFS call may
/// recycle it, so this copies every entry out - a FileList is valid long after
/// the read that produced it was closed. Iterate it for [`Entry`] views.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct FileList {
    entries: Vec<OwnedEntry>,
    mount_version: u32,
}

impl FileList {
    /// How many entries the listing held.
    #[inline]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Whether the listed directory was empty. A successful listing of an empty
    /// directory is not an error; whether it counts as one is the caller's call.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// The mount version the listing was taken at. A later version means the mount
    /// changed underneath it and the listing is stale.
    #[inline]
    pub fn mount_version(&self) -> u32 {
        self.mount_version
    }

    /// The entries, in listing order.
    pub fn iter(&self) -> impl Iterator<Item = Entry<'_>> + '_ {
        self.entries.iter().map(|raw| Entry { raw })
    }
}

/// One listing entry, borrowed from the [`FileList`] that owns it. The fields are
/// one-for-one with the C FlVfsEntry.
#[derive(Copy, Clone)]
pub struct Entry<'a> {
    raw: &'a OwnedEntry,
}

impl<'a> Entry<'a> {
    /// The entry's name - not a full path.
    #[inline]
    pub fn name(&self) -> &'a str {
        &self.raw.name
    }

    /// Size in bytes; 0 for a directory.
    #[inline]
    pub fn size(&self) -> i64 {
        self.raw.size
    }

    /// Platform-specific attribute bits, as the driver reported them.
    #[inline]
    pub fn attributes(&self) -> u32 {
        self.raw.attributes
    }

    /// Whether the entry is a directory.
    #[inline]
    pub fn is_directory(&self) -> bool {
        self.raw.is_directory
    }

    /// Whether the entry can itself be mounted as an archive (a .zip, say).
    #[inline]
    pub fn is_archive(&self) -> bool {
        self.raw.is_archive
    }
}

impl core::fmt::Debug for Entry<'_> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Entry")
            .field("name", &self.name())
            .field("size", &self.size())
            .field("attributes", &self.attributes())
            .field("is_directory", &self.is_directory())
            .field("is_archive", &self.is_archive())
            .finish()
    }
}

impl Payload for FileList {
    fn drain(ticket: u32) -> Result<FileList, FlError> {
        // SAFETY: as VfsData::drain.
        let list = unsafe { ffi::vfs_get_file_list(ticket) };
        if !list.success {
            return Err(FlError::GenericError);
        }
        let entries = if list.entries.is_null() || list.count == 0 {
            Vec::new()
        } else {
            // SAFETY: on success the C side guarantees count entries at
            // entries; every field is copied out here, before vfs_close frees
            // the listing's storage.
            let raw = unsafe { std::slice::from_raw_parts(list.entries, list.count as usize) };
            raw.iter()
                .map(|entry| OwnedEntry {
                    name: entry.name.into_string(),
                    size: entry.size,
                    attributes: entry.attributes,
                    is_directory: entry.is_directory,
                    is_archive: entry.is_archive,
                })
                .collect()
        };
        Ok(FileList {
            entries,
            mount_version: list.mount_version,
        })
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The closure-transported read.

/// The transform's slot: where [`Mount::read_with`] leaves the closure for the
/// worker, and where the worker leaves the value for the poller.
///
/// The Mutex is not contention control - only one thread ever touches this at a
/// time - it is the release/acquire pair that publishes the worker's write to the
/// thread that polls.
struct MapSlot<T> {
    state: Mutex<MapState<T>>,
}

/// The caller's transform, boxed so the trampoline needs no F type parameter.
type MapFn<T> = Box<dyn FnOnce(&[u8]) -> T + Send>;

/// The slot's three states: waiting to run, holding a value, and emptied.
enum MapState<T> {
    /// The transform, not yet run.
    Pending(MapFn<T>),
    /// What the transform produced.
    Done(T),
    /// The value has been taken, or the transform never ran.
    Taken,
}

/// The VfsReadCallback [`Mount::read_with`] installs: run the caller's closure on
/// the worker that finished the read, and leave its value in the slot.
///
/// The C side's rule is that a callback returning a different data pointer has
/// taken ownership of the buffer. This one returns the pointer it was given
/// unchanged, so the VFS keeps owning and freeing its own buffer - the transform's
/// product never crosses the C boundary at all, it goes into the Rust-owned slot.
extern "C" fn map_trampoline<T>(
    data: *mut c_void,
    size: i64,
    user_data: *mut c_void,
) -> sys::VfsData {
    let failed = |success| sys::VfsData {
        data: data.cast::<u8>(),
        size,
        success,
        error_message: RawStr::EMPTY,
    };
    if user_data.is_null() {
        return failed(false);
    }
    // SAFETY: user_data is the `MapSlot<T>` box read_with handed to exactly this
    // read, kept alive by the MappedRead that owns it; this callback is the only
    // other reader and the VFS invokes it at most once per read.
    let slot = unsafe { &*user_data.cast::<MapSlot<T>>() };

    // Take the closure out before running it, so user code never runs under the
    // lock and a panic inside it cannot poison the slot.
    let Ok(mut guard) = slot.state.lock() else {
        return failed(false);
    };
    let MapState::Pending(transform) = core::mem::replace(&mut *guard, MapState::Taken) else {
        return failed(false);
    };
    drop(guard);

    let bytes = if data.is_null() || size <= 0 {
        &[][..]
    } else {
        // SAFETY: the VFS hands the callback size readable bytes at data.
        unsafe { std::slice::from_raw_parts(data.cast::<u8>(), size as usize) }
    };
    // A panic must not unwind across this extern "C" boundary; a transform that
    // panics is reported as a failed read.
    let Ok(value) = catch_unwind(AssertUnwindSafe(|| transform(bytes))) else {
        log::error!("VFS read transform panicked; reporting the read as failed");
        return failed(false);
    };

    let Ok(mut guard) = slot.state.lock() else {
        return failed(false);
    };
    *guard = MapState::Done(value);
    failed(true)
}

/// An in-flight [`Mount::read_with`]: the read's ticket plus the slot its transform
/// leaves a value in.
///
/// Owning, like [`Handle`]: dropping it closes the C ticket non-blockingly and
/// frees the transform and any value it produced, whether or not the read ever
/// completed. Poll it with [`MappedRead::poll`].
pub struct MappedRead<T> {
    handle: Handle<Mapped>,
    /// Boxed so the address the trampoline was given stays put while this moves.
    slot: Box<MapSlot<T>>,
}

impl<T> MappedRead<T> {
    /// Non-blocking check for completion, yielding the transform's value the first
    /// time it observes the read finish.
    ///
    /// [`Poll::Pending`] while the read runs. A read that failed, or whose
    /// transform failed, yields [`Err`]. Polling again after a [`Poll::Ready`]
    /// panics - the value is yielded exactly once, like a fused future.
    pub fn poll(&mut self) -> Poll<Result<T, FlError>> {
        match self.handle.poll() {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Err(error)) => Poll::Ready(Err(error)),
            Poll::Ready(Ok(Mapped)) => Poll::Ready(self.take_value()),
        }
    }

    /// Take what the transform produced, or the generic error when it left nothing.
    fn take_value(&mut self) -> Result<T, FlError> {
        let Ok(mut guard) = self.slot.state.lock() else {
            return Err(FlError::GenericError);
        };
        match core::mem::replace(&mut *guard, MapState::Taken) {
            MapState::Done(value) => Ok(value),
            MapState::Pending(_) | MapState::Taken => Err(FlError::GenericError),
        }
    }
}

/// The payload of a [`Mount::read_with`] ticket: nothing. The transform's value
/// travels through the Rust-owned slot, so all the ticket carries is whether the
/// read (and the transform) succeeded.
pub(crate) struct Mapped;

impl Payload for Mapped {
    fn drain(ticket: u32) -> Result<Mapped, FlError> {
        // SAFETY: as VfsData::drain.
        let data = unsafe { ffi::vfs_get_data(ticket) };
        if data.success {
            Ok(Mapped)
        } else {
            Err(FlError::GenericError)
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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

#[cfg(test)]
mod tests {
    //! VFS coverage driven by a fake VfsApi table, so the exact FFI-shaped call
    //! paths - mount → vfs_mount_with_options, read → vfs_mount_read_all_with_options, poll →
    //! vfs_is_ready/vfs_get_data, drop → vfs_close - are exercised without a
    //! live host. The fake's per-thread state stands in for the async operation.

    use super::*;
    use core::cell::RefCell;

    /// One entry the fake listing reports.
    #[derive(Clone)]
    struct FakeEntry {
        name: String,
        size: i64,
        attributes: u32,
        is_directory: bool,
        is_archive: bool,
    }

    struct FakeState {
        /// is_ready returns false this many polls, then true.
        polls_until_ready: i32,
        /// Bytes get_data hands back on success.
        data: Vec<u8>,
        /// When set, get_data / get_file_list report failure.
        fail: bool,
        /// When set, the launch calls return the invalid sentinel.
        launch_invalid: bool,
        /// When set, a launch accepts the read callback but does not run it - the
        /// real VFS's "still in flight" state, which is when a read_with slot is
        /// still holding its untouched transform.
        hold_callback: bool,
        /// Entries get_file_list reports.
        entries: Vec<FakeEntry>,
        /// What vfs_mount_with_options reports.
        mount_status: sys::VfsMountErrorStatus,
        /// When set, vfs_mount_with_options hands back a null mount whatever its status.
        mount_null: bool,
        /// How many times vfs_close was called.
        close_count: u32,
        /// How many times vfs_mount_close was called.
        mount_close_count: u32,
        /// How many times vfs_wait was called.
        wait_count: u32,
        /// The arena the last vfs_init was handed.
        init_arena: *mut sys::Arena,
        /// Bytes vfs_read copies into the caller's buffer.
        prefix: Vec<u8>,
        /// The last size vfs_read was asked for.
        prefix_requested: i64,
        /// The names/entries the callback path saw, so a read_with test can
        /// assert the transform ran on the bytes the read produced.
        last_callback_size: i64,
        /// What vfs_mount_get_job_handle reports for the mount.
        mount_job_handle: u64,
    }

    impl Default for FakeState {
        fn default() -> FakeState {
            FakeState {
                polls_until_ready: 0,
                data: Vec::new(),
                fail: false,
                launch_invalid: false,
                hold_callback: false,
                entries: Vec::new(),
                mount_status: sys::VfsMountErrorStatus::Success,
                mount_null: false,
                close_count: 0,
                mount_close_count: 0,
                wait_count: 0,
                init_arena: core::ptr::null_mut(),
                prefix: Vec::new(),
                prefix_requested: 0,
                last_callback_size: -1,
                mount_job_handle: 0,
            }
        }
    }

    thread_local! {
        static FAKE: RefCell<FakeState> = RefCell::new(FakeState::default());
        /// The FlVfsEntry array the fake listing points at. Held per-thread so it
        /// outlives the vfs_get_file_list call that returns a pointer into it -
        /// exactly like the C listing's own storage.
        static FAKE_ENTRIES: RefCell<Vec<sys::VfsEntry>> = const { RefCell::new(Vec::new()) };
    }

    fn reset_fake(state: FakeState) {
        fake::install(fake_table());
        FAKE.with(|f| *f.borrow_mut() = state);
        FAKE_ENTRIES.with(|e| e.borrow_mut().clear());
    }

    fn with_fake<R>(f: impl FnOnce(&mut FakeState) -> R) -> R {
        FAKE.with(|state| f(&mut state.borrow_mut()))
    }

    /// A non-null pointer the fake never dereferences, standing in for a mount.
    fn fake_mount_ptr() -> *mut sys::FlVfsMount {
        core::ptr::NonNull::<u8>::dangling().as_ptr().cast()
    }

    unsafe extern "C" fn fake_mount_with_options(
        _path: sys::RawStr,
        _options: sys::VfsMountOptions,
    ) -> sys::VfsMountResult {
        with_fake(|f| sys::VfsMountResult {
            mount: if f.mount_null || f.mount_status != sys::VfsMountErrorStatus::Success {
                core::ptr::null_mut()
            } else {
                fake_mount_ptr()
            },
            status: f.mount_status,
            error_message: RawStr::EMPTY,
        })
    }

    unsafe extern "C" fn fake_mount_close(_mount: *mut sys::FlVfsMount) {
        with_fake(|f| f.mount_close_count += 1);
    }

    unsafe extern "C" fn fake_mount_is_ready(_mount: *mut sys::FlVfsMount) -> bool {
        true
    }

    unsafe extern "C" fn fake_mount_get_status(
        _mount: *mut sys::FlVfsMount,
    ) -> sys::VfsMountStatus {
        sys::VfsMountStatus::Ready
    }

    unsafe extern "C" fn fake_mount_get_info(_mount: *mut sys::FlVfsMount) -> sys::VfsMountInfo {
        sys::VfsMountInfo {
            path: sys::RawStr::from_static("/fake/source"),
            is_ready: true,
        }
    }

    unsafe extern "C" fn fake_mount_enable_watching(_mount: *mut sys::FlVfsMount) -> bool {
        true
    }

    unsafe extern "C" fn fake_read_all_with_options(
        _mount: *mut sys::FlVfsMount,
        _path: sys::RawStr,
        callback: sys::VfsReadCallback,
        user_data: *mut c_void,
        _reuse: u32,
    ) -> u32 {
        if with_fake(|f| f.launch_invalid) {
            return FL_VFS_HANDLE_INVALID;
        }
        // The real VFS runs the callback on the worker that finished the read, then
        // publishes the result. The fake runs it inline at launch, which exercises
        // the same hand-off with the same ordering guarantees; hold_callback
        // stands in for a read that has not got there yet.
        match callback {
            Some(callback) if !with_fake(|f| f.hold_callback) => {
                let (ptr, size) =
                    with_fake(|f| (f.data.as_ptr() as *mut c_void, f.data.len() as i64));
                // SAFETY: ptr/size describe the fake's live buffer; user_data is the
                // caller's own pointer passed straight back.
                let result = unsafe { callback(ptr, size, user_data) };
                with_fake(|f| {
                    f.last_callback_size = size;
                    f.fail = !result.success;
                });
            }
            _ => {}
        }
        1
    }

    unsafe extern "C" fn fake_mount_open(
        _mount: *mut sys::FlVfsMount,
        _path: sys::RawStr,
        _flags: u32,
    ) -> u32 {
        if with_fake(|f| f.launch_invalid) {
            FL_VFS_HANDLE_INVALID
        } else {
            3
        }
    }

    /// # Safety
    /// buffer must address at least size writable bytes, as the real vfs_read requires.
    unsafe extern "C" fn fake_read(_file: u32, buffer: *mut c_void, size: i64) -> u32 {
        with_fake(|f| {
            f.prefix_requested = size;
            let count = f.prefix.len().min(size.max(0) as usize);
            // SAFETY: the caller's contract gives size writable bytes at buffer, and count is clamped
            // to that; the fixture's own prefix is the disjoint source.
            unsafe {
                core::ptr::copy_nonoverlapping(f.prefix.as_ptr(), buffer.cast::<u8>(), count)
            };
            f.data = f.prefix[..count].to_vec();
        });
        4
    }

    unsafe extern "C" fn fake_get_listing(
        _mount: *mut sys::FlVfsMount,
        _path: sys::RawStr,
        _depth: i32,
    ) -> u32 {
        if with_fake(|f| f.launch_invalid) {
            FL_VFS_HANDLE_INVALID
        } else {
            2
        }
    }

    unsafe extern "C" fn fake_is_ready(_handle: u32) -> bool {
        with_fake(|f| {
            if f.polls_until_ready > 0 {
                f.polls_until_ready -= 1;
                false
            } else {
                true
            }
        })
    }

    unsafe extern "C" fn fake_wait(_handle: u32) {
        with_fake(|f| {
            f.wait_count += 1;
            f.polls_until_ready = 0;
        });
    }

    unsafe extern "C" fn fake_get_data(_handle: u32) -> sys::VfsData {
        with_fake(|f| {
            if f.fail {
                sys::VfsData {
                    data: core::ptr::null_mut(),
                    size: 0,
                    success: false,
                    error_message: RawStr::EMPTY,
                }
            } else {
                sys::VfsData {
                    data: f.data.as_ptr() as *mut u8,
                    size: f.data.len() as i64,
                    success: true,
                    error_message: RawStr::EMPTY,
                }
            }
        })
    }

    unsafe extern "C" fn fake_get_file_list(_handle: u32) -> sys::VfsFileList {
        // Materialise the C entry array into thread-local storage the returned
        // pointer can name, with each name borrowed from the fake state's own
        // String - so the listing's storage lives exactly as long as the fake
        // state does, and is invalidated by a reset_fake the way a real listing
        // is invalidated by its handle closing.
        FAKE.with(|state| {
            let state = state.borrow();
            FAKE_ENTRIES.with(|storage| {
                let mut storage = storage.borrow_mut();
                *storage = state
                    .entries
                    .iter()
                    .map(|entry| sys::VfsEntry {
                        name: sys::RawStr::borrow(&entry.name),
                        size: entry.size,
                        attributes: entry.attributes,
                        is_directory: entry.is_directory,
                        is_archive: entry.is_archive,
                    })
                    .collect();
                sys::VfsFileList {
                    entries: storage.as_mut_ptr(),
                    count: storage.len() as u32,
                    mount_version: 7,
                    success: !state.fail,
                    error_message: RawStr::EMPTY,
                }
            })
        })
    }

    unsafe extern "C" fn fake_close(_handle: u32) {
        with_fake(|f| f.close_count += 1);
    }

    // The struct-update tail is redundant today (the list below covers every slot), but it is what makes a
    // newly added slot inherit the panicking stub instead of needing this fixture edited. That is the
    // reason for the allow.
    #[allow(clippy::needless_update)]
    fn fake_table() -> fake::VfsApi {
        fake::VfsApi {
            vfs_mount_with_options: fake_mount_with_options,
            vfs_mount_close: fake_mount_close,
            vfs_mount_is_ready: fake_mount_is_ready,
            vfs_mount_get_status: fake_mount_get_status,
            vfs_mount_get_info: fake_mount_get_info,
            vfs_mount_enable_watching: fake_mount_enable_watching,
            vfs_mount_read_all_with_options: fake_read_all_with_options,
            vfs_mount_open: fake_mount_open,
            vfs_read: fake_read,
            vfs_get_listing: fake_get_listing,
            vfs_is_ready: fake_is_ready,
            vfs_wait: fake_wait,
            vfs_get_data: fake_get_data,
            vfs_get_file_list: fake_get_file_list,
            vfs_close: fake_close,
            vfs_mount_get_job_handle: fake_mount_get_job_handle,
            vfs_init: fake_init,
            ..fake::VfsApi::unimplemented()
        }
    }

    unsafe extern "C" fn fake_init(arena: *mut sys::Arena) -> bool {
        with_fake(|f| {
            f.init_arena = arena;
            !f.fail
        })
    }

    /// Bring-up passes the caller's arena straight through and reports what the C
    /// says - a refusal (a job system too large for the handle array, say) is a
    /// false the caller has to act on, not something the wrapper swallows.
    #[test]
    fn init_forwards_the_arena_and_reports_refusal() {
        reset_fake(FakeState::default());
        let arena = crate::Arena::new(1 << 16);
        assert!(Vfs::init(&arena));
        assert_eq!(
            with_fake(|f| f.init_arena),
            arena.as_raw(),
            "the caller's arena must reach the C unchanged"
        );

        reset_fake(FakeState {
            fail: true,
            ..Default::default()
        });
        assert!(!Vfs::init(&arena), "a refused bring-up must report false");
    }

    unsafe extern "C" fn fake_mount_get_job_handle(_mount: *mut sys::FlVfsMount) -> sys::JobHandle {
        with_fake(|f| sys::JobHandle(f.mount_job_handle))
    }

    // -----------------------------------------------------------------------
    // Mounting.

    #[test]
    fn mount_maps_every_error_status_to_its_own_variant() {
        for (status, expected) in [
            (
                sys::VfsMountErrorStatus::InvalidPath,
                MountError::InvalidPath,
            ),
            (
                sys::VfsMountErrorStatus::PluginNotFound,
                MountError::PluginNotFound,
            ),
            (
                sys::VfsMountErrorStatus::MountFailed,
                MountError::MountFailed,
            ),
            (
                sys::VfsMountErrorStatus::OutOfMemory,
                MountError::OutOfMemory,
            ),
            (
                sys::VfsMountErrorStatus::NotInitialized,
                MountError::NotInitialized,
            ),
        ] {
            reset_fake(FakeState {
                mount_status: status,
                ..Default::default()
            });
            assert_eq!(
                Vfs::mount("/anything").err(),
                Some(expected),
                "status {status:?}"
            );
        }
    }

    /// The handle is what a caller schedules its reads after, so it must be the
    /// mount's own - not a fresh or invalid one. The zero case is the mount a
    /// driver resolved without scheduling anything, and it has to survive the trip
    /// intact rather than being turned into some "not ready yet" stand-in: the job
    /// system reads 0 as "no dependency", which is exactly right for that mount.
    #[test]
    fn a_mount_reports_its_own_bring_up_job() {
        reset_fake(FakeState {
            mount_job_handle: 0x2a,
            ..Default::default()
        });
        assert_eq!(Vfs::mount("/data").expect("mount").job_handle().0, 0x2a);

        reset_fake(FakeState::default());
        assert_eq!(
            Vfs::mount("/data").expect("mount").job_handle(),
            crate::JobHandle::INVALID,
            "a mount with no async bring-up must report the no-dependency handle"
        );
    }

    #[test]
    fn a_success_status_with_no_mount_is_a_failure_not_a_null_mount() {
        reset_fake(FakeState {
            mount_null: true,
            ..Default::default()
        });
        assert_eq!(Vfs::mount("/anything").err(), Some(MountError::MountFailed));
    }

    #[test]
    fn a_successful_mount_closes_exactly_once_on_drop() {
        reset_fake(FakeState::default());
        {
            let mount = Vfs::mount("/data").expect("mount");
            assert_eq!(mount.source_path(), "/fake/source");
            assert_eq!(
                mount.info(),
                MountInfo {
                    path: "/fake/source".to_string(),
                    is_ready: true
                }
            );
            assert_eq!(mount.status(), MountStatus::Ready);
            assert!(mount.is_ready());
            assert_eq!(with_fake(|f| f.mount_close_count), 0);
        }
        assert_eq!(with_fake(|f| f.mount_close_count), 1);
    }

    #[test]
    fn a_leaked_mount_is_never_closed_and_is_shareable() {
        reset_fake(FakeState::default());
        let leaked: &'static Mount = Vfs::mount("/data").expect("mount").leak();
        let (first, second) = (leaked, leaked);
        assert_eq!(first.source_path(), second.source_path());
        assert_eq!(with_fake(|f| f.mount_close_count), 0);
    }

    #[test]
    fn mount_with_forwards_its_options() {
        reset_fake(FakeState::default());
        let options = MountOptions {
            cache_dir: "/tmp/cache",
            cache_validation: true,
            enable_file_watching: true,
        };
        let lowered = options.to_sys();
        assert!(lowered.cache_validation);
        assert!(lowered.enable_file_watching);
        assert_eq!(lowered.cache_dir.into_string(), "/tmp/cache");
        assert!(Vfs::mount_with("/data", options).is_ok());
    }

    // -----------------------------------------------------------------------
    // The frame-loop surface.

    #[test]
    fn read_polls_pending_then_completes() {
        reset_fake(FakeState {
            polls_until_ready: 2,
            data: b"hello".to_vec(),
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");

        let mut handle = mount.read("foo.txt").expect("launch");
        assert!(matches!(handle.poll(), Poll::Pending));
        assert!(matches!(handle.poll(), Poll::Pending));
        match handle.poll() {
            Poll::Ready(Ok(data)) => assert_eq!(data.as_slice(), b"hello"),
            other => panic!("expected ready-ok, got {other:?}"),
        }

        drop(handle);
        assert_eq!(with_fake(|f| f.close_count), 1);
    }

    #[test]
    fn read_failure_propagates() {
        reset_fake(FakeState {
            fail: true,
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        let mut handle = mount.read("missing.txt").expect("launch");
        assert_eq!(handle.poll(), Poll::Ready(Err(FlError::GenericError)));
    }

    #[test]
    fn invalid_launch_is_none_not_sentinel() {
        reset_fake(FakeState::default());
        let mount = Vfs::mount("/data").expect("mount");
        with_fake(|f| f.launch_invalid = true);
        assert!(mount.read("foo.txt").is_none());
        assert!(mount.listing("/", 1).is_none());
        assert!(mount.read_with("foo.txt", |bytes| bytes.len()).is_none());
    }

    #[test]
    #[should_panic(expected = "handle polled after completion")]
    fn poll_after_ready_panics() {
        reset_fake(FakeState {
            data: b"x".to_vec(),
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        let mut handle = mount.read("foo.txt").expect("launch");
        assert!(matches!(handle.poll(), Poll::Ready(Ok(_))));
        let _ = handle.poll();
    }

    #[test]
    fn drop_before_completion_closes_without_blocking() {
        // is_ready would never fire (the op stays pending); if Drop join-spun on
        // completion this test would hang. It returns, and the ticket is closed.
        reset_fake(FakeState {
            polls_until_ready: i32::MAX,
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        let mut handle = mount.read("slow.txt").expect("launch");
        assert!(matches!(handle.poll(), Poll::Pending));
        drop(handle);
        assert_eq!(with_fake(|f| f.close_count), 1);
    }

    #[test]
    fn read_empty_data_completes_without_dereferencing_null() {
        // Empty file: size == 0 (with a non-null dangling data pointer, as an
        // empty Vec yields). The poll must take the zero-length branch - never
        // calling from_raw_parts on that pointer - and yield an empty VfsData.
        reset_fake(FakeState::default());
        let mount = Vfs::mount("/data").expect("mount");
        let mut handle = mount.read("empty.txt").expect("launch");
        match handle.poll() {
            Poll::Ready(Ok(data)) => {
                assert!(data.is_empty());
                assert_eq!(data.len(), 0);
            }
            other => panic!("expected ready-ok, got {other:?}"),
        }
    }

    // -----------------------------------------------------------------------
    // Owning listings.

    fn entry(name: &str, is_directory: bool) -> FakeEntry {
        FakeEntry {
            name: name.to_string(),
            size: if is_directory { 0 } else { 42 },
            attributes: 0x8000,
            is_directory,
            is_archive: name.ends_with(".zip"),
        }
    }

    #[test]
    fn a_listing_owns_its_entries_and_outlives_the_close() {
        reset_fake(FakeState {
            entries: vec![entry("sub", true), entry("game.zip", false)],
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");

        let list = {
            let mut handle = mount.listing(".", 1).expect("launch");
            match handle.poll() {
                Poll::Ready(Ok(list)) => list,
                other => panic!("expected ready-ok, got {other:?}"),
            }
            // handle drops here: the C ticket is closed and its entry storage is
            // gone. The list below must still read cleanly.
        };
        assert_eq!(with_fake(|f| f.close_count), 1);
        // Overwrite the fake's entry storage the way a recycled listing would.
        reset_fake(FakeState {
            entries: vec![entry("unrelated", false)],
            ..Default::default()
        });

        assert_eq!(list.len(), 2);
        assert!(!list.is_empty());
        assert_eq!(list.mount_version(), 7);
        let names: Vec<&str> = list.iter().map(|entry| entry.name()).collect();
        assert_eq!(names, ["sub", "game.zip"]);
        let second = list.iter().nth(1).expect("second entry");
        assert_eq!(second.size(), 42);
        assert_eq!(second.attributes(), 0x8000);
        assert!(!second.is_directory());
        assert!(second.is_archive());
        assert!(list.iter().next().expect("first entry").is_directory());
    }

    #[test]
    fn an_empty_listing_succeeds_with_no_entries() {
        reset_fake(FakeState::default());
        let mount = Vfs::mount("/data").expect("mount");
        let mut handle = mount.listing(".", 1).expect("launch");
        match handle.poll() {
            Poll::Ready(Ok(list)) => {
                assert!(list.is_empty());
                assert_eq!(list.len(), 0);
                assert_eq!(list.iter().count(), 0);
            }
            other => panic!("expected ready-ok, got {other:?}"),
        }
    }

    #[test]
    fn listing_failure_propagates() {
        reset_fake(FakeState {
            fail: true,
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        let mut handle = mount.listing("/", 1).expect("launch");
        assert_eq!(handle.poll(), Poll::Ready(Err(FlError::GenericError)));
    }

    #[test]
    fn listing_drop_closes_ticket() {
        reset_fake(FakeState {
            entries: vec![entry("one", false)],
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        let handle = mount.listing("/", 1).expect("launch");
        drop(handle);
        assert_eq!(with_fake(|f| f.close_count), 1);
    }

    // -----------------------------------------------------------------------
    // The closure-transported read.

    #[test]
    fn read_with_runs_the_closure_on_the_read_bytes() {
        reset_fake(FakeState {
            data: b"one\ntwo\n".to_vec(),
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");

        // A transform that produces an owned, non-Copy value: what it hands back
        // has to survive the trip through the slot intact.
        let mut read = mount
            .read_with("list.txt", |bytes| {
                String::from_utf8_lossy(bytes)
                    .lines()
                    .map(str::to_string)
                    .collect::<Vec<String>>()
            })
            .expect("launch");
        match read.poll() {
            Poll::Ready(Ok(lines)) => assert_eq!(lines, ["one", "two"]),
            other => panic!("expected the transformed value, got {other:?}"),
        }
        assert_eq!(with_fake(|f| f.last_callback_size), 8);
        drop(read);
        assert_eq!(with_fake(|f| f.close_count), 1);
    }

    #[test]
    fn read_with_reports_a_panicking_transform_as_a_failed_read() {
        reset_fake(FakeState {
            data: b"junk".to_vec(),
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        let mut read = mount
            .read_with("list.txt", |_bytes| -> u32 { panic!("transform blew up") })
            .expect("launch");
        assert_eq!(read.poll(), Poll::Ready(Err(FlError::GenericError)));
    }

    #[test]
    fn read_with_dropped_mid_flight_frees_its_slot_and_closes() {
        reset_fake(FakeState {
            polls_until_ready: i32::MAX,
            hold_callback: true,
            data: b"x".to_vec(),
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        // The transform captures an Arc, so the strong count proves the closure
        // (and with it the slot) was freed rather than leaked.
        let witness = std::sync::Arc::new(());
        let captured = std::sync::Arc::clone(&witness);
        let mut read = mount
            .read_with("slow.txt", move |_bytes| {
                let _ = &captured;
            })
            .expect("launch");
        assert!(matches!(read.poll(), Poll::Pending));
        assert_eq!(std::sync::Arc::strong_count(&witness), 2);
        drop(read);
        assert_eq!(std::sync::Arc::strong_count(&witness), 1);
        assert_eq!(with_fake(|f| f.close_count), 1);
    }

    #[test]
    #[should_panic(expected = "handle polled after completion")]
    fn read_with_yields_its_value_exactly_once() {
        reset_fake(FakeState {
            data: b"x".to_vec(),
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        let mut read = mount
            .read_with("list.txt", |bytes| bytes.len())
            .expect("launch");
        assert_eq!(read.poll(), Poll::Ready(Ok(1)));
        let _ = read.poll();
    }

    // -----------------------------------------------------------------------
    // The worker surface.

    #[test]
    fn a_worker_view_reads_blocking_on_the_thread_that_holds_it() {
        reset_fake(FakeState {
            // Never ready by polling: only the fake's wait flips it ready, which is
            // what makes this exercise the blocking surface rather than a poll.
            polls_until_ready: i32::MAX,
            data: b"payload".to_vec(),
            entries: vec![entry("a", false), entry("b", true)],
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        // SAFETY: the mount outlives every use of the view below.
        let view = unsafe { mount.worker_view() };

        let data = view.read("file.bin").expect("blocking read");
        assert_eq!(data.as_slice(), b"payload");
        let list = view.listing(".", 1).expect("blocking listing");
        assert_eq!(list.len(), 2);
        assert_eq!(with_fake(|f| f.wait_count), 2);
        assert_eq!(with_fake(|f| f.close_count), 2);

        // The queries reached through Deref are the same ones Mount carries.
        assert_eq!(view.source_path(), "/fake/source");

        // And the view is Send: a job body can own one. That it works once
        // moved is the next test.
        fn assert_send<T: Send>(_: &T) {}
        assert_send(&view);
        let cloned = view.clone();
        assert_send(&cloned);
    }

    #[test]
    fn a_worker_view_moved_to_another_thread_reads_through_it() {
        // The point of WorkerMount being Send: a job body on a worker thread
        // owns one and drives the blocking operations from there. The test above
        // only proves the type is Send; this proves a moved view is usable.
        //
        // The fake table is per-thread, so the spawned thread installs its own -
        // standing in for the real C VFS, which is process-wide and needs no such
        // step. What is under test is the view, not the table.
        reset_fake(FakeState {
            polls_until_ready: i32::MAX,
            data: b"from the worker".to_vec(),
            entries: vec![entry("a", false), entry("b", true)],
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        // SAFETY: the mount outlives the joined thread below, so it is open for
        // every use of the view.
        let view = unsafe { mount.worker_view() };

        let worker = std::thread::spawn(move || {
            reset_fake(FakeState {
                polls_until_ready: i32::MAX,
                data: b"from the worker".to_vec(),
                entries: vec![entry("a", false), entry("b", true)],
                ..Default::default()
            });
            let data = view.read("file.bin").expect("blocking read on the worker");
            let list = view
                .listing(".", 1)
                .expect("blocking listing on the worker");
            // The mount queries reached through Deref work here too.
            let source = view.source_path().to_string();
            (
                data.into_bytes(),
                list.len(),
                source,
                with_fake(|f| f.wait_count),
            )
        });

        let (bytes, count, source, waits) = worker.join().expect("the worker thread panicked");
        assert_eq!(bytes, b"from the worker");
        assert_eq!(count, 2);
        assert_eq!(source, "/fake/source");
        // Both operations reached their wait on that thread rather than polling.
        assert_eq!(waits, 2);
    }

    #[test]
    fn a_blocking_read_of_a_missing_file_is_an_error_not_a_hang() {
        reset_fake(FakeState {
            launch_invalid: true,
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        // SAFETY: the mount outlives the view.
        let view = unsafe { mount.worker_view() };
        assert_eq!(view.read("missing").unwrap_err(), FlError::GenericError);
        assert_eq!(
            view.listing("missing", 1).unwrap_err(),
            FlError::GenericError
        );
        assert_eq!(
            view.read_prefix("missing", 16).unwrap_err(),
            FlError::GenericError
        );
        // Nothing was launched, so nothing was waited on or closed.
        assert_eq!(with_fake(|f| f.wait_count), 0);
        assert_eq!(with_fake(|f| f.close_count), 0);
    }

    #[test]
    fn read_prefix_bounds_the_read_and_keeps_a_short_file_whole() {
        reset_fake(FakeState {
            prefix: b"0123456789".to_vec(),
            ..Default::default()
        });
        let mount = Vfs::mount("/data").expect("mount");
        // SAFETY: the mount outlives the view.
        let view = unsafe { mount.worker_view() };

        // Asked for less than the file holds: exactly that many bytes come back.
        assert_eq!(view.read_prefix("blob.bin", 4).expect("prefix"), b"0123");
        assert_eq!(with_fake(|f| f.prefix_requested), 4);
        // Asked for more: the short read is the whole file, not an error.
        assert_eq!(
            view.read_prefix("blob.bin", 64).expect("prefix"),
            b"0123456789"
        );
        // Both prefix reads open a file handle and chain a read onto it, so each
        // closes two tickets.
        assert_eq!(with_fake(|f| f.close_count), 4);
        // A zero-byte request never reaches the VFS at all.
        assert_eq!(view.read_prefix("blob.bin", 0).expect("prefix"), b"");
        assert_eq!(with_fake(|f| f.close_count), 4);
    }
}
