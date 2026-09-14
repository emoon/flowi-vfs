//! The raw C ABI of flowi's VFS: the consumer surface, the driver-plugin vtable, and the
//! two hand-written entry points that have no IDL description.
//!
//! Everything in [`generated`] is single-sourced from `api/*.def` - api_gen emits both the
//! Rust binding here and the real `Fl*` C typedef in `include/flowi/vfs/*.h`. The Rust
//! spelling drops the C prefix (`FlVfsData` -> [`VfsData`]).
//!
//! All of the unsafe in the VFS stack is meant to live at this level or below it; the safe
//! wrapper is the `flowi_vfs` crate next door.

#![allow(non_camel_case_types)]

mod generated;

/// The C foundation's whole ABI, re-exported flat.
///
/// This is a bridge, not a surface of its own: `flowi_core_sys` owns these declarations
/// and documents them. It is a glob for two reasons. The generated bindings in
/// [`generated`] name every type they touch by crate-root path - `crate::Arena`,
/// `crate::RawStr` - so a core type that is not at this crate's root does not resolve; and
/// the library this crate links bundles the foundation's symbols alongside the VFS's, so a
/// caller linking this crate really does reach all of them here.
///
/// The foundation is declared once, in one crate, and linked once: the dependency in
/// Cargo.toml turns flowi_core_sys's native build off precisely so the process does not end
/// up with a second copy of core.
pub use flowi_core_sys::*;

/// The exported `vfs_*` service functions a directly linked consumer calls.
pub use generated::vfs::{
    vfs_close, vfs_get_data, vfs_get_file_list, vfs_get_listing, vfs_is_ready, vfs_mount_close,
    vfs_mount_enable_watching, vfs_mount_get_info, vfs_mount_get_status, vfs_mount_is_ready,
    vfs_mount_open, vfs_mount_read_all_with_options, vfs_mount_with_options, vfs_read, vfs_wait,
};
/// The consumer VFS ABI: the opaque FlVfsMount, the result PODs (VfsData / VfsFileList /
/// VfsMountInfo / VfsMountResult), the VfsMountStatus / VfsLoadPriority /
/// VfsMountErrorStatus enums, and the VfsReadCallback post-processing hook.
pub use generated::vfs::{
    FlVfsMount, VfsData, VfsFileList, VfsLoadPriority, VfsMountErrorStatus, VfsMountInfo,
    VfsMountResult, VfsMountStatus, VfsReadCallback,
};
pub use generated::vfs_listing::VfsListingCallback;
/// The VFS driver-plugin ABI: the VfsPlugin fn-pointer vtable a driver implements plus its
/// result types (VfsOpenResult / VfsReadResult / VfsSizeResult / VfsWriteResult /
/// PluginCreateResult), the shared VfsDriverError, the VfsEntry listing record, and the
/// VfsStatus / VfsDriverCapabilities / VfsOpenFlags enums. VfsPlugin's
/// `Option<extern "C" fn>` slots mirror the C function-pointer members; its RpPluginInfo /
/// FlArena deps are module-local extern-opaque ZSTs.
pub use generated::vfs_plugin::{
    PluginCreateResult, VfsDriverCapabilities, VfsDriverError, VfsEntry, VfsMountOptions,
    VfsOpenFlags, VfsOpenResult, VfsPlugin, VfsReadResult, VfsSizeResult, VfsStatus,
    VfsWriteResult,
};

// The two functions the IDL does not describe: vfs_init takes the arena by pointer and
// vfs_mount_get_job_handle returns a core type, so both are declared by hand here rather
// than generated. They are exported by the same library as everything above.
extern "C" {
    /// Bring the VFS singleton up against arena and register the built-in
    /// LocalFS driver (vfs/vfs.h). False when it is already up, or when the job
    /// system has more workers than the VFS supports.
    pub fn vfs_init(arena: *mut Arena) -> bool;

    /// The job the mount's own bring-up runs as (vfs/vfs.h), so work that reads
    /// through the mount can be scheduled after it rather than polled for
    /// readiness. [`JobHandle::INVALID`] once the mount is up.
    pub fn vfs_mount_get_job_handle(mount: *mut FlVfsMount) -> JobHandle;
}
