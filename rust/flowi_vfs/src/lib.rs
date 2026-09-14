//! Reading files and listing directories through flowi's VFS, safely.
//!
//! [`Vfs`] opens a mount over a directory, an archive or a URL; [`Mount`] carries every
//! operation on it; [`WorkerMount`] is the blocking view a job body reads through. Results
//! arrive as a [`Handle<T>`], a typed ticket that polls to `Result<T, FlError>` and closes
//! itself on drop.
//!
//! The raw ABI is the `flowi_vfs_sys` crate next door; nothing here hands a caller a raw
//! pointer or a lifetime the borrow checker cannot see.

/// `RawStr` at this crate's root, which is where the two modules below reach for it.
/// It is `flowi_core_sys`'s type, re-exported by `flowi_vfs_sys` along with the rest of
/// the foundation's ABI.
pub use flowi_vfs_sys::RawStr;

/// The two foundation types this crate's own signatures name: [`Vfs::init`] takes an
/// `&Arena` to build the VFS state in, and [`Mount::job_handle`] hands back the job its
/// bring-up runs as. They are `flowi_core`'s types, re-exported so a caller reaches them
/// by the same path as the API that needs them.
pub use flowi_core::{Arena, JobHandle};

/// Typed async handles over the C VFS ticket system: an owning `Handle<T>` wraps the
/// u32 ticket, polls it to `Result<T, FlError>`, and closes it non-blockingly on drop;
/// ownership (no Copy/Clone) is what makes recycled-u32 ABA unrepresentable. See the
/// module docs.
mod handle;

/// The typed async-handle wrapper.
pub use handle::{Handle, HandleFuture, Payload, Poll};

/// Reading files and listing directories through flowi's VFS: [`Vfs`] opens a
/// mount, [`Mount`] carries every operation, [`WorkerMount`] is the blocking view a
/// job body reads through. See the module docs.
mod vfs;

/// A single-threaded, frame-ticked local executor for cooperative multi-step async chains:
/// spawn a 'static owning-output async fn, tick it once per frame, drop its [`Task`] to
/// cancel. Its only leaf is [`HandleFuture`], so it lives here beside the handle it drives.
mod executor;

/// The frame-ticked local executor and its owning task handle.
pub use executor::{LocalExecutor, Task};

/// The VFS surface.
pub use vfs::{
    Entry, FileList, MappedRead, Mount, MountError, MountInfo, MountOptions, MountStatus,
    OwnedMount, Vfs, VfsData, WorkerMount,
};

/// The FFI-boundary kit for crates that mirror the VFS's C ABI - plugin SDKs and ABI shims
/// that hand-write extern "C" entry points against the same layouts the C side uses.
///
/// Application code uses this crate's root surface instead: [`Mount`] is the safe handle
/// over the same pointer.
pub mod boundary {
    /// The opaque C FlVfsMount, as it appears behind `*mut FlVfsMount` in hand-written C
    /// signatures. [`Mount`](crate::Mount) is the safe handle over the same pointer -
    /// prefer it.
    pub use flowi_vfs_sys::FlVfsMount;

    /// Compile-time proof that the type this module re-exports is the sys mirror and not a
    /// same-named shadow, named in a pointer position so a sys-side change fails here
    /// rather than at a shim.
    const _: fn(flowi_vfs_sys::FlVfsMount) -> FlVfsMount = |v| v;
    const _: () = {
        const _MOUNT: Option<*mut FlVfsMount> = None;
    };
}
