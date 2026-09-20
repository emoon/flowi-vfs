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

#[cfg(test)]
pub(crate) mod fake;
pub(crate) mod ffi;
mod mapped;
mod mount;
mod payload;
mod slots;
#[cfg(test)]
mod tests;

pub use mapped::MappedRead;
pub use mount::{
    Mount, MountError, MountInfo, MountOptions, MountStatus, OwnedMount, Vfs, WorkerMount,
};
pub use payload::{Entry, FileList, VfsData};
