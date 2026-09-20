//! Every C VFS call this module makes, in one crate-private place.
//!
//! In a normal build each function here is an `#[inline]` forward to the
//! `flowi_vfs_sys::vfs_*` symbol the shared library links - no table, no pointer, no
//! dispatch. Under cfg(test) the same functions route to a per-thread fake
//! instead ([`super::fake`]), which is what lets the mount/ticket plumbing be tested
//! without a live host - including the failure arms (OutOfMemory,
//! NotInitialized) a real VFS cannot be asked to produce.
//!
//! The two builds are the modules below, exactly one of which exists; the
//! re-exports give both the same `ffi::vfs_*` paths.

#[cfg(test)]
pub(crate) use faked::*;
#[cfg(not(test))]
pub(crate) use host::*;

#[cfg(not(test))]
mod host {
    use crate::vfs::*;

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
    host_call!(vfs_mount_read_all_with_options(mount: *mut sys::FlVfsMount, path: sys::RawStr, callback: sys::VfsReadCallback, user_data: *mut c_void, release: sys::VfsReleaseCallback, reuse: u32) -> u32);
    host_call!(vfs_mount_open(mount: *mut sys::FlVfsMount, path: sys::RawStr, flags: u32) -> u32);
    host_call!(vfs_read(ticket: u32, buffer: *mut c_void, size: i64) -> u32);
    host_call!(vfs_get_listing(mount: *mut sys::FlVfsMount, path: sys::RawStr, depth: i32) -> u32);
    host_call!(vfs_is_ready(ticket: u32) -> bool);
    host_call!(vfs_wait(ticket: u32));
    host_call!(vfs_get_data(ticket: u32) -> sys::VfsData);
    host_call!(vfs_get_file_list(ticket: u32) -> sys::VfsFileList);
    host_call!(vfs_close(ticket: u32));
}

// The test build of [`ffi`]: the same calls, dispatched through the per-thread
// table [`fake`] holds.

#[cfg(test)]
mod faked {
    use crate::vfs::*;

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
    fake_call!(vfs_mount_read_all_with_options(mount: *mut sys::FlVfsMount, path: sys::RawStr, callback: sys::VfsReadCallback, user_data: *mut c_void, release: sys::VfsReleaseCallback, reuse: u32) -> u32);
    fake_call!(vfs_mount_open(mount: *mut sys::FlVfsMount, path: sys::RawStr, flags: u32) -> u32);
    fake_call!(vfs_read(ticket: u32, buffer: *mut c_void, size: i64) -> u32);
    fake_call!(vfs_get_listing(mount: *mut sys::FlVfsMount, path: sys::RawStr, depth: i32) -> u32);
    fake_call!(vfs_is_ready(ticket: u32) -> bool);
    fake_call!(vfs_wait(ticket: u32));
    fake_call!(vfs_get_data(ticket: u32) -> sys::VfsData);
    fake_call!(vfs_get_file_list(ticket: u32) -> sys::VfsFileList);
    fake_call!(vfs_close(ticket: u32));
}
