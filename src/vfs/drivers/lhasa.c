#include "archive_common.h"
#include <core/arena.h>
#include <core/heap.h>
#include <core/path.h>
#include <core/string.h>
#include <flowi/core/log_macros.h>
#include <flowi/core/types.h>
#include <flowi/core/utils.h>
#include <flowi/vfs/vfs_plugin.h>
#include <flowi/vfs/vfs_types.h>
#include <lhasa.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Lhasa VFS Plugin
//
// This plugin provides LHA archive support using the lhasa library. It specifically handles Amiga LHA files
// that libarchive rejects due to attribute byte issues (e.g., attr=0x00).
//
// Key features:
// - File and memory-based archive support
// - Directory listing within archives
// - File reading from archives
// - Clone support for concurrent access

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Memory stream wrapper for lhasa
//
// Provides a memory-backed input stream for lhasa to read LHA archives from memory buffers.

typedef struct LhasaMemoryStream {
    const uint8_t* data;
    size_t size;
    size_t pos;
} LhasaMemoryStream;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int lhasa_mem_read(void* handle, void* buf, size_t buf_len) {
    LhasaMemoryStream* stream = (LhasaMemoryStream*)handle;
    size_t remaining = stream->size - stream->pos;
    size_t to_read = buf_len < remaining ? buf_len : remaining;

    if (to_read > 0) {
        memcpy(buf, stream->data + stream->pos, to_read);
        stream->pos += to_read;
    }

    return (int)to_read;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int lhasa_mem_skip(void* handle, size_t bytes) {
    LhasaMemoryStream* stream = (LhasaMemoryStream*)handle;
    size_t remaining = stream->size - stream->pos;
    size_t to_skip = bytes < remaining ? bytes : remaining;
    stream->pos += to_skip;
    return (to_skip == bytes) ? 1 : 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void lhasa_mem_close(void* handle) {
    // Stream struct is allocated by caller, nothing to free here
    (void)handle;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static LHAInputStreamType s_lhasa_memory_stream_type = {
    .read = lhasa_mem_read,
    .skip = lhasa_mem_skip,
    .close = lhasa_mem_close,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin-specific handle structure

typedef struct LhasaHandle {
    FlString archive_path;         // Path to the archive file itself
    FlString target_path;          // Path within archive (empty for directory operations)
    bool is_directory;             // Whether this handle represents the root directory of archive
    uint8_t* memory_buffer;        // For memory-based archives (nullptr for file-based)
    uint64_t memory_size;          // Size of memory buffer
    bool owns_memory;              // Whether this handle owns memory_buffer (must free in close)
    LhasaMemoryStream* mem_stream; // Memory stream state (for memory-based archives)
    LHAInputStream* stream;        // Lhasa input stream
    LHAReader* reader;             // Lhasa reader

    // Cached directory structure (built on first listing)
    FlArena* cache_arena;           // Arena for all cached data (nullptr until first listing)
    VfsCachedEntry* cached_entries; // Linked list head
    uint32_t cached_entry_count;    // Number of cached entries
} LhasaHandle;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin instance data

typedef struct LhasaPluginInstance {
    bool initialized;
} LhasaPluginInstance;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Forward declarations

static void lhasa_close(void* driver_instance, void* handle_ptr);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Check if path has LHA extension

static bool is_lha_file(FlString path) {
    return path_has_extension(path, S("lha")) || path_has_extension(path, S("lzh"));
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Check if data looks like an LHA archive (check magic bytes)

static bool is_lha_archive_data(const uint8_t* data, size_t size) {
    // LHA header: byte 2 should be '-', byte 3 'l', byte 4 'h' or 'z', byte 6 '-'
    // Format: [header_size][checksum][-lh?-] or [-lz?-]
    if (size < 7) {
        return false;
    }

    // Check for "-lh" or "-lz" at offset 2
    if (data[2] == '-' && data[3] == 'l' && (data[4] == 'h' || data[4] == 'z') && data[6] == '-') {
        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Build full path from LHA header

static size_t build_full_path(char* dest, size_t dest_size, LHAFileHeader* header) {
    const char* filename = header->filename ? header->filename : "";
    const char* path = header->path ? header->path : "";

    size_t path_len = strlen(path);
    size_t filename_len = strlen(filename);
    size_t full_len = path_len + filename_len;

    if (full_len >= dest_size || full_len == 0) {
        dest[0] = '\0';
        return 0;
    }

    if (path_len > 0) {
        memcpy(dest, path, path_len);
    }
    memcpy(dest + path_len, filename, filename_len);
    dest[full_len] = '\0';

    // Remove trailing slash if present
    if (full_len > 0 && dest[full_len - 1] == '/') {
        dest[full_len - 1] = '\0';
        full_len--;
    }

    return full_len;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Create lhasa reader from memory buffer

static bool lhasa_open_from_memory(LhasaHandle* handle) {
    if (!handle->memory_buffer || handle->memory_size == 0) {
        return false;
    }

    // Allocate memory stream state
    LhasaMemoryStream* mem_stream = (LhasaMemoryStream*)heap_alloc(sizeof(LhasaMemoryStream));
    if (!mem_stream) {
        return false;
    }

    mem_stream->data = handle->memory_buffer;
    mem_stream->size = (size_t)handle->memory_size;
    mem_stream->pos = 0;

    LHAInputStream* input = lha_input_stream_new(&s_lhasa_memory_stream_type, mem_stream);
    if (!input) {
        heap_free(mem_stream);
        return false;
    }

    LHAReader* reader = lha_reader_new(input);
    if (!reader) {
        lha_input_stream_free(input);
        heap_free(mem_stream);
        return false;
    }

    handle->mem_stream = mem_stream;
    handle->stream = input;
    handle->reader = reader;

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Reset reader to beginning (recreate from memory)

static bool lhasa_reset_reader(LhasaHandle* handle) {
    if (!handle->memory_buffer || handle->memory_size == 0) {
        return false;
    }

    // Free existing reader/stream
    if (handle->reader) {
        lha_reader_free(handle->reader);
        handle->reader = nullptr;
    }
    if (handle->stream) {
        lha_input_stream_free(handle->stream);
        handle->stream = nullptr;
    }
    if (handle->mem_stream) {
        heap_free(handle->mem_stream);
        handle->mem_stream = nullptr;
    }

    return lhasa_open_from_memory(handle);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Resolve target path from handle or explicit path argument

static FlString resolve_target_path(LhasaHandle* handle, FlString path) {
    if (path.length > 0) {
        return path;
    }
    return handle->target_path;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Find entry in archive by path, returns header if found (reader positioned for reading)
// Returns nullptr if not found

static LHAFileHeader* find_lha_entry(LhasaHandle* handle, FlString target_path) {
    if (!handle->memory_buffer || handle->memory_size == 0) {
        return nullptr;
    }

    if (!lhasa_reset_reader(handle)) {
        return nullptr;
    }

    arena_scratch_auto(temp);
    const char* target_cstr = string_to_cstr(temp.arena, target_path);

    LHAFileHeader* header;
    while ((header = lha_reader_next_file(handle->reader)) != nullptr) {
        char full_path[1024];
        size_t full_len = build_full_path(full_path, sizeof(full_path), header);
        if (full_len == 0) {
            continue;
        }

        if (vfs_archive_entry_path_matches(full_path, target_cstr, target_path.length)) {
            return header;
        }
    }

    return nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Build entry cache

static void build_entry_cache(LhasaHandle* handle) {
    if (handle->cache_arena || !handle->memory_buffer) {
        return;
    }

    // Reset reader to beginning
    if (!lhasa_reset_reader(handle)) {
        return;
    }

    handle->cache_arena = arena_create(256 * 1024 * 1024, __FILE__, __LINE__);
    handle->cached_entries = nullptr;
    handle->cached_entry_count = 0;

    VfsCachedEntry* tail = nullptr;

    LHAFileHeader* header;
    while ((header = lha_reader_next_file(handle->reader)) != nullptr) {
        // Skip fake directory entries
        if (lha_reader_current_is_fake(handle->reader)) {
            continue;
        }

        char full_path[1024];
        size_t full_len = build_full_path(full_path, sizeof(full_path), header);
        if (full_len == 0) {
            continue;
        }

        VfsCachedEntry* cached = arena_alloc(handle->cache_arena, VfsCachedEntry);
        cached->path = string_copy(handle->cache_arena, string_from_cstr(full_path));
        cached->size = (int64_t)header->length;
        // Check if it's a directory (lh0 with zero length)
        cached->is_directory = (header->length == 0 && memcmp(header->compress_method, "-lh0-", 5) == 0);
        cached->next = nullptr;

        if (!handle->cached_entries) {
            handle->cached_entries = cached;
        } else {
            tail->next = cached;
        }
        tail = cached;
        handle->cached_entry_count++;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Create plugin instance

static FlPluginCreateResult lhasa_create(void) {
    LhasaPluginInstance* instance = (LhasaPluginInstance*)heap_alloc(sizeof(LhasaPluginInstance));
    instance->initialized = true;

    fl_log_info("Lhasa plugin initialized");

    return (FlPluginCreateResult) { .instance = instance, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Destroy plugin instance

static void lhasa_destroy(void* driver_instance) {
    if (!driver_instance) {
        return;
    }

    LhasaPluginInstance* instance = (LhasaPluginInstance*)driver_instance;
    heap_free(instance);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Check if Lhasa supports this type of path

static bool lhasa_supports_path(FlString path) {
    return is_lha_file(path);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Check if Lhasa can open a path

static bool lhasa_can_open(FlString path) {
    return is_lha_file(path);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Open archive file

static FlVfsOpenResult lhasa_open(void* driver_instance, void* parent_handle, FlString path, uint32_t flags,
                                  FlVfsMountOptions mount_options) {
    (void)flags;
    (void)mount_options;

    // Opening a file inside an already-opened archive
    if (parent_handle) {
        LhasaHandle* parent = (LhasaHandle*)parent_handle;
        LhasaHandle* handle = (LhasaHandle*)heap_alloc(sizeof(LhasaHandle));
        if (!handle) {
            return (FlVfsOpenResult) { .handle = nullptr, .error = { .vfs_status = FlVfsStatus_OutOfMemory } };
        }

        handle->archive_path = string_copy_malloc(parent->archive_path);
        handle->target_path = string_copy_malloc(path);
        handle->is_directory = false;
        handle->memory_buffer = parent->memory_buffer;
        handle->memory_size = parent->memory_size;
        handle->owns_memory = false;
        handle->mem_stream = nullptr;
        handle->stream = nullptr;
        handle->reader = nullptr;
        handle->cache_arena = nullptr;
        handle->cached_entries = nullptr;
        handle->cached_entry_count = 0;

        return (FlVfsOpenResult) { .handle = handle, .error = { .vfs_status = FlVfsStatus_Success } };
    }

    // Opening the archive file itself from file path is not directly supported
    // Lhasa plugin primarily handles memory-based archives from nested archives
    // For direct file access, the archive should be read into memory first
    if (!is_lha_file(path)) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    // For now, file-based LHA archives should go through libarchive first
    // and fall back to lhasa only for memory-based access
    fl_log_debug("Lhasa: Direct file open not supported, use memory-based open");
    return (FlVfsOpenResult) { .handle = nullptr,
                               .is_directory = false,
                               .error = { .vfs_status = FlVfsStatus_NotImplemented } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Check if handle is a directory

static bool lhasa_is_directory(void* driver_instance, void* handle) {
    (void)driver_instance;
    LhasaHandle* h = (LhasaHandle*)handle;
    return h ? h->is_directory : false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Read file contents from archive

static FlVfsReadResult lhasa_read(void* driver_instance, void* handle_ptr, FlString path, uint8_t* buffer,
                                  int64_t size) {
    (void)driver_instance;
    LhasaHandle* handle = (LhasaHandle*)handle_ptr;

    if (!handle) {
        fl_log_error("Invalid handle for read");
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    FlString target_path = resolve_target_path(handle, path);
    if (target_path.length == 0) {
        fl_log_error("No target file specified for read");
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    LHAFileHeader* header = find_lha_entry(handle, target_path);
    if (!header) {
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_FileNotFound } };
    }

    // Refuse to read into a buffer smaller than the entry. Without this check a
    // partial read would fill the whole buffer and still report Success, so a
    // status-checking caller would treat a silent truncation as a complete read.
    if (size < 0 || (uint64_t)size < header->length) {
        return (FlVfsReadResult) { .bytes_read = -1, .error = { .vfs_status = FlVfsStatus_GenericError } };
    }

    // Read the file data
    size_t total_read = 0;
    size_t to_read = (size_t)size;

    while (total_read < to_read) {
        size_t chunk = lha_reader_read(handle->reader, buffer + total_read, to_read - total_read);
        if (chunk == 0) {
            break;
        }
        total_read += chunk;
    }

    return (FlVfsReadResult) { .bytes_read = (int64_t)total_read, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Get file size in archive

static FlVfsSizeResult lhasa_get_size(void* driver_instance, void* handle_ptr, FlString path) {
    (void)driver_instance;
    LhasaHandle* handle = (LhasaHandle*)handle_ptr;

    if (!handle) {
        fl_log_error("Invalid handle for get_size");
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    FlString target_path = resolve_target_path(handle, path);
    if (target_path.length == 0) {
        fl_log_error("No target file specified for get_size");
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    LHAFileHeader* header = find_lha_entry(handle, target_path);
    if (!header) {
        return (FlVfsSizeResult) { .size = -1, .error = { .vfs_status = FlVfsStatus_FileNotFound } };
    }

    return (FlVfsSizeResult) { .size = (int64_t)header->length, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// List directory contents in archive
//
// Returns immediate children of the given path, consistent with filesystem behavior.

static void lhasa_list_directory(void* driver_instance, void* handle_ptr, FlString path, FlVfsListingCallback callback,
                                 void* user_data) {
    (void)driver_instance;
    LhasaHandle* handle = (LhasaHandle*)handle_ptr;

    if (!handle || !handle->is_directory) {
        return;
    }

    build_entry_cache(handle);

    vfs_archive_list_children(handle->cached_entries, path, callback, user_data);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Clone handle for concurrent access

static FlVfsOpenResult lhasa_clone(void* driver_instance, void* handle_ptr) {
    LhasaHandle* source = (LhasaHandle*)handle_ptr;

    if (!source || !source->memory_buffer) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }

    LhasaHandle* handle = (LhasaHandle*)heap_alloc(sizeof(LhasaHandle));
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
    handle->mem_stream = nullptr;
    handle->stream = nullptr;
    handle->reader = nullptr;
    handle->cache_arena = nullptr;
    handle->cached_entries = nullptr;
    handle->cached_entry_count = 0;

    if (!lhasa_open_from_memory(handle)) {
        lhasa_close(driver_instance, handle);
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    return (FlVfsOpenResult) { .handle = handle, .is_directory = true, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Close archive handle

static void lhasa_close(void* driver_instance, void* handle_ptr) {
    (void)driver_instance;
    LhasaHandle* handle = (LhasaHandle*)handle_ptr;

    if (!handle) {
        return;
    }

    if (handle->reader) {
        lha_reader_free(handle->reader);
        handle->reader = nullptr;
    }
    if (handle->stream) {
        lha_input_stream_free(handle->stream);
        handle->stream = nullptr;
    }
    if (handle->mem_stream) {
        heap_free(handle->mem_stream);
        handle->mem_stream = nullptr;
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

static FlVfsOpenResult lhasa_open_memory(void* driver_instance, const uint8_t* data, uint64_t size,
                                         FlString original_path) {
    (void)driver_instance;

    if (!data || size == 0) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    // Verify this is actually an LHA archive
    if (!is_lha_archive_data(data, (size_t)size)) {
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    LhasaHandle* handle = (LhasaHandle*)heap_alloc(sizeof(LhasaHandle));
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
    handle->mem_stream = nullptr;
    handle->stream = nullptr;
    handle->reader = nullptr;
    handle->cache_arena = nullptr;
    handle->cached_entries = nullptr;
    handle->cached_entry_count = 0;

    if (!lhasa_open_from_memory(handle)) {
        fl_log_debug("Failed to open LHA archive from memory (size: %llu)", (unsigned long long)size);
        lhasa_close(driver_instance, handle);
        return (FlVfsOpenResult) { .handle = nullptr,
                                   .is_directory = false,
                                   .error = { .vfs_status = FlVfsStatus_ArchiveError } };
    }

    handle->owns_memory = true;

    fl_log_info("Opened LHA archive with lhasa (size: %llu)", (unsigned long long)size);

    return (FlVfsOpenResult) { .handle = handle, .is_directory = true, .error = { .vfs_status = FlVfsStatus_Success } };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Get capabilities

static FlVfsDriverCapabilities lhasa_get_capabilities(void) {
    return FlVfsDriverCapabilities_Read | FlVfsDriverCapabilities_List | FlVfsDriverCapabilities_Clone
           | FlVfsDriverCapabilities_Memory | FlVfsDriverCapabilities_Archive;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin definition

static FlVfsPlugin s_lhasa_plugin = {
    .api_version = FL_VFS_PLUGIN_API_VERSION,
    .plugin_name = S_("Lhasa"),
    .plugin_info = nullptr,

    .create = lhasa_create,
    .destroy = lhasa_destroy,
    .supports_path = lhasa_supports_path,
    .can_open = lhasa_can_open,
    .open = lhasa_open,
    .is_directory = lhasa_is_directory,
    .read = lhasa_read,
    .get_size = lhasa_get_size,
    .list_directory = lhasa_list_directory,
    .clone = lhasa_clone,
    .close = lhasa_close,
    .get_capabilities = lhasa_get_capabilities,

    .open_memory = lhasa_open_memory,
    .get_file_path = nullptr,
    .is_cached = nullptr,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Driver accessor

FlVfsPlugin* vfs_driver_lhasa(void) {
    return &s_lhasa_plugin;
}
