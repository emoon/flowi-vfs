//! The VFS slot list: every C entry point this crate calls, named once with its
//! signature.
//!
//! [`vfs_slots!`] is an X-macro: it takes the name of another macro and hands it the
//! whole list, so each copy that used to be maintained by hand - the two [`super::ffi`]
//! forward sets, and the fake's field struct, stubs and
//! [`unimplemented()`](super::fake::VfsApi::unimplemented) - is generated from this one
//! place. Adding or changing a slot is an edit here plus `fake_table()`, which stays
//! hand-written because it picks per-test behaviour that is not derivable from a
//! signature.
//!
//! Each entry is `name(arg: Type, ...) -> Ret;`, with the return arrow omitted for a
//! slot that returns nothing. The argument names are load-bearing: the forwarding
//! macros bind and pass them.

macro_rules! vfs_slots {
    ($emit:ident) => {
        $emit! {
            vfs_init(arena: *mut sys::Arena) -> bool;
            vfs_mount_with_options(path: sys::RawStr, options: sys::VfsMountOptions) -> sys::VfsMountResult;
            vfs_mount_close(mount: *mut sys::FlVfsMount);
            vfs_mount_get_job_handle(mount: *mut sys::FlVfsMount) -> sys::JobHandle;
            vfs_mount_is_ready(mount: *mut sys::FlVfsMount) -> bool;
            vfs_mount_get_status(mount: *mut sys::FlVfsMount) -> sys::VfsMountStatus;
            vfs_mount_get_info(mount: *mut sys::FlVfsMount) -> sys::VfsMountInfo;
            vfs_mount_enable_watching(mount: *mut sys::FlVfsMount) -> bool;
            vfs_mount_read_all_with_options(
                mount: *mut sys::FlVfsMount,
                path: sys::RawStr,
                callback: sys::VfsReadCallback,
                user_data: *mut c_void,
                release: sys::VfsReleaseCallback,
                reuse: u32
            ) -> u32;
            vfs_mount_open(mount: *mut sys::FlVfsMount, path: sys::RawStr, flags: u32) -> u32;
            vfs_read(ticket: u32, buffer: *mut c_void, size: i64) -> u32;
            vfs_get_listing(mount: *mut sys::FlVfsMount, path: sys::RawStr, depth: i32) -> u32;
            vfs_is_ready(ticket: u32) -> bool;
            vfs_wait(ticket: u32);
            vfs_get_data(ticket: u32) -> sys::VfsData;
            vfs_get_file_list(ticket: u32) -> sys::VfsFileList;
            vfs_close(ticket: u32);
        }
    };
}

pub(crate) use vfs_slots;
