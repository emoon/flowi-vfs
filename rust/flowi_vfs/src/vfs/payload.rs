//! The owned results a completed operation hands back: a file's bytes, and a
//! directory listing with a borrowing view over its entries.

use super::*;

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

/// One entry of a completed listing, as owned values. The borrowing view over it is
/// [`Entry`]; this is the storage [`FileList`] keeps.
///
/// `pub(super)` so the test fake can hold the same struct rather than a field-for-field
/// copy of it: the fake's listing storage is exactly what a real listing drains into.
#[derive(Clone, PartialEq, Eq, Debug)]
pub(super) struct OwnedEntry {
    pub(super) name: String,
    pub(super) size: i64,
    pub(super) attributes: u32,
    pub(super) is_directory: bool,
    pub(super) is_archive: bool,
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
