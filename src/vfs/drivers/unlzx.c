#include "archive_common.h"
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
#include <unlzx.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// UnLZX VFS Plugin
//
// This plugin provides Amiga LZX archive support using the unlzx library.
// LZX was a popular archive format on the Amiga platform, created by Jonathan Forbes
// and Tomi Poutanen in 1995.
//
// Key features:
// - Memory-based archive support (data must be loaded first)
// - Directory listing within archives
// - File reading from archives
// - Clone support for concurrent access

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin-specific handle structure

typedef struct UnlzxHandle {
    FlString archive_path;  // Path to the archive file itself
    FlString target_path;   // Path within archive (empty for directory operations)
    bool is_directory;      // Whether this handle represents the root directory of archive
    uint8_t* memory_buffer; // For memory-based archives (nullptr for file-based)
    uint64_t memory_size;   // Size of memory buffer
    bool owns_memory;       // Whether this handle owns memory_buffer (must free in close)
    bool owns_archive;      // Whether this handle owns archive (must close in close)
    UnlzxArchive* archive;  // UnLZX archive handle

    // Cached directory structure (built on first listing)
    FlArena* cache_arena;           // Arena for all cached data (nullptr until first listing)
    VfsCachedEntry* cached_entries; // Linked list head
    uint32_t cached_entry_count;    // Number of cached entries
} UnlzxHandle;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin instance data

typedef struct UnlzxPluginInstance {
    bool initialized;
} UnlzxPluginInstance;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Forward declarations

static void unlzx_vfs_close(void* driver_instance, void* handle_ptr);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Check if path has LZX extension

static bool is_lzx_file(FlString path) {
    return path_has_extension(path, S("lzx"));
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Check if data looks like an LZX archive (check magic bytes)

static bool is_lzx_archive_data(const uint8_t* data, size_t size) {
    // LZX header: "LZX" at offset 0
    if (size < 10) {
        return false;
    }
    return (data[0] == 'L' && data[1] == 'Z' && data[2] == 'X');
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Build entry cache

static void build_entry_cache(UnlzxHandle* handle) {
    if (handle->cache_arena || !handle->archive) {
        return;
    }

    handle->cache_arena = arena_create(256 * 1024 * 1024, __FILE__, __LINE__);
    handle->cached_entries = nullptr;
    handle->cached_entry_count = 0;

    VfsCachedEntry* tail = nullptr;

    int entry_count = unlzx_get_entry_count(handle->archive);
    for (int i = 0; i < entry_count; i++) {
        const UnlzxEntry* entry = unlzx_get_entry(handle->archive, i);
        if (!entry || !entry->filename) {
            continue;
        }

        VfsCachedEntry* cached = arena_alloc(handle->cache_arena, VfsCachedEntry);
        cached->path = string_copy(handle->cache_arena, string_from_cstr(entry->filename));
        cached->size = (int64_t)entry->unpack_size;
        // LZX doesn't have explicit directory entries, so check if unpack_size is 0
        cached->is_directory = (entry->unpack_size == 0);
        cached->next = nullptr;

        if (!handle->cached_entries) {
            handle->cached_entries = cached;
        } else {
            ANALYZER_ASSUME_NONNULL(tail);
            tail->next = cached;
        }
        tail = cached;
        handle->cached_entry_count++;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Create plugin instance

static FlPluginCreateResult unlzx_create(void) {
    UnlzxPluginInstance* instance = (UnlzxPluginInstance*)heap_alloc(sizeof(UnlzxPluginInstance));
    instance->initialized = true;

    fl_log_info("UnLZX plugin initialized");

    return (FlPluginCreateResult) { .instance = instance, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Destroy plugin instance

static void unlzx_destroy(void* driver_instance) {
    if (!driver_instance) {
        return;
    }

    UnlzxPluginInstance* instance = (UnlzxPluginInstance*)driver_instance;
    heap_free(instance);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Check if UnLZX supports this type of path

static bool unlzx_supports_path(FlString path) {
    return is_lzx_file(path);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Check if UnLZX can open a path

static bool unlzx_can_open(FlString path) {
    return is_lzx_file(path);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Open archive file

static FlVfsOpenResult unlzx_open(void* driver_instance, void* parent_handle, FlString path, uint32_t flags,
                                  FlVfsMountOptions mount_options) {
    (void)flags;
    (void)mount_options;

    // Opening a file inside an already-opened archive
    if (parent_handle) {
        UnlzxHandle* parent = (UnlzxHandle*)parent_handle;
        UnlzxHandle* handle = (UnlzxHandle*)heap_alloc(sizeof(UnlzxHandle));
        if (!handle) {
            return (FlVfsOpenResult) { .handle = nullptr, .error = { .vfs_status = FlVfsStatus_OutOfMemory } };
        }

        handle->archive_path = string_copy_malloc(parent->archive_path);
        handle->target_path = string_copy_malloc(path);
        handle->is_directory = false;
        handle->memory_buffer = parent->memory_buffer;
        handle->memory_size = parent->memory_size;
        handle->owns_memory = false;
        handle->owns_archive = false; // Share parent's archive, don't close in close()
        handle->archive = parent->archive;
        handle->cache_arena = nullptr;
        handle->cached_entries = nullptr;
        handle->cached_entry_count = 0;

        return (FlVfsOpenResult) { .handle = handle, .error = { .vfs_status = FlVfsStatus_Success } };
    }

    // Opening the archive file itself from file path is not directly supported
    // UnLZX plugin primarily handles memory-based archives from nested archives
    // For direct file access, the archive should be read into memory first
    if (!is_lzx_file(path)) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    // For now, file-based LZX archives should go through memory-based open
    fl_log_debug("UnLZX: Direct file open not supported, use memory-based open");
    return (FlVfsOpenResult) { .handle = nullptr,
                               .is_directory = false,
                               .error = { .vfs_status = FlVfsStatus_NotImplemented } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Check if handle is a directory

static bool unlzx_is_directory(void* driver_instance, void* handle) {
    (void)driver_instance;
    UnlzxHandle* h = (UnlzxHandle*)handle;
    return h ? h->is_directory : false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Resolve target path from handle or explicit path argument

static FlString resolve_target_path(UnlzxHandle* handle, FlString path) {
    if (path.length > 0) {
        return path;
    }
    return handle->target_path;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Read file contents from archive

static FlVfsReadResult unlzx_read(void* driver_instance, void* handle_ptr, FlString path, uint8_t* buffer,
                                  int64_t size) {
    (void)driver_instance;
    UnlzxHandle* handle = (UnlzxHandle*)handle_ptr;

    if (!handle) {
        fl_log_error("Invalid handle for read");
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    if (!handle->archive) {
        fl_log_error("No archive opened");
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    FlString target_path = resolve_target_path(handle, path);
    if (target_path.length == 0) {
        fl_log_error("No target file specified for read");
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    arena_scratch_auto(temp);
    const char* target_cstr = string_to_cstr(temp.arena, target_path);

    const UnlzxEntry* entry = unlzx_find_entry(handle->archive, target_cstr);
    if (!entry) {
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_FileNotFound } };
    }

    if (size < 0 || (uint64_t)size < entry->unpack_size) {
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_GenericError } };
    }

    UnlzxError err = unlzx_extract_entry(handle->archive, entry, buffer, (size_t)size);
    if (err != UNLZX_OK) {
        fl_log_error("UnLZX extraction failed: %d", err);
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    return (FlVfsReadResult) { .bytes_read = (int64_t)entry->unpack_size,
                               .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Get file size in archive

static FlVfsSizeResult unlzx_get_size(void* driver_instance, void* handle_ptr, FlString path) {
    (void)driver_instance;
    UnlzxHandle* handle = (UnlzxHandle*)handle_ptr;

    if (!handle) {
        fl_log_error("Invalid handle for get_size");
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    if (!handle->archive) {
        fl_log_error("No archive opened");
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    FlString target_path = resolve_target_path(handle, path);
    if (target_path.length == 0) {
        fl_log_error("No target file specified for get_size");
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    arena_scratch_auto(temp);
    const char* target_cstr = string_to_cstr(temp.arena, target_path);

    const UnlzxEntry* entry = unlzx_find_entry(handle->archive, target_cstr);
    if (!entry) {
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_FileNotFound } };
    }

    return (FlVfsSizeResult) { .size = (int64_t)entry->unpack_size, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// List directory contents in archive
//
// Returns immediate children of the given path, consistent with filesystem behavior.

static void unlzx_list_directory(void* driver_instance, void* handle_ptr, FlString path, FlVfsListingCallback callback,
                                 void* user_data) {
    (void)driver_instance;
    UnlzxHandle* handle = (UnlzxHandle*)handle_ptr;

    if (!handle || !handle->is_directory) {
        return;
    }

    build_entry_cache(handle);

    vfs_archive_list_children(handle->cached_entries, path, callback, user_data);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Clone handle for concurrent access

static FlVfsOpenResult unlzx_clone(void* driver_instance, void* handle_ptr) {
    UnlzxHandle* source = (UnlzxHandle*)handle_ptr;

    if (!source || !source->memory_buffer) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    UnlzxHandle* handle = (UnlzxHandle*)heap_alloc(sizeof(UnlzxHandle));
    if (!handle) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_OutOfMemory } };
    }

    handle->archive_path = string_copy_malloc(source->archive_path);
    handle->target_path = S("");
    handle->is_directory = true;
    handle->memory_buffer = source->memory_buffer;
    handle->memory_size = source->memory_size;
    handle->owns_memory = false;
    handle->owns_archive = true; // Clone creates its own archive instance
    handle->cache_arena = nullptr;
    handle->cached_entries = nullptr;
    handle->cached_entry_count = 0;

    // Create new archive instance from same memory
    handle->archive = unlzx_open_memory(handle->memory_buffer, (size_t)handle->memory_size);
    if (!handle->archive) {
        unlzx_vfs_close(driver_instance, handle);
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    return (FlVfsOpenResult) { .handle = handle, .is_directory = true, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Close archive handle

static void unlzx_vfs_close(void* driver_instance, void* handle_ptr) {
    (void)driver_instance;
    UnlzxHandle* handle = (UnlzxHandle*)handle_ptr;

    if (!handle) {
        return;
    }

    // Only close the archive if this handle owns it
    // (child handles from unlzx_open share parent's archive, cloned handles own their own)
    if (handle->owns_archive && handle->archive) {
        unlzx_close(handle->archive);
        handle->archive = nullptr;
    }

    if (handle->cache_arena) {
        arena_destroy(handle->cache_arena);
    }

    string_free(handle->target_path);
    string_free(handle->archive_path);

    if (handle->owns_memory && handle->memory_buffer) {
        heap_free(handle->memory_buffer);
    }

    heap_free(handle);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Open archive from memory

static FlVfsOpenResult unlzx_vfs_open_memory(void* driver_instance, const uint8_t* data, uint64_t size,
                                             FlString original_path) {
    (void)driver_instance;

    if (!data || size == 0) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    // Verify this is actually an LZX archive
    if (!is_lzx_archive_data(data, (size_t)size)) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    UnlzxHandle* handle = (UnlzxHandle*)heap_alloc(sizeof(UnlzxHandle));
    if (!handle) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_OutOfMemory } };
    }

    handle->archive_path = string_copy_malloc(original_path);
    handle->target_path = S("");
    handle->is_directory = true;
    handle->memory_buffer = (uint8_t*)data;
    handle->memory_size = size;
    handle->owns_memory = false; // Not owned until the open fully succeeds; host owns data until then
    handle->owns_archive = true; // This handle owns the archive
    handle->cache_arena = nullptr;
    handle->cached_entries = nullptr;
    handle->cached_entry_count = 0;

    handle->archive = unlzx_open_memory(data, (size_t)size);
    if (!handle->archive) {
        fl_log_debug("Failed to open LZX archive from memory (size: %llu)", (unsigned long long)size);
        unlzx_vfs_close(driver_instance, handle);
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    handle->owns_memory = true;

    fl_log_info("Opened LZX archive with unlzx (size: %llu, entries: %d)", (unsigned long long)size,
                unlzx_get_entry_count(handle->archive));

    return (FlVfsOpenResult) { .handle = handle, .is_directory = true, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Get capabilities

static FlVfsDriverCapabilities unlzx_get_capabilities(void) {
    return FlVfsDriverCapabilities_Read | FlVfsDriverCapabilities_List | FlVfsDriverCapabilities_Clone
           | FlVfsDriverCapabilities_Memory | FlVfsDriverCapabilities_Archive;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin definition

static FlVfsPlugin s_unlzx_plugin = {
    .api_version = FL_VFS_PLUGIN_API_VERSION,
    .plugin_name = S_("UnLZX"),
    .plugin_info = nullptr,

    .create = unlzx_create,
    .destroy = unlzx_destroy,
    .supports_path = unlzx_supports_path,
    .can_open = unlzx_can_open,
    .open = unlzx_open,
    .is_directory = unlzx_is_directory,
    .read = unlzx_read,
    .get_size = unlzx_get_size,
    .list_directory = unlzx_list_directory,
    .clone = unlzx_clone,
    .close = unlzx_vfs_close,
    .get_capabilities = unlzx_get_capabilities,

    .open_memory = unlzx_vfs_open_memory,
    .get_file_path = nullptr,
    .is_cached = nullptr,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Driver accessor

FlVfsPlugin* vfs_driver_unlzx(void) {
    return &s_unlzx_plugin;
}
