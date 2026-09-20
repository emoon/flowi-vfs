//! Every C VFS call this module makes, in one crate-private place.
//!
//! In a normal build each function here is an `#[inline]` forward to the
//! `flowi_vfs_sys::vfs_*` symbol the shared library links - no table, no pointer, no
//! dispatch. Under cfg(test) the same functions route to a per-thread fake
//! instead ([`super::fake`]), which is what lets the mount/ticket plumbing be tested
//! without a live host - including the failure arms (OutOfMemory,
//! NotInitialized) a real VFS cannot be asked to produce.
//!
//! Both sets are generated from [`vfs_slots!`](super::slots::vfs_slots), so neither
//! can drift from the other or from the fake. The two builds are the modules below,
//! exactly one of which exists; the re-exports give both the same `ffi::vfs_*` paths.

#[cfg(test)]
pub(crate) use faked::*;
#[cfg(not(test))]
pub(crate) use host::*;

#[cfg(not(test))]
mod host {
    use crate::vfs::slots::vfs_slots;
    use crate::vfs::*;

    /// Forward every VFS slot to its flowi_vfs_sys symbol.
    ///
    /// The generated functions are unsafe because the C symbols are, and they add no invariant of their
    /// own - every argument is passed through untouched. Their callers in this crate are the ones that
    /// establish mount/ticket liveness.
    macro_rules! host_calls {
        ($($name:ident($($arg:ident: $ty:ty),* $(,)?) $(-> $ret:ty)?;)*) => {
            $(
                /// # Safety
                /// The caller must satisfy the C symbol's own contract; this forward imposes nothing further.
                #[inline]
                pub(crate) unsafe fn $name($($arg: $ty),*) $(-> $ret)? {
                    // SAFETY: a pure forward - the caller's contract is the callee's, unchanged.
                    unsafe { sys::$name($($arg),*) }
                }
            )*
        };
    }

    vfs_slots!(host_calls);
}

#[cfg(test)]
mod faked {
    use crate::vfs::slots::vfs_slots;
    use crate::vfs::*;

    /// Forward every VFS slot to the per-thread fake's slot of the same name. Mirrors [`host_calls!`] -
    /// the generated functions add no invariant of their own.
    macro_rules! fake_calls {
        ($($name:ident($($arg:ident: $ty:ty),* $(,)?) $(-> $ret:ty)?;)*) => {
            $(
                /// # Safety
                /// The caller must satisfy the slot's own contract; this forward imposes nothing further.
                #[inline]
                pub(crate) unsafe fn $name($($arg: $ty),*) $(-> $ret)? {
                    // SAFETY: a pure forward - the caller's contract is the fake slot's, unchanged.
                    unsafe { (fake::table().$name)($($arg),*) }
                }
            )*
        };
    }

    vfs_slots!(fake_calls);
}
