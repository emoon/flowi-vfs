#include "archive_common.h"
#include <archive.h>
#include <archive_entry.h>
#include <core/arena.h>
#include <core/heap.h>
#include <core/path.h>
#include <core/string.h>
#include <flowi/core/log_macros.h>
#include <flowi/core/platform.h>
#include <flowi/core/types.h>
#include <flowi/core/utils.h>
#include <flowi/vfs/vfs_plugin.h>
#include <flowi/vfs/vfs_types.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// LibArchive VFS Plugin
//
// This plugin provides archive format support for the VFS system. It handles:
// - ZIP, TAR, 7Z, RAR, and other formats supported by libarchive
// - File and memory-based archives
// - Directory listing within archives
// - Nested archive support (archives within archives)
//
// The plugin wraps libarchive operations and provides them through the VFS plugin API.

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin-specific handle structure

typedef struct LibArchiveHandle {
    FlString archive_path;       // Path to the archive file itself
    FlString target_path;        // Path within archive (empty for directory operations)
    struct archive* archive;     // libarchive handle
    bool is_directory;           // Whether this handle represents the root directory of archive
    bool is_raw_compression;     // True if this is single-file compression (.gz, .bz2, etc.)
    FlString decompressed_name;  // Filename after stripping compression extension (for raw format)
    bool owns_decompressed_name; // Whether this handle owns decompressed_name (must free in close)
    uint8_t* memory_buffer;      // For memory-based archives (nullptr for file-based)
    uint64_t memory_size;        // Size of memory buffer
    bool owns_memory;            // Whether this handle owns memory_buffer (must free in close)
    int64_t entry_size;          // Cached size when positioned at entry (-1 if unknown)
    bool entry_ready;            // True if archive is positioned at entry for reading

    // Cached directory structure (built on first listing, used for all subsequent listings)
    FlArena* cache_arena;           // Arena for all cached data (nullptr until first listing)
    VfsCachedEntry* cached_entries; // Linked list head
    uint32_t cached_entry_count;    // Number of cached entries
} LibArchiveHandle;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin instance data

typedef struct LibArchivePluginInstance {
    bool initialized;
} LibArchivePluginInstance;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Map libarchive errors to VFS error codes

static FlVfsStatus libarchive_errno_to_vfs_status(int archive_errno) {
    switch (archive_errno) {
        case ARCHIVE_EOF:
            return FlVfsStatus_FileNotFound;
        case ARCHIVE_RETRY:
        case ARCHIVE_WARN:
            return FlVfsStatus_GenericError;
        case ARCHIVE_FAILED:
            return FlVfsStatus_ReadError;
        case ARCHIVE_FATAL:
            return FlVfsStatus_ArchiveError;
        default:
            return FlVfsStatus_GenericError;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper functions

// Check if file is single-file compression (not a multi-file archive like .tar.gz)
// These need special handling with archive_read_support_format_raw()
static bool is_single_file_compression(FlString path) {
    // Must NOT be .tar.* (those are multi-file archives)
    if (path_has_extension(path, S("tar.gz")) || path_has_extension(path, S("tar.xz"))
        || path_has_extension(path, S("tar.lz")) || path_has_extension(path, S("tar.bz2"))
        || path_has_extension(path, S("tar.lzma")) || path_has_extension(path, S("tgz"))
        || path_has_extension(path, S("tbz2")) || path_has_extension(path, S("txz"))) {
        return false;
    }

    // Check for single-file compression extensions
    FlString single_file_exts[] = { S("gz"), S("bz2"), S("xz"), S("lz"), S("lzma"), S("zst") };
    for_count(i, sizeof(single_file_exts) / sizeof(single_file_exts[0])) {
        if (path_has_extension(path, single_file_exts[i])) {
            return true;
        }
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Get filename without compression extension (e.g., "MOD.song.gz" -> "MOD.song")

static FlString get_decompressed_filename(FlString path, FlArena* arena) {
    FlString filename = path_get_filename(path);
    if (filename.length == 0) {
        return filename;
    }

    // Strip known compression extensions (case-insensitive)
    FlString comp_exts[] = { S(".gz"), S(".bz2"), S(".xz"), S(".lz"), S(".lzma"), S(".zst") };
    for_count(i, sizeof(comp_exts) / sizeof(comp_exts[0])) {
        FlString ext = comp_exts[i];
        if (filename.length > ext.length) {
            FlString suffix = string_substr(filename, filename.length - ext.length, ext.length);
            if (string_equals_nocase(suffix, ext)) {
                return string_copy(arena, string_substr(filename, 0, filename.length - ext.length));
            }
        }
    }
    return string_copy(arena, filename);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool is_archive_file(FlString path) {
    // Single-file compression formats are also treated as "archives" (with one entry)
    if (is_single_file_compression(path)) {
        return true;
    }

    FlString extensions[]
        = { S("zip"),    S("tar"),      S("tgz"), S("tar.gz"), S("tar.bz2"), S("tbz2"), S("tar.xz"), S("txz"),
            S("tar.lz"), S("tar.lzma"), S("7z"),  S("rar"),    S("cab"),     S("iso"),  S("cpio") };

    for_count(i, sizeof(extensions) / sizeof(extensions[0])) {
        if (path_has_extension(path, extensions[i])) {
            return true;
        }
    }

    if (path.length > 7) {
        FlString suffix = string_substr(path, path.length - 7, 7);
        if (string_equals(suffix, S(".tar.gz")) || string_equals(suffix, S(".tar.xz"))
            || string_equals(suffix, S(".tar.lz"))) {
            return true;
        }
    }

    if (path.length > 8 && string_equals(string_substr(path, path.length - 8, 8), S(".tar.bz2"))) {
        return true;
    }

    if (path.length > 9 && string_equals(string_substr(path, path.length - 9, 9), S(".tar.lzma"))) {
        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Configure archive format support based on compression type

static void configure_archive_format(struct archive* a, bool is_raw_compression) {
    // For single-file compression, use raw format to get decompressed content as one entry
    if (is_raw_compression) {
        archive_read_support_format_raw(a);
    } else {
        archive_read_support_format_all(a);
    }
    archive_read_support_filter_all(a);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Open archive for reading from file or memory

static struct archive* open_archive_reader(LibArchiveHandle* handle, FlArena* scratch) {
    struct archive* a = archive_read_new();
    configure_archive_format(a, handle->is_raw_compression);

    int r;
    if (handle->memory_buffer) {
        r = archive_read_open_memory(a, handle->memory_buffer, handle->memory_size);
    } else {
        const char* archive_path_cstr = string_to_cstr(scratch, handle->archive_path);
        r = archive_read_open_filename(a, archive_path_cstr, 10240);
    }

    if (r != ARCHIVE_OK) {
        fl_log_error("Failed to open archive: %s", archive_error_string(a));
        archive_read_free(a);
        return nullptr;
    }

    return a;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Find entry in archive, returns entry if found (archive positioned for reading)
// If entry_out is provided and entry found, archive is positioned at entry for reading
// Returns true if entry found, false otherwise

static bool find_archive_entry(struct archive* a, const char* target_cstr, size_t target_len,
                               struct archive_entry** entry_out) {
    struct archive_entry* entry;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char* entry_path = archive_entry_pathname(entry);
        if (!entry_path) {
            archive_read_data_skip(a);
            continue;
        }

        if (vfs_archive_entry_path_matches(entry_path, target_cstr, target_len)) {
            if (entry_out) {
                *entry_out = entry;
            }
            return true;
        }

        archive_read_data_skip(a);
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Forward declarations

static void libarchive_close(void* driver_instance, void* handle_ptr);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Create plugin instance

static FlPluginCreateResult libarchive_create(void) {
    LibArchivePluginInstance* instance = (LibArchivePluginInstance*)heap_alloc(sizeof(LibArchivePluginInstance));
    if (!instance) {
        return (FlPluginCreateResult) { .instance = nullptr, .error = { .vfs_status = FlVfsStatus_OutOfMemory } };
    }

    instance->initialized = true;

    fl_log_info("LibArchive plugin initialized");

    return (FlPluginCreateResult) { .instance = instance, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Destroy plugin instance

static void libarchive_destroy(void* driver_instance) {
    if (!driver_instance) {
        return;
    }

    LibArchivePluginInstance* instance = (LibArchivePluginInstance*)driver_instance;
    heap_free(instance);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Check if LibArchive supports this type of path

static bool libarchive_supports_path(FlString path) {
    return is_archive_file(path);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Check if LibArchive can open a path

static bool libarchive_can_open(FlString path) {
    return is_archive_file(path);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Open archive and seek to target entry, storing state in handle
// Returns FlVfsStatus_Success if entry found, error code otherwise

static FlVfsStatus libarchive_seek_to_entry(LibArchiveHandle* handle, FlArena* scratch) {
    if (handle->entry_ready) {
        return FlVfsStatus_Success; // Already positioned
    }

    if (handle->target_path.length == 0) {
        return FlVfsStatus_PathInvalid;
    }

    // Open archive if not already open (store in handle for persistent use)
    if (!handle->archive) {
        handle->archive = open_archive_reader(handle, scratch);
        if (!handle->archive) {
            return FlVfsStatus_ArchiveError;
        }
    }

    struct archive_entry* entry;

    // For raw compression, there's only one entry ("data") - just seek to first entry
    if (handle->is_raw_compression) {
        if (archive_read_next_header(handle->archive, &entry) == ARCHIVE_OK) {
            handle->entry_size = archive_entry_size(entry); // May be 0 (unknown) for raw format
            handle->entry_ready = true;
            return FlVfsStatus_Success;
        }
        return FlVfsStatus_FileNotFound;
    }

    // Find target entry by name
    const char* target_cstr = string_to_cstr(scratch, handle->target_path);

    if (find_archive_entry(handle->archive, target_cstr, handle->target_path.length, &entry)) {
        handle->entry_size = archive_entry_size(entry);
        handle->entry_ready = true;
        return FlVfsStatus_Success;
    }

    return FlVfsStatus_FileNotFound;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Open archive file

static FlVfsOpenResult libarchive_open(void* driver_instance, void* parent_handle, FlString path, uint32_t flags,
                                       FlVfsMountOptions mount_options) {
    (void)flags;
    (void)mount_options; // LibArchive doesn't use mount options

    // Opening a file inside an already-opened archive
    if (parent_handle) {
        LibArchiveHandle* parent = (LibArchiveHandle*)parent_handle;
        LibArchiveHandle* handle = (LibArchiveHandle*)heap_alloc(sizeof(LibArchiveHandle));
        if (!handle) {
            return (FlVfsOpenResult) { .handle = nullptr, .error = { .vfs_status = FlVfsStatus_OutOfMemory } };
        }
        *handle = (LibArchiveHandle) { 0 };

        handle->archive_path = string_copy_malloc(parent->archive_path);
        handle->target_path = string_copy_malloc(path);
        handle->is_directory = false;
        handle->is_raw_compression = parent->is_raw_compression;
        handle->decompressed_name = parent->decompressed_name; // Shared, no copy needed
        handle->owns_decompressed_name = false;
        handle->memory_buffer = parent->memory_buffer;
        handle->memory_size = parent->memory_size;
        handle->owns_memory = false;
        handle->archive = nullptr;
        handle->entry_size = -1;
        handle->entry_ready = false;
        handle->cache_arena = nullptr;

        // Seek to entry now so we can report errors
        arena_scratch_auto(temp);
        FlVfsStatus status = libarchive_seek_to_entry(handle, temp.arena);
        if (status != FlVfsStatus_Success) {
            libarchive_close(driver_instance, handle);
            return (FlVfsOpenResult) { .handle = nullptr, .error = { .vfs_status = status } };
        }

        return (FlVfsOpenResult) { .handle = handle, .error = { .vfs_status = FlVfsStatus_Success } };
    }

    // Opening the archive file itself
    if (!is_archive_file(path)) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    arena_scratch_auto(temp);

    LibArchiveHandle* handle = (LibArchiveHandle*)heap_alloc(sizeof(LibArchiveHandle));
    if (!handle) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_OutOfMemory } };
    }
    *handle = (LibArchiveHandle) { 0 };

    handle->archive_path = string_copy_malloc(path);
    handle->target_path = S("");
    handle->is_directory = true;
    handle->is_raw_compression = is_single_file_compression(path);
    handle->decompressed_name = handle->is_raw_compression ? get_decompressed_filename(path, temp.arena) : S("");
    handle->owns_decompressed_name = false;
    if (handle->is_raw_compression && handle->decompressed_name.length > 0) {
        handle->decompressed_name = string_copy_malloc(handle->decompressed_name);
        handle->owns_decompressed_name = true;
    }
    handle->memory_buffer = nullptr;
    handle->memory_size = 0;
    handle->owns_memory = false;
    handle->cache_arena = nullptr;

    handle->archive = archive_read_new();
    configure_archive_format(handle->archive, handle->is_raw_compression);

    const char* archive_path_cstr = string_to_cstr(temp.arena, path);

    int r = archive_read_open_filename(handle->archive, archive_path_cstr, 10240);
    if (r != ARCHIVE_OK) {
        FlVfsStatus status = libarchive_errno_to_vfs_status(r);
        int native_error = archive_errno(handle->archive);
        libarchive_close(driver_instance, handle);

        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = status, .native_error = native_error } };
    }

    struct archive_entry* entry;
    r = archive_read_next_header(handle->archive, &entry);
    if (r != ARCHIVE_OK && r != ARCHIVE_EOF) {
        fl_log_debug("Archive validation failed for: %.*s", (int)path.length, path.data);
        libarchive_close(driver_instance, handle);

        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    archive_read_free(handle->archive);
    handle->archive = archive_read_new();
    configure_archive_format(handle->archive, handle->is_raw_compression);

    r = archive_read_open_filename(handle->archive, archive_path_cstr, 10240);
    if (r != ARCHIVE_OK) {
        FlVfsStatus status = libarchive_errno_to_vfs_status(r);
        libarchive_close(driver_instance, handle);

        return (FlVfsOpenResult) { .handle = nullptr, .is_directory = false, .error = { .vfs_status = status } };
    }

    return (FlVfsOpenResult) { .handle = handle, .is_directory = true, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Check if handle is a directory

static bool libarchive_is_directory(void* driver_instance, void* handle) {
    (void)driver_instance;
    LibArchiveHandle* h = (LibArchiveHandle*)handle;
    return h ? h->is_directory : false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Read file contents from archive

static FlVfsReadResult libarchive_read(void* driver_instance, void* handle_ptr, FlString path, uint8_t* buffer,
                                       int64_t size) {
    (void)driver_instance;
    LibArchiveHandle* handle = (LibArchiveHandle*)handle_ptr;

    if (!handle) {
        fl_log_error("Invalid handle for read");
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    // Determine target path
    FlString target_path;
    if (path.length > 0) {
        target_path = path;
    } else if (handle->target_path.length > 0) {
        target_path = handle->target_path;
    } else {
        fl_log_error("No target file specified for read");
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    // If archive is already positioned at entry, read directly
    if (handle->entry_ready && handle->archive) {
        la_ssize_t bytes_read = archive_read_data(handle->archive, buffer, (size_t)size);
        if (bytes_read < 0) {
            fl_log_error("Failed to read from archive entry: %S (libarchive: %s)", handle->target_path,
                         archive_error_string(handle->archive));
            return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_ReadError } };
        }
        return (FlVfsReadResult) { .bytes_read = (int64_t)bytes_read, .error = { .vfs_status = FlVfsStatus_Success } };
    }

    // Fallback: path-based read (reopens archive)
    arena_scratch_auto(temp);

    struct archive* a = open_archive_reader(handle, temp.arena);
    if (!a) {
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    // For raw compression, just seek to first entry
    struct archive_entry* entry = nullptr;
    if (handle->is_raw_compression) {
        if (archive_read_next_header(a, &entry) != ARCHIVE_OK) {
            archive_read_free(a);
            return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_FileNotFound } };
        }
    } else {
        const char* target_cstr = string_to_cstr(temp.arena, target_path);
        if (!find_archive_entry(a, target_cstr, target_path.length, nullptr)) {
            archive_read_free(a);
            return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_FileNotFound } };
        }
    }

    la_ssize_t bytes_read = archive_read_data(a, buffer, (size_t)size);

    if (bytes_read < 0) {
        fl_log_error("Failed to read from archive entry: %.*s", (int)target_path.length, target_path.data);
        archive_read_free(a);

        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_ReadError } };
    }

    archive_read_free(a);

    return (FlVfsReadResult) { .bytes_read = (int64_t)bytes_read, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Get file size in archive

static FlVfsSizeResult libarchive_get_size(void* driver_instance, void* handle_ptr, FlString path) {
    (void)driver_instance;
    LibArchiveHandle* handle = (LibArchiveHandle*)handle_ptr;

    if (!handle) {
        fl_log_error("Invalid handle for get_size");
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    // If entry is ready, return cached size
    if (handle->entry_ready && handle->entry_size >= 0) {
        return (FlVfsSizeResult) { .size = handle->entry_size, .error = { .vfs_status = FlVfsStatus_Success } };
    }

    // Determine target path
    FlString target_path;
    if (path.length > 0) {
        target_path = path;
    } else if (handle->target_path.length > 0) {
        target_path = handle->target_path;
    } else {
        fl_log_error("No target file specified for get_size");
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    arena_scratch_auto(temp);

    struct archive* a = open_archive_reader(handle, temp.arena);
    if (!a) {
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    struct archive_entry* entry = nullptr;

    // For raw compression, just seek to first entry (size may be 0/unknown)
    if (handle->is_raw_compression) {
        if (archive_read_next_header(a, &entry) != ARCHIVE_OK) {
            archive_read_free(a);
            return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_FileNotFound } };
        }
    } else {
        const char* target_cstr = string_to_cstr(temp.arena, target_path);
        if (!find_archive_entry(a, target_cstr, target_path.length, &entry)) {
            archive_read_free(a);
            return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_FileNotFound } };
        }
    }

    int64_t result_size = archive_entry_size(entry);
    archive_read_free(a);

    return (FlVfsSizeResult) { .size = result_size, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Build cache of all archive entries on first directory listing (libarchive is forward-only)

static void build_entry_cache(LibArchiveHandle* handle) {
    if (handle->cache_arena) {
        return;
    }

    handle->cache_arena = arena_create(256 * 1024 * 1024, __FILE__, __LINE__);
    handle->cached_entries = nullptr;
    handle->cached_entry_count = 0;

    VfsCachedEntry* tail = nullptr;

    struct archive_entry* entry;
    while (archive_read_next_header(handle->archive, &entry) == ARCHIVE_OK) {
        FlString path;

        // For raw compression, use the decompressed filename instead of libarchive's "data"
        if (handle->is_raw_compression && handle->decompressed_name.length > 0) {
            path = handle->decompressed_name;
        } else {
            const char* entry_name = archive_entry_pathname(entry);
            if (!entry_name) {
                archive_read_data_skip(handle->archive);
                continue;
            }
            path = string_from_cstr(entry_name);
        }

        if (path.length > 0 && path.data[path.length - 1] == '/') {
            path.length--;
        }

        if (path.length == 0) {
            archive_read_data_skip(handle->archive);
            continue;
        }

        VfsCachedEntry* cached = arena_alloc(handle->cache_arena, VfsCachedEntry);
        cached->path = string_copy(handle->cache_arena, path);
        cached->size = archive_entry_size(entry);
        cached->is_directory = (archive_entry_filetype(entry) == AE_IFDIR);
        cached->next = nullptr;

        if (!handle->cached_entries) {
            handle->cached_entries = cached;
        } else {
            ANALYZER_ASSUME_NONNULL(tail);
            tail->next = cached;
        }
        tail = cached;
        handle->cached_entry_count++;

        archive_read_data_skip(handle->archive);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// List directory contents in archive
//
// Returns immediate children of the given path, consistent with filesystem behavior.
// For recursive extraction, callers should descend into directories manually.

static void libarchive_list_directory(void* driver_instance, void* handle_ptr, FlString path,
                                      FlVfsListingCallback callback, void* user_data) {
    (void)driver_instance;
    LibArchiveHandle* handle = (LibArchiveHandle*)handle_ptr;

    if (!handle || !handle->is_directory) {
        return;
    }

    build_entry_cache(handle);

    vfs_archive_list_children(handle->cached_entries, path, callback, user_data);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Clone handle for concurrent access

static FlVfsOpenResult libarchive_clone(void* driver_instance, void* handle_ptr) {
    LibArchiveHandle* source = (LibArchiveHandle*)handle_ptr;

    if (source->memory_buffer) {
        LibArchiveHandle* handle = (LibArchiveHandle*)heap_alloc(sizeof(LibArchiveHandle));
        if (!handle) {
            return (FlVfsOpenResult) { .handle = nullptr,
                                       .is_directory = false,
                                       .error = { .vfs_status = FlVfsStatus_OutOfMemory } };
        }
        *handle = (LibArchiveHandle) { 0 };

        handle->archive_path = string_copy_malloc(source->archive_path);
        handle->target_path = S("");
        handle->is_directory = true;
        handle->is_raw_compression = source->is_raw_compression;
        handle->decompressed_name = source->decompressed_name; // Shared, no copy needed
        handle->owns_decompressed_name = false;
        handle->memory_buffer = source->memory_buffer;
        handle->memory_size = source->memory_size;
        handle->owns_memory = false;
        handle->cache_arena = nullptr;
        handle->archive = nullptr;

        handle->archive = archive_read_new();
        configure_archive_format(handle->archive, handle->is_raw_compression);

        int r = archive_read_open_memory(handle->archive, (void*)handle->memory_buffer, (size_t)handle->memory_size);
        if (r != ARCHIVE_OK) {
            libarchive_close(driver_instance, handle);
            return (FlVfsOpenResult) { .handle = nullptr,
                                       .is_directory = false,
                                       .error = { .vfs_status = libarchive_errno_to_vfs_status(r) } };
        }

        return (
            FlVfsOpenResult) { .handle = handle, .is_directory = true, .error = { .vfs_status = FlVfsStatus_Success } };
    } else {
        return libarchive_open(driver_instance, nullptr, source->archive_path, 0, (FlVfsMountOptions) { 0 });
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Close archive handle

static void libarchive_close(void* driver_instance, void* handle_ptr) {
    (void)driver_instance;
    LibArchiveHandle* handle = (LibArchiveHandle*)handle_ptr;

    if (!handle) {
        return;
    }

    if (handle->archive) {
        archive_read_free(handle->archive);
        handle->archive = nullptr;
    }

    if (handle->cache_arena) {
        arena_destroy(handle->cache_arena);
    }

    string_free(handle->target_path);
    string_free(handle->archive_path);

    // Only free decompressed_name if this handle owns it (child/cloned handles share it)
    if (handle->owns_decompressed_name) {
        string_free(handle->decompressed_name);
    }

    // Only free memory buffer if this handle owns it
    if (handle->owns_memory && handle->memory_buffer) {
        heap_free(handle->memory_buffer);
    }

    heap_free(handle);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Open archive from memory

static FlVfsOpenResult libarchive_open_memory(void* driver_instance, const uint8_t* data, uint64_t size,
                                              FlString original_path) {
    (void)driver_instance;

    if (!data || size == 0) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    arena_scratch_auto(temp);

    LibArchiveHandle* handle = (LibArchiveHandle*)heap_alloc(sizeof(LibArchiveHandle));
    if (!handle) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_OutOfMemory } };
    }
    *handle = (LibArchiveHandle) { 0 };

    handle->archive_path = string_copy_malloc(original_path);
    handle->target_path = S("");
    handle->is_directory = true;
    handle->is_raw_compression = is_single_file_compression(original_path);
    handle->decompressed_name
        = handle->is_raw_compression ? get_decompressed_filename(original_path, temp.arena) : S("");
    handle->owns_decompressed_name = false;
    if (handle->is_raw_compression && handle->decompressed_name.length > 0) {
        handle->decompressed_name = string_copy_malloc(handle->decompressed_name);
        handle->owns_decompressed_name = true;
    }
    handle->memory_buffer = (uint8_t*)data;
    handle->memory_size = size;
    handle->owns_memory = false;
    handle->cache_arena = nullptr;

    handle->archive = archive_read_new();
    configure_archive_format(handle->archive, handle->is_raw_compression);

    int r = archive_read_open_memory(handle->archive, (void*)data, (size_t)size);
    if (r != ARCHIVE_OK) {
        fl_log_debug("Failed to open archive from memory buffer (size: %llu) - %s", (unsigned long long)size,
                     archive_error_string(handle->archive));
        libarchive_close(driver_instance, handle);
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    struct archive_entry* entry = nullptr;
    r = archive_read_next_header(handle->archive, &entry);
    if (r != ARCHIVE_OK && r != ARCHIVE_EOF) {
        fl_log_debug("Failed to validate archive from memory buffer (size: %llu)", (unsigned long long)size);
        libarchive_close(driver_instance, handle);
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    archive_read_free(handle->archive);
    handle->archive = archive_read_new();
    configure_archive_format(handle->archive, handle->is_raw_compression);

    r = archive_read_open_memory(handle->archive, (void*)data, (size_t)size);
    if (r != ARCHIVE_OK) {
        libarchive_close(driver_instance, handle);
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = libarchive_errno_to_vfs_status(r) } };
    }

    handle->owns_memory = true;

    return (FlVfsOpenResult) { .handle = handle, .is_directory = true, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Get capabilities

static FlVfsDriverCapabilities libarchive_get_capabilities(void) {
    return FlVfsDriverCapabilities_Read | FlVfsDriverCapabilities_List | FlVfsDriverCapabilities_Clone
           | FlVfsDriverCapabilities_Memory | FlVfsDriverCapabilities_Archive;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin definition

static FlVfsPlugin s_libarchive_plugin = {
    .api_version = FL_VFS_PLUGIN_API_VERSION,
    .plugin_name = S_("LibArchive"),
    .plugin_info = nullptr,

    .create = libarchive_create,
    .destroy = libarchive_destroy,
    .supports_path = libarchive_supports_path,
    .can_open = libarchive_can_open,
    .open = libarchive_open,
    .is_directory = libarchive_is_directory,
    .read = libarchive_read,
    .get_size = libarchive_get_size,
    .list_directory = libarchive_list_directory,
    .clone = libarchive_clone,
    .close = libarchive_close,
    .get_capabilities = libarchive_get_capabilities,

    .open_memory = libarchive_open_memory,
    .get_file_path = nullptr,
    .is_cached = nullptr,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Driver accessor

FlVfsPlugin* vfs_driver_libarchive(void) {
    return &s_libarchive_plugin;
}
