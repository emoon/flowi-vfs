// VFS Operations
// File and directory operations (read, write, list, open)

#include <core/error_report.h>
#include <core/file_watcher.h>
#include <core/log.h>
#include <core/memory.h>
#include <core/path.h>
#include <core/profile.h>
#include <core/sprintf.h>
#include <core/string.h>
#include <core/jobsys.h>
#include "match.h"
#include "vfs_private.h"
#include <ctype.h>

// Maximum file size for read operations (512 MB)
#define VFS_MAX_READ_SIZE (512 * 1024 * 1024)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Failure idiom for file operations: log the error, then publish the status with release semantics.
// A macro (not a function) so the log line keeps the call site's file/line. The caller returns.

#define vfs_op_fail(handle, status, fmt, ...)                                         \
    do {                                                                              \
        logc_error(VFS_ID, fmt, ##__VA_ARGS__);                                       \
        atomic_store_explicit(&(handle)->error_status, status, memory_order_release); \
    } while (0)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// File operations

void vfs_op_file_open(VfsState* self, VfsHandleData* handle, FlArena* scratch, int thread_index) {
    profile_function_auto_nc("vfs:vfs_op_file_open", PROFILE_COLOR_CYAN);
    FlVfsMount* mount = handle->mount;
    FlString path = handle->path;

    atomic_store_explicit(&handle->result_data, nullptr, memory_order_relaxed);

    if (!mount || !mount->root_node || !mount->root_node->plugin_entry) {
        vfs_op_fail(handle, VFS_ERROR_MOUNT, "No correct plugin set up for VFS");
        return;
    }

    // Walk the tree to resolve through nested mounts and containers
    TreeWalkResult walk_result = vfs_tree_walk_and_resolve(self, mount, path, scratch, thread_index, false);

    if (!walk_result.success || !walk_result.plugin_entry) {
        vfs_op_fail(handle, VFS_ERROR_FILE_NOT_FOUND, "Failed to resolve path: %S", path);
        return;
    }

    VfsPluginEntry* plugin_entry = walk_result.plugin_entry;

    u32 open_flags = atomic_load(&handle->open_flags);
    FlVfsOpenResult open_result = plugin_entry->plugin->open(plugin_entry->plugin_instance, walk_result.plugin_handle,
                                                             walk_result.path_from_node, open_flags, mount->options);

    if (!open_result.handle) {
        vfs_op_fail(handle, VFS_ERROR_FILE_NOT_FOUND, "Failed to open file: %S", path);
        return;
    }

    // Store resolved plugin info and file handle for subsequent operations
    handle->plugin_entry = plugin_entry;
    handle->plugin_handle = walk_result.plugin_handle;
    handle->plugin_file_handle = open_result.handle;
    atomic_store_explicit(&handle->error_status, VFS_STATE_SUCCESS, memory_order_release);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_op_file_write(VfsState* self, VfsHandleData* handle, FlArena* scratch, int thread_index) {
    profile_function_auto_nc("vfs:vfs_op_file_write", PROFILE_COLOR_CYAN);

    UNUSED(self);
    UNUSED(scratch);
    UNUSED(thread_index);

    atomic_store_explicit(&handle->result_data, nullptr, memory_order_relaxed);

    VfsHandleData* file_handle = vfs_get_handle_data(handle->file_handle);
    if (!file_handle || file_handle->op_type != VfsOp_FileOpen) {
        vfs_op_fail(handle, VFS_ERROR_DRIVER, "Invalid file handle for write operation");
        return;
    }

    if (!file_handle->plugin_file_handle) {
        vfs_op_fail(handle, VFS_ERROR_DRIVER, "File not opened");
        return;
    }

    // Use the resolved plugin_entry from the file open operation
    VfsPluginEntry* plugin_entry = file_handle->plugin_entry;
    if (!plugin_entry || !plugin_entry->plugin->write) {
        vfs_op_fail(handle, VFS_ERROR_PERMISSION, "Plugin does not support write operations");
        return;
    }

    FlVfsWriteResult write_result = plugin_entry->plugin->write(
        plugin_entry->plugin_instance, file_handle->plugin_file_handle, handle->write_data, handle->write_size);

    if (write_result.bytes_written != handle->write_size) {
        vfs_op_fail(handle, VFS_ERROR_DRIVER, "Write operation failed or incomplete (wrote %ld of %ld bytes)",
                    write_result.bytes_written, handle->write_size);
        return;
    }

    FlVfsData* ret_data = mi_alloc_zero(FlVfsData);
    ret_data->size = write_result.bytes_written;
    ret_data->success = true;

    atomic_store_explicit(&handle->result_data, ret_data, memory_order_release);
    atomic_store_explicit(&handle->error_status, VFS_STATE_SUCCESS, memory_order_release);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_op_file_read(VfsState* self, VfsHandleData* handle, FlArena* scratch, int thread_index) {
    profile_function_auto_nc("vfs:vfs_op_file_read", PROFILE_COLOR_CYAN);

    UNUSED(self);
    UNUSED(scratch);
    UNUSED(thread_index);

    atomic_store_explicit(&handle->result_data, nullptr, memory_order_relaxed);

    VfsHandleData* file_handle = vfs_get_handle_data(handle->file_handle);
    if (!file_handle || file_handle->op_type != VfsOp_FileOpen) {
        vfs_op_fail(handle, VFS_ERROR_DRIVER, "Invalid file handle for read operation");
        return;
    }

    if (!file_handle->plugin_file_handle) {
        vfs_op_fail(handle, VFS_ERROR_DRIVER, "File not opened");
        return;
    }

    if (!handle->read_buffer || handle->read_size <= 0) {
        vfs_op_fail(handle, VFS_ERROR_DRIVER, "Invalid read buffer or size");
        return;
    }

    // Use the resolved plugin_entry from the file open operation
    VfsPluginEntry* plugin_entry = file_handle->plugin_entry;
    if (!plugin_entry || !plugin_entry->plugin->read) {
        vfs_op_fail(handle, VFS_ERROR_PERMISSION, "Plugin does not support read operations");
        return;
    }

    // Read data - empty path since file is already opened
    FlVfsReadResult read_result
        = plugin_entry->plugin->read(plugin_entry->plugin_instance, file_handle->plugin_file_handle, (FlString) { 0 },
                                     handle->read_buffer, handle->read_size);

    if (read_result.bytes_read < 0) {
        vfs_op_fail(handle, VFS_ERROR_READ, "Read operation failed");
        return;
    }

    handle->bytes_read = read_result.bytes_read;

    FlVfsData* ret_data = mi_alloc_zero(FlVfsData);
    ret_data->data = handle->read_buffer;
    ret_data->size = read_result.bytes_read;
    ret_data->success = true;

    atomic_store_explicit(&handle->result_data, ret_data, memory_order_release);
    atomic_store_explicit(&handle->error_status, VFS_STATE_SUCCESS, memory_order_release);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// A failed read still invokes the callback, with data == nullptr and size == 0, so it can release whatever its
// user_data owns and settle its own state. It runs before the result is published so a poller cannot close the
// handle out from under it, and any data it returns is dropped - a failed result has no reader for it.
static void read_all_notify_failure(VfsHandleData* handle) {
    if (handle->callback) {
        handle->callback(nullptr, 0, handle->user_data);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void read_all_set_error(VfsHandleData* handle, FlVfsData* result, const FlVfsPlugin* plugin, void* instance,
                               void* file_handle, u8* buffer, const char* error_fmt) {
    if (buffer) {
        mi_free(buffer);
    }
    result->error_message = vfs_format_error_message(error_fmt, handle->path);
    plugin->close(instance, file_handle);
    handle->plugin_file_handle = nullptr;
    read_all_notify_failure(handle);
    atomic_store_explicit(&handle->result_data, result, memory_order_release);
    atomic_store_explicit(&handle->error_status, VFS_ERROR_READ, memory_order_release);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Monolithic read-all operation: reuses vfs_op_file_open, then reads entire file and closes

static void vfs_op_read_all(VfsState* self, VfsHandleData* handle, FlArena* scratch, int thread_index) {
    profile_function_auto_nc("vfs:vfs_op_read_all", PROFILE_COLOR_CYAN);

    FlVfsData* result = mi_alloc_zero(FlVfsData);

    // Note: vfs_op_file_open clears result_data, so we store result after it returns
    atomic_store(&handle->open_flags, FlVfsOpenFlags_Read);
    vfs_op_file_open(self, handle, scratch, thread_index);

    VfsOpStatus open_status = atomic_load_explicit(&handle->error_status, memory_order_acquire);
    if (open_status != VFS_STATE_SUCCESS) {
        result->error_message = vfs_format_error_message("Failed to read '%S': file not found", handle->path);
        read_all_notify_failure(handle);
        atomic_store_explicit(&handle->result_data, result, memory_order_release);
        return;
    }

    const FlVfsPlugin* plugin = handle->plugin_entry->plugin;
    void* file_handle = handle->plugin_file_handle;
    void* instance = handle->plugin_entry->plugin_instance;

    FlVfsSizeResult size_result = plugin->get_size(instance, file_handle, (FlString) { 0 });
    if (size_result.size < 0) {
        read_all_set_error(handle, result, plugin, instance, file_handle, nullptr, "Failed to get size: %S");
        return;
    }

    // Enforce the cap before allocating: an over-cap file is a hard read error, never a truncated success.
    if (size_result.size > VFS_MAX_READ_SIZE) {
        read_all_set_error(handle, result, plugin, instance, file_handle, nullptr, "File exceeds max read size: %S");
        return;
    }

    u8* buffer = mi_malloc(size_result.size > 0 ? size_result.size : 1);
    if (!buffer) {
        read_all_set_error(handle, result, plugin, instance, file_handle, nullptr, "Out of memory: %S");
        return;
    }

    // Cooperative cancellation before the potentially large, non-interruptible known-size read starts.
    if (vfs_should_cancel(handle)) {
        read_all_set_error(handle, result, plugin, instance, file_handle, buffer, "Read canceled: %S");
        atomic_store_explicit(&handle->error_status, VFS_ERROR_CANCELED, memory_order_release);
        return;
    }

    if (size_result.size > 0) {
        FlVfsReadResult read_result = plugin->read(instance, file_handle, (FlString) { 0 }, buffer, size_result.size);
        if (read_result.bytes_read < 0) {
            read_all_set_error(handle, result, plugin, instance, file_handle, buffer, "Read failed: %S");
            return;
        }
        result->data = buffer;
        result->size = read_result.bytes_read;
    } else {
        // Size is 0/unknown (e.g., gzip raw compression) - read in chunks until EOF
        mi_free(buffer);

        i64 capacity = 64 * 1024;
        i64 total_read = 0;
        buffer = mi_malloc(capacity);

        if (!buffer) {
            read_all_set_error(handle, result, plugin, instance, file_handle, nullptr, "Out of memory: %S");
            return;
        }

        bool reached_eof = false;

        while (total_read < VFS_MAX_READ_SIZE) {
            // Cooperative cancellation: bail between chunks so a large streaming read stops early.
            if (vfs_should_cancel(handle)) {
                read_all_set_error(handle, result, plugin, instance, file_handle, buffer, "Read canceled: %S");
                atomic_store_explicit(&handle->error_status, VFS_ERROR_CANCELED, memory_order_release);
                return;
            }

            i64 chunk_size = capacity - total_read;
            if (chunk_size <= 0) {
                capacity *= 2;
                if (capacity > VFS_MAX_READ_SIZE) {
                    capacity = VFS_MAX_READ_SIZE;
                }
                u8* new_buffer = mi_realloc(buffer, capacity);
                if (!new_buffer) {
                    read_all_set_error(handle, result, plugin, instance, file_handle, buffer, "Out of memory: %S");
                    return;
                }
                buffer = new_buffer;
                chunk_size = capacity - total_read;
            }

            FlVfsReadResult read_result
                = plugin->read(instance, file_handle, (FlString) { 0 }, buffer + total_read, chunk_size);
            if (read_result.bytes_read < 0) {
                read_all_set_error(handle, result, plugin, instance, file_handle, buffer, "Read failed: %S");
                return;
            }

            if (read_result.bytes_read == 0) {
                reached_eof = true;
                break;
            }

            total_read += read_result.bytes_read;
        }

        if (!reached_eof) {
            // The loop stopped at the cap. Probe one byte to tell an exactly-cap-sized file (EOF now)
            // from a larger one, which must fail hard instead of returning truncated data as success.
            u8 probe;
            FlVfsReadResult probe_result = plugin->read(instance, file_handle, (FlString) { 0 }, &probe, 1);
            if (probe_result.bytes_read < 0) {
                read_all_set_error(handle, result, plugin, instance, file_handle, buffer, "Read failed: %S");
                return;
            }
            if (probe_result.bytes_read > 0) {
                read_all_set_error(handle, result, plugin, instance, file_handle, buffer,
                                   "File exceeds max read size: %S");
                return;
            }
        }

        result->data = buffer;
        result->size = total_read;
    }

    plugin->close(instance, file_handle);
    handle->plugin_file_handle = nullptr;

    if (handle->callback) {
        FlVfsData callback_result = handle->callback((void*)result->data, result->size, handle->user_data);
        if (callback_result.data != result->data) {
            mi_free((void*)result->data);
            result->data = callback_result.data;
            handle->callback_owns_data = true;
        }
        result->size = callback_result.size;
        result->success = callback_result.success;
    } else {
        result->success = true;
    }

    atomic_store_explicit(&handle->result_data, result, memory_order_release);
    atomic_store_explicit(&handle->error_status, VFS_STATE_SUCCESS, memory_order_release);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Directory listing

typedef struct {
    VfsTreeNode* parent_node;  // Tree node being listed
    FlVfsMount* mount;         // For tree_lock
    VfsState* vfs_state;       // VFS state for archive detection
    void* plugin_instance;     // Plugin instance for get_file_path
    void* plugin_handle;       // Plugin handle for get_file_path
    const FlVfsPlugin* plugin; // Plugin for get_file_path
    VfsHandleData* handle;     // Owning op handle, for cooperative cancellation
} ListCallbackData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool vfs_ops_check_if_archive(VfsState* self, FlString file_path) {
    FL_VALIDATE_RET(self != nullptr, false);

    // Try each plugin to see if it can open this as a directory (archives are navigable)
    for (VfsPluginEntry* entry = self->plugin_first; entry != nullptr; entry = entry->next) {
        const FlVfsPlugin* plugin = entry->plugin;

        if (!plugin->can_open(file_path)) {
            continue;
        }

        FlVfsOpenResult open_result
            = plugin->open(entry->plugin_instance, nullptr, file_path, 0, (FlVfsMountOptions) { 0 });
        if (!open_result.handle) {
            continue;
        }

        bool is_dir = open_result.is_directory;
        plugin->close(entry->plugin_instance, open_result.handle);

        if (is_dir) {
            return true;
        }
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool vfs_dir_entry_callback(void* user_data, const FlVfsEntry* entry_ptr) {
    profile_function_auto_nc("vfs:vfs_dir_entry_callback", PROFILE_COLOR_CYAN);
    ListCallbackData* data = user_data;

    if (vfs_should_cancel(data->handle)) {
        return false; // stop listing
    }

    FlVfsEntry entry = { .name = entry_ptr->name,
                         .size = entry_ptr->size,
                         .attributes = entry_ptr->attributes,
                         .is_directory = entry_ptr->is_directory,
                         .is_archive = entry_ptr->is_archive };

    // Skip "." and ".." directory entries to prevent infinite recursion
    if (string_equals(entry.name, S(".")) || string_equals(entry.name, S(".."))) {
        return true; // Continue listing
    }

    // For LocalFS mounts: check if non-directory files are archives
    if (!entry.is_directory && !entry.is_archive && data->plugin && data->plugin->get_file_path) {
        if (string_equals(data->plugin->plugin_name, S("LocalFS"))) {
            arena_scratch_auto(temp);
            FlString file_path
                = data->plugin->get_file_path(data->plugin_instance, data->plugin_handle, entry.name, temp.arena);
            if (file_path.length > 0) {
                entry.is_archive = vfs_ops_check_if_archive(data->vfs_state, file_path);
            }
        }
    }

    // Create/update tree node (in mount's persistent storage) in sorted order
    mutex_lock(&data->mount->tree_lock);

    VfsTreeNode* child = vfs_tree_find_child_node(data->parent_node, entry.name);
    if (!child) {
        child = vfs_tree_create_node(data->mount, entry.name);
        child->is_directory = entry.is_directory;
        child->is_archive = entry.is_archive;
        child->size = entry.size;
        child->attributes = entry.attributes;
        vfs_tree_add_child_node(data->parent_node, child);
        // Driver not loaded yet - happens lazily when node is accessed
    } else {
        child->size = entry.size;
        child->attributes = entry.attributes;
    }

    mutex_unlock(&data->mount->tree_lock);
    return true; // Continue listing
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Directory listing operation

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Failure path for directory listing. Destroys list_arena when non-null (the earliest failures happen before
// it exists). Logging stays at the call site since the message and level differ per failure.

static void vfs_list_fail(VfsHandleData* handle, FlArena* list_arena, VfsOpStatus status, const char* fmt,
                          FlString path) {
    atomic_store_explicit(&handle->error_status, status, memory_order_release);

    FlVfsFileList* error_result = mi_alloc_zero(FlVfsFileList);
    error_result->success = false;
    error_result->error_message = vfs_format_error_message(fmt, path);
    atomic_store_explicit(&handle->result_data, error_result, memory_order_release);

    if (list_arena) {
        arena_destroy(list_arena);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_op_list(VfsState* self, VfsHandleData* handle, FlArena* scratch, int thread_index) {
    profile_function_auto_nc("vfs:vfs_op_list", PROFILE_COLOR_CYAN);
    FlVfsMount* mount = handle->mount;
    FlString path = handle->path;

    atomic_store_explicit(&handle->result_data, nullptr, memory_order_relaxed);

    if (!mount || !mount->root_node || !mount->root_node->plugin_entry) {
        logc_error(VFS_ID, "No correct plugin set up for VFS mount list");
        vfs_list_fail(handle, nullptr, VFS_ERROR_MOUNT, "Failed to list '%S': Mount not ready", path);
        return;
    }

    // Dedicated arena for listing (destroyed on vfs_close)
    FlArena* list_arena = arena_new();

    VfsTreeNode* list_node = nullptr;
    {
        TreeWalkResult walk = vfs_tree_walk_and_resolve(self, mount, path, scratch, thread_index, true);

        if (!walk.success || !walk.plugin_entry) {
            logc_info(VFS_ID, "mount: %S : Directory not found %S", mount->source_path, path);
            vfs_list_fail(handle, list_arena, VFS_ERROR_PATH, "Failed to list '%S': Path not found", path);
            return;
        }

        if (!walk.plugin_entry->plugin->list_directory) {
            logc_error(VFS_ID, "Plugin %S does not support directory listing", walk.plugin_entry->plugin->plugin_name);
            vfs_list_fail(handle, list_arena, VFS_ERROR_DRIVER, "Failed to list '%S': Plugin does not support listing",
                          path);
            return;
        }

        ListCallbackData cb_data = {
            .parent_node = walk.node,
            .mount = mount,
            .vfs_state = self,
            .plugin_instance = walk.plugin_entry->plugin_instance,
            .plugin_handle = walk.plugin_handle,
            .plugin = walk.plugin_entry->plugin,
            .handle = handle,
        };

        // Plugin callback populates tree in sorted order
        walk.plugin_entry->plugin->list_directory(walk.plugin_entry->plugin_instance, walk.plugin_handle,
                                                  walk.path_from_node, vfs_dir_entry_callback, &cb_data);

        list_node = walk.node;
    }

    // Cooperative cancellation: if the listing was canceled (enumeration stopped early or cancel arrived
    // after it finished), report it rather than returning a partial/complete result the caller abandoned.
    if (vfs_should_cancel(handle)) {
        vfs_list_fail(handle, list_arena, VFS_ERROR_CANCELED, "Listing of '%S' canceled", path);
        return;
    }

    FlString needle = handle->filter_needle;
    bool has_filter = needle.length > 0;

    // Convert needle to null-terminated lowercase for fzy if filtering
    char* needle_cstr = nullptr;
    if (has_filter) {
        needle_cstr = arena_alloc_array(scratch, char, needle.length + 1);
        for (u64 i = 0; i < needle.length; i++) {
            needle_cstr[i] = (char)tolower((unsigned char)needle.data[i]);
        }
        needle_cstr[needle.length] = '\0';
    }

    // Snapshot of one child node. Node pointers must not be cached across the unlock below:
    // the file watcher can free nodes as soon as the tree lock is released, so everything
    // needed for filtering and for building the result is copied out while the lock is held.
    typedef struct {
        FlString name; // Deep copy in scratch; node-owned storage may be freed after unlock
        i64 size;
        u32 attributes;
        bool is_directory;
        bool is_archive;
    } NodeInfo;

    typedef struct {
        NodeInfo* info;
        score_t score;
    } ScoredNode;

    // First pass: snapshot all child nodes under the lock
    u32 total_count = 0;
    NodeInfo* all_nodes = nullptr;

    mutex_lock(&mount->tree_lock);
    for (VfsTreeNode* child = list_node->first_child; child; child = child->next_sibling) {
        total_count++;
    }

    if (total_count > 0) {
        all_nodes = arena_alloc_array(scratch, NodeInfo, total_count);
        u32 idx = 0;
        for (VfsTreeNode* child = list_node->first_child; child; child = child->next_sibling) {
            all_nodes[idx].name = string_copy(scratch, child->name);
            all_nodes[idx].size = child->size;
            all_nodes[idx].attributes = child->attributes;
            all_nodes[idx].is_directory = child->is_directory;
            all_nodes[idx].is_archive = child->is_archive;
            idx++;
        }
    }
    mutex_unlock(&mount->tree_lock);

    // Second pass: filter and score (outside lock)
    ScoredNode* scored_nodes = nullptr;
    u32 entry_count = 0;

    if (has_filter && needle_cstr && total_count > 0) {
        scored_nodes = arena_alloc_array(scratch, ScoredNode, total_count);

        for_count(i, total_count) {
            const char* name_cstr = string_to_cstr(scratch, all_nodes[i].name);

            if (has_match(needle_cstr, name_cstr)) {
                score_t score = match(needle_cstr, name_cstr);
                scored_nodes[entry_count].info = &all_nodes[i];
                scored_nodes[entry_count].score = score;
                entry_count++;
            }
        }

        // Sort by score (insertion sort)
        for (u32 i = 1; i < entry_count; i++) {
            ScoredNode key = scored_nodes[i];
            int j = i - 1;
            while (j >= 0 && scored_nodes[j].score < key.score) {
                scored_nodes[j + 1] = scored_nodes[j];
                j--;
            }
            scored_nodes[j + 1] = key;
        }
    } else {
        entry_count = total_count;
    }

    FlVfsEntry* entries = entry_count > 0 ? arena_alloc_array(list_arena, FlVfsEntry, entry_count) : nullptr;

    // Populate entries from the snapshots (no lock needed; nodes are not touched)
    for_count(i, entry_count) {
        const NodeInfo* info = (has_filter && scored_nodes) ? scored_nodes[i].info : &all_nodes[i];
        FlVfsEntry* slot = &entries[i];
        slot->name = string_copy(list_arena, info->name);
        slot->size = info->size;
        slot->is_directory = info->is_directory;
        slot->is_archive = info->is_archive;
        slot->attributes = info->attributes;
    }

    FlVfsFileList* file_list = arena_alloc_zero(list_arena, FlVfsFileList);
    file_list->entries = entries;
    file_list->count = entry_count;
    file_list->mount_version = atomic_load_explicit(&mount->version, memory_order_acquire);
    file_list->success = true;

    // Store snapshot version in handle for staleness checking
    handle->snapshot_version = file_list->mount_version;

    // Use atomic store with release semantics to ensure all writes to file_list are visible
    atomic_store_explicit(&handle->result_data, file_list, memory_order_release);
    handle->result_arena = list_arena; // Store for cleanup on vfs_close
    atomic_store_explicit(&handle->error_status, VFS_STATE_SUCCESS, memory_order_release);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mount operation

void vfs_op_mount(VfsState* self, VfsHandleData* handle, FlArena* scratch, int worker_index) {
    profile_function_auto_nc("vfs:vfs_op_mount", PROFILE_COLOR_CYAN);
    FlVfsMount* mount = handle->mount;

    // Resolve the full path - this handles nested archives and validates it's a directory
    FinalDriver result = vfs_tree_mount_path(self, mount, handle->path, scratch, worker_index);

    // Use per-mount tree lock to protect root_node modifications
    mutex_lock(&mount->tree_lock);

    if (result.error == VfsResolveError_None) {
        VfsHandles handles
            = vfs_tree_clone_plugin_handle(result.plugin_entry, result.plugin_entry->plugin, result.handle);

        // A failed clone hands result.handle back to us still open.
        if (!handles.handle[0]) {
            result.plugin_entry->plugin->close(result.plugin_entry->plugin_instance, result.handle);
        }

        mount->root_node->plugin_entry = result.plugin_entry;
        mount->root_node->handles = handles;
        atomic_store_explicit(&handle->error_status, VFS_STATE_SUCCESS, memory_order_release);

        if (mount->enable_file_watching && result.plugin_entry
            && string_equals(result.plugin_entry->plugin->plugin_name, S("LocalFS"))) {
            FlFileWatcherConfig config
                = { .recursive = true, .watch_files = true, .watch_directories = true, .max_changes = 0 };

            mount->watcher_handle = file_watcher_start(mount->source_path, config);
            mount->watching_enabled = (mount->watcher_handle != 0);
            atomic_store_explicit(&mount->version, 0, memory_order_release);
        } else {
            mount->watcher_handle = 0;
            mount->watching_enabled = false;
            atomic_store_explicit(&mount->version, 0, memory_order_release);
        }
    } else {
        mount->root_node->plugin_entry = nullptr;
        mount->root_node->handles = (VfsHandles) { 0 };
        mount->watcher_handle = 0;
        mount->watching_enabled = false;
        atomic_store_explicit(&handle->error_status, VFS_ERROR_MOUNT, memory_order_release);
    }

    mutex_unlock(&mount->tree_lock);

    // Error is propagated via handle->error_status - caller can check and log if needed
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Job scheduling

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Core execution logic - shared by both async and sync paths
static void vfs_ops_execute(VfsHandleData* handle, FlArena* scratch, int worker_index) {
    profile_function_auto_nc("vfs:vfs_ops_execute", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    // Cooperative cancellation: if cancel was requested before the job started, bail before doing any work.
    if (vfs_should_cancel(handle)) {
        if (handle->op_type == VfsOp_ReadAll) {
            read_all_notify_failure(handle);
        }
        atomic_store_explicit(&handle->result_data, nullptr, memory_order_relaxed);
        atomic_store_explicit(&handle->error_status, VFS_ERROR_CANCELED, memory_order_release);
        return;
    }

    switch (handle->op_type) {
        case VfsOp_Mount:
            vfs_op_mount(self, handle, scratch, worker_index);
            break;

        case VfsOp_MountList:
            vfs_op_list(self, handle, scratch, worker_index);
            break;

        case VfsOp_FileOpen:
            vfs_op_file_open(self, handle, scratch, worker_index);
            break;

        case VfsOp_FileRead:
            vfs_op_file_read(self, handle, scratch, worker_index);
            break;

        case VfsOp_FileWrite:
            vfs_op_file_write(self, handle, scratch, worker_index);
            break;

        case VfsOp_ReadAll:
            vfs_op_read_all(self, handle, scratch, worker_index);
            break;
    }

    UNUSED(self);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Async job wrapper - called by job system
void vfs_ops_do_job(void* data, FlJobsWorkerInfo info) {
    vfs_ops_execute((VfsHandleData*)data, info.scratch, info.worker_index);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Synchronous execution - called when already in job context
void vfs_ops_execute_sync(VfsHandleData* handle) {
    arena_scratch_auto(temp);
    vfs_ops_execute(handle, temp.arena, 0);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlJobHandle vfs_ops_schedule_job(FlVfsMount* mount, FlJobsFunc func, void* user_data) {
    profile_function_auto_nc("vfs:vfs_ops_schedule_job", PROFILE_COLOR_CYAN);

    // Handle 0 means mount was completed synchronously - schedule immediately
    if (mount->job_handle == 0) {
        return fl_jobs_add_job(func, user_data);
    }

    FlJobsResult mount_status = fl_jobs_is_finished(mount->job_handle);

    if (mount_status == FlJobsResult_NotFinished) {
        // Use job dependency to automatically schedule when mount completes
        return fl_jobs_add_job_with_dependency(func, user_data, mount->job_handle);
    }

    return fl_jobs_add_job(func, user_data);
}
