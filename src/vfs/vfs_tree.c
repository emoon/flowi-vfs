// VFS Tree Management
// Tree structure, path resolution, and archive loading

#include <core/log.h>
#include <core/path.h>
#include <core/profile.h>
#include <core/string_allocator.h>
#include <core/string.h>
#include "vfs_private.h"
#include <stdio.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// The one way this file closes a plugin handle. Every call site is reached only with a live handle, so a null
// here is a lifecycle bug rather than a case to skip.

static void close_plugin_handle(const FlVfsPlugin* plugin, void* instance, void* handle) {
    FL_VALIDATE(handle != nullptr);
    plugin->close(instance, handle);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tree debugging

static void dump_tree_node_recursive(VfsTreeNode* node, int depth) {
    profile_function_auto_nc("vfs:dump_tree_node_recursive", PROFILE_COLOR_CYAN);
    FL_VALIDATE(node != nullptr);

    for_count(i, depth) {
        printf("  ");
    }

    const char* type = node->is_directory ? "[DIR]" : "[FILE]";

    printf("%s %.*s (plugin: ", type, (int)node->name.length, node->name.data);

    if (node->plugin_entry && node->plugin_entry->plugin && node->plugin_entry->plugin->plugin_name.data) {
        printf("%.*s", (int)node->plugin_entry->plugin->plugin_name.length,
               node->plugin_entry->plugin->plugin_name.data);
    } else {
        printf("inherit");
    }

    if (node->memory_data) {
        printf(", memory: %llu bytes", (unsigned long long)node->memory_size);
    }

    printf(")\n");

    VfsTreeNode* child = node->first_child;
    while (child) {
        dump_tree_node_recursive(child, depth + 1);
        child = child->next_sibling;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_dump_tree(FlVfsMount* mount) {
    profile_function_auto_nc("vfs:vfs_dump_tree", PROFILE_COLOR_CYAN);
    if (!mount) {
        printf("VFS Tree Dump: nullptr mount\n");
        return;
    }

    printf("========================================\n");
    printf("VFS Tree Dump for mount:\n");
    printf("  Source Path: %.*s\n", (int)mount->source_path.length, mount->source_path.data);
    printf("  Status: %u\n", atomic_load(&mount->status));
    printf("  Tree:\n");

    mutex_lock(&mount->tree_lock);
    if (mount->root_node) {
        dump_tree_node_recursive(mount->root_node, 1);
    } else {
        printf("    (empty tree)\n");
    }
    mutex_unlock(&mount->tree_lock);
    printf("========================================\n\n");
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tree node operations

VfsTreeNode* vfs_tree_create_node(FlVfsMount* mount, FlString name) {
    profile_function_auto_nc("vfs:vfs_tree_create_node", PROFILE_COLOR_CYAN);
    VfsTreeNode* node = pool_alloc(&mount->nodes_pool);
    memset(node, 0, sizeof(VfsTreeNode));
    node->name = string_allocator_copy(mount->strings, name);
    return node;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// The mount's own root node, which is a directory by construction: it is what every path within the mount
// is resolved against.
VfsTreeNode* vfs_tree_create_root_node(FlVfsMount* mount, FlString name) {
    VfsTreeNode* node = vfs_tree_create_node(mount, name);
    node->is_directory = true;
    return node;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// True once the mount's root has been resolved to a driver and that driver has handed back a usable handle,
// which is what makes the mount servable.
bool vfs_tree_root_is_resolved(const FlVfsMount* mount) {
    return mount->root_node && mount->root_node->plugin_entry && mount->root_node->handles.handle[0];
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_tree_free_node(FlVfsMount* mount, VfsTreeNode* node) {
    FL_VALIDATE(node != nullptr);
    string_allocator_free(mount->strings, node->name);
    pool_free(&mount->nodes_pool, node);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

VfsTreeNode* vfs_tree_find_child_node(VfsTreeNode* parent, FlString name) {
    profile_function_auto_nc("vfs:vfs_tree_find_child_node", PROFILE_COLOR_CYAN);
    for (VfsTreeNode* child = parent->first_child; child != nullptr; child = child->next_sibling) {
        if (string_equals(child->name, name)) {
            return child;
        }
    }
    return nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int vfs_tree_compare_nodes(VfsTreeNode* a, VfsTreeNode* b) {
    profile_function_auto_nc("vfs:vfs_tree_compare_nodes", PROFILE_COLOR_CYAN);
    // Sort directories before files
    if (a->is_directory && !b->is_directory) {
        return -1;
    }
    if (!a->is_directory && b->is_directory) {
        return 1;
    }
    return string_compare_natural(a->name, b->name);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_tree_add_child_node(VfsTreeNode* parent, VfsTreeNode* child) {
    profile_function_auto_nc("vfs:vfs_tree_add_child_node", PROFILE_COLOR_CYAN);
    child->parent = parent;
    child->next_sibling = nullptr;

    if (!parent->first_child) {
        parent->first_child = child;
        return;
    }

    // Insert in sorted order: directories first, then files, both naturally sorted
    VfsTreeNode* current = parent->first_child;
    VfsTreeNode* prev = nullptr;

    while (current && vfs_tree_compare_nodes(child, current) > 0) {
        prev = current;
        current = current->next_sibling;
    }

    if (prev == nullptr) {
        child->next_sibling = parent->first_child;
        parent->first_child = child;
    } else {
        child->next_sibling = prev->next_sibling;
        prev->next_sibling = child;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Unlinks a child mount's root node from the sibling list of the node it was mounted under, leaving it
// parentless and ready for vfs_tree_cleanup_node. A no-op for a top-level mount, whose root has no parent.
// Takes the mount's tree_lock, so the caller must not already hold it.
void vfs_tree_unlink_from_parent(FlVfsMount* mount) {
    profile_function_auto_nc("vfs:vfs_tree_unlink_from_parent", PROFILE_COLOR_CYAN);

    if (!mount->root_node || !mount->root_node->parent) {
        return;
    }

    mutex_lock_auto(&mount->tree_lock);

    VfsTreeNode* node = mount->root_node;
    VfsTreeNode* parent = node->parent;
    VfsTreeNode* prev = nullptr;

    for (VfsTreeNode* current = parent->first_child; current; current = current->next_sibling) {
        if (current == node) {
            if (prev) {
                prev->next_sibling = current->next_sibling;
            } else {
                parent->first_child = current->next_sibling;
            }
            node->parent = nullptr;
            node->next_sibling = nullptr;
            return;
        }
        prev = current;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Caller must hold tree_lock.

static VfsTreeNode* find_node_by_path_unlocked(VfsTreeNode* root, FlString path, FlArena* scratch) {
    if (!root || path.length == 0) {
        return root;
    }

    PathComponents components = path_components(scratch, path);
    VfsTreeNode* current = root;

    for_count(i, components.segments.count) {
        if (!current) {
            break;
        }
        current = vfs_tree_find_child_node(current, components.segments.items[i]);
    }

    return current;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Release the plugin handle(s) and archive-owned memory a single node holds. Does not touch the node's
// children or free the node struct itself.

static void close_node_resources(VfsTreeNode* node) {
    // Note: Only close unique handles since some slots may share the same pointer
    if (node->plugin_entry) {
        void* closed_handles[MAX_JOB_THREADS] = { 0 };
        int num_closed = 0;

        for_count(i, MAX_JOB_THREADS) {
            if (!node->handles.handle[i]) {
                continue;
            }

            bool already_closed = false;
            for_count(j, num_closed) {
                if (closed_handles[j] == node->handles.handle[i]) {
                    already_closed = true;
                    break;
                }
            }

            if (!already_closed) {
                node->plugin_entry->plugin->close(node->plugin_entry->plugin_instance, node->handles.handle[i]);
                closed_handles[num_closed++] = node->handles.handle[i];
            }
            node->handles.handle[i] = nullptr;
        }
    }

    // Memory data is freed by the plugin's close() function (plugin owns the memory after open_memory())
    node->memory_data = nullptr;
    node->memory_size = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Caller must hold tree_lock. Does NOT free children - caller should handle recursively if needed.

static void remove_node_unlocked(FlVfsMount* mount, VfsTreeNode* node) {
    if (!node || !node->parent) {
        return; // Can't remove root node
    }

    VfsTreeNode* parent = node->parent;

    if (parent->first_child == node) {
        parent->first_child = node->next_sibling;
    } else {
        VfsTreeNode* prev = parent->first_child;
        while (prev && prev->next_sibling != node) {
            prev = prev->next_sibling;
        }
        if (prev) {
            prev->next_sibling = node->next_sibling;
        }
    }

    // Releases the node's plugin handle(s) and archive-owned memory; must run before the node is freed.
    close_node_resources(node);

    vfs_tree_free_node(mount, node);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void remove_node_recursive_unlocked(FlVfsMount* mount, VfsTreeNode* node) {
    FL_VALIDATE(node != nullptr);

    VfsTreeNode* child = node->first_child;
    while (child) {
        VfsTreeNode* next = child->next_sibling;
        remove_node_recursive_unlocked(mount, child);
        child = next;
    }
    node->first_child = nullptr;

    remove_node_unlocked(mount, node);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool vfs_tree_remove_node_by_path(FlVfsMount* mount, FlString relative_path) {
    if (!mount || !mount->root_node || relative_path.length == 0) {
        return false;
    }

    arena_scratch_auto(temp);

    mutex_lock(&mount->tree_lock);

    VfsTreeNode* node = find_node_by_path_unlocked(mount->root_node, relative_path, temp.arena);
    if (!node || node == mount->root_node) {
        mutex_unlock(&mount->tree_lock);
        return false; // Not found or trying to remove root
    }

    remove_node_recursive_unlocked(mount, node);

    mutex_unlock(&mount->tree_lock);
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Converts a file-watcher change path to a path relative to the mount's source_path. Watchers report absolute
// paths (FSEvents on macOS) even for a mount created with a relative path, so a direct prefix match is tried
// first, then a substring search for source_path bounded on a slash. The returned FlString points into
// change_path (no copy). When source_path cannot be located, change_path is returned unchanged.

FlString vfs_tree_watcher_relative_path(FlString change_path, FlString source_path) {
    // First try: direct prefix match (works when both are absolute or both are relative)
    if (string_begins_with(change_path, source_path)) {
        u64 prefix_len = source_path.length;
        // Skip any trailing/leading slashes
        while (prefix_len < change_path.length
               && (change_path.data[prefix_len] == '/' || change_path.data[prefix_len] == '\\')) {
            prefix_len++;
        }
        return (FlString) {
            .data = change_path.data + prefix_len,
            .length = change_path.length - prefix_len,
        };
    }

    // Second try: source_path as a slash-bounded substring, for a relative source_path against an
    // absolute change_path - source_path="data/test", change_path="/home/user/data/test/file.txt".
    if (source_path.length == 0 || change_path.length <= source_path.length) {
        return change_path;
    }

    // Search for "/source_path/" or "/source_path" at end
    u64 search_limit = change_path.length - source_path.length + 1;
    for (u64 i = 0; i < search_limit; i++) {
        bool preceded_by_slash = (i == 0) || (change_path.data[i - 1] == '/') || (change_path.data[i - 1] == '\\');
        if (!preceded_by_slash) {
            continue;
        }

        FlString substring = (FlString) { .data = change_path.data + i, .length = source_path.length, .is_static = 0 };
        if (string_equals(substring, source_path)) {
            u64 prefix_len = i + source_path.length;
            // Skip any trailing slashes
            while (prefix_len < change_path.length
                   && (change_path.data[prefix_len] == '/' || change_path.data[prefix_len] == '\\')) {
                prefix_len++;
            }
            return (FlString) {
                .data = change_path.data + prefix_len,
                .length = change_path.length - prefix_len,
            };
        }
    }

    return change_path;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper functions

static VfsTreeNode* ensure_child_node_exists(FlVfsMount* mount, VfsTreeNode* parent, FlString segment,
                                             bool mark_as_directory) {
    mutex_lock(&mount->tree_lock);

    VfsTreeNode* child = vfs_tree_find_child_node(parent, segment);
    if (!child) {
        child = vfs_tree_create_node(mount, segment);
        if (mark_as_directory) {
            child->is_directory = true;
        }
        vfs_tree_add_child_node(parent, child);
    }

    mutex_unlock(&mount->tree_lock);
    return child;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static inline VfsTreeNode* advance_to_virtual_directory(FlVfsMount* mount, VfsTreeNode** current_node, FlString segment,
                                                        int* segments_since_plugin) {
    VfsTreeNode* child = ensure_child_node_exists(mount, *current_node, segment, true);
    *current_node = child;
    (*segments_since_plugin)++;
    return child;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin utilities

static TryLoadData load_data_from_plugin(VfsPluginEntry* plugin_entry, void* handle, FlString read_path,
                                         FlString loaded_path, i64 size, int path_split_offset, FlArena* scratch) {
    u8* file_buffer = mi_malloc(size);

    FlVfsReadResult read_result
        = plugin_entry->plugin->read(plugin_entry->plugin_instance, handle, read_path, file_buffer, size);
    const int error = read_result.bytes_read == size ? VfsResolveError_None : VfsResolveError_InvalidFileSize;

    log_trace("%S", read_path);
    log_trace("%S", loaded_path);

    return (TryLoadData) {
        .handle = handle,
        .path_split_offset = path_split_offset,
        .data = file_buffer,
        .length = read_result.bytes_read,
        .error = error,
        .loaded_path = string_copy(scratch, loaded_path),
    };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static TryLoadData try_open_file(VfsPluginEntry* plugin_entry, const int range_start, const int range_end,
                                 const PathComponents* components, FlArena* scratch) {

    if (!plugin_entry || components->segments.count == 0) {
        return (TryLoadData) { .error = VfsResolveError_InvalidPath };
    }

    const FlVfsPlugin* plugin = plugin_entry->plugin;

    for (int try_segments = range_start; try_segments > range_end; try_segments--) {
        const FlString try_path = path_reconstruct_from_segments(components, range_end, try_segments, scratch);

        log_trace("%S: trying to open %S", plugin->plugin_name, try_path);

        if (!plugin->can_open(try_path)) {
            log_trace("%S: trying to open %S (failed)", plugin->plugin_name, try_path);
            continue;
        }

        log_trace("Trying to open %S", try_path);

        void* instance = plugin_entry->plugin_instance;
        FlVfsOpenResult open_result = plugin->open(instance, nullptr, try_path, 0, (FlVfsMountOptions) { 0 });

        if (!open_result.handle) {
            continue;
        }

        void* handle = open_result.handle;

        // Only return directly if top path is a directory
        if (plugin->is_directory(instance, handle) && try_segments == range_start) {
            log_debug("is dir");
            return (TryLoadData) { .handle = handle,
                                   .path_split_offset = try_segments,
                                   .loaded_path = string_copy(scratch, try_path),
                                   .is_directory = true };
        }

        FlVfsSizeResult size_result = plugin->get_size(instance, handle, (FlString) { 0 });

        if (size_result.size < 0) {
            close_plugin_handle(plugin, instance, handle);
            continue;
        }

        i64 size = size_result.size;

        // For opened handle, read with empty path but store try_path as loaded_path
        return load_data_from_plugin(plugin_entry, handle, (FlString) { 0 }, try_path, size, try_segments, scratch);
    }

    log_trace("not found");

    return (TryLoadData) { .error = VfsResolveError_FileNotFound };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

VfsHandles vfs_tree_clone_plugin_handle(VfsPluginEntry* plugin_entry, const FlVfsPlugin* plugin, void* handle) {
    profile_function_auto_nc("vfs:vfs_tree_clone_plugin_handle", PROFILE_COLOR_CYAN);
    VfsHandles handles = { 0 };

    handles.handle[0] = handle;

    for (int i = 1; i < MAX_JOB_THREADS; ++i) {
        FlVfsOpenResult res = { 0 };

        if (plugin->clone)
            res = plugin->clone(plugin_entry->plugin_instance, handle);

        if (!res.handle) {
            log_trace("Failed to clone plugin %S error: %d", plugin->plugin_name, res.error.vfs_status);

            // Close only the clones made here; handle stays the caller's to close.
            for (int j = 1; j < i; ++j) {
                plugin->close(plugin_entry->plugin_instance, handles.handle[j]);
            }

            return (VfsHandles) { 0 };
        }

        handles.handle[i] = res.handle;
    }

    return handles;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

DriverHandle vfs_tree_find_root_plugin_for_path(VfsState* self, FlArena* temp, const PathComponents* comp, int start) {
    profile_function_auto_nc("vfs:vfs_tree_find_root_plugin_for_path", PROFILE_COLOR_CYAN);
    int end = comp->segments.count;

    for (int try_segments = end; try_segments > start; try_segments--) {
        const FlString try_path = path_reconstruct_from_segments(comp, start, try_segments, temp);

        for (VfsPluginEntry* entry = self->plugin_first; entry != nullptr; entry = entry->next) {
            const FlVfsPlugin* plugin = entry->plugin;
            FlVfsOpenResult open_result
                = plugin->open(entry->plugin_instance, nullptr, try_path, 0, (FlVfsMountOptions) { 0 });

            if (!open_result.handle) {
                continue;
            }

            log_trace("Found root at %S plugin %S", try_path, plugin->plugin_name);

            return (DriverHandle) { .plugin_entry = entry, .handle = open_result.handle, .path_index = try_segments };
        }
    }

    log_trace("not found");

    return (DriverHandle) { 0 };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

DriverHandle vfs_tree_find_plugin_for_memory(VfsState* self, FlString org_path, u8* data, u64 size) {
    profile_function_auto_nc("vfs:vfs_tree_find_plugin_for_memory", PROFILE_COLOR_CYAN);
    log_trace("%S", org_path);

    for (VfsPluginEntry* entry = self->plugin_first; entry != nullptr; entry = entry->next) {
        const FlVfsPlugin* plugin = entry->plugin;

        FlVfsDriverCapabilities caps = plugin->get_capabilities();
        if (!(caps & FlVfsDriverCapabilities_Memory) || !plugin->open_memory) {
            continue;
        }

        FlVfsOpenResult open_result = plugin->open_memory(entry->plugin_instance, data, size, org_path);

        if (open_result.handle) {
            return (DriverHandle) { .plugin_entry = entry, .handle = open_result.handle };
        }

        if (open_result.error.vfs_status == FlVfsStatus_ArchiveCorrupt) {
            return (DriverHandle) { 0 };
        }
    }

    return (DriverHandle) { 0 };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Path resolution

FinalDriver vfs_tree_resolve_full_path(VfsState* self, VfsPluginEntry* root_entry, void* root_handle,
                                       PathComponents* components, int path_start, FlArena* scratch) {
    VfsPluginEntry* current_entry = root_entry;
    const FlVfsPlugin* current_plugin = root_entry->plugin;
    void* current_instance = current_entry->plugin_instance;
    void* current_handle = root_handle;
    int current_pos = path_start;
    const int total_segments = components->segments.count;

    // Special case: if all path segments were consumed by find_root_plugin_for_path,
    // check if the root handle itself is a file that needs to be opened as an archive
    if (current_pos >= total_segments && !current_plugin->is_directory(current_instance, current_handle)) {
        FlVfsSizeResult size_result = current_plugin->get_size(current_instance, current_handle, (FlString) { 0 });
        if (size_result.size < 0) {
            close_plugin_handle(current_plugin, current_instance, current_handle);
            return (FinalDriver) { .error = VfsResolveError_InvalidFileSize };
        }

        FlString loaded_path = path_reconstruct_from_segments(components, 0, total_segments, scratch);
        TryLoadData data = load_data_from_plugin(current_entry, current_handle, (FlString) { 0 }, loaded_path,
                                                 size_result.size, total_segments, scratch);

        if (data.error != VfsResolveError_None) {
            mi_free(data.data);
            close_plugin_handle(current_plugin, current_instance, current_handle);
            return (FinalDriver) { .error = data.error };
        }

        // Try to find a memory plugin for the loaded data (e.g., libarchive for ZIP)
        DriverHandle mem_plugin = vfs_tree_find_plugin_for_memory(self, data.loaded_path, data.data, data.length);

        if (!mem_plugin.plugin_entry || !mem_plugin.handle) {
            // No memory plugin - it's a regular file, cannot be mounted
            mi_free(data.data);
            close_plugin_handle(current_plugin, current_instance, current_handle);
            return (FinalDriver) { .error = VfsResolveError_NotMountable };
        }

        // Memory plugin took ownership of data.data - it will free it when handle is closed
        close_plugin_handle(current_plugin, current_instance, current_handle);
        current_entry = mem_plugin.plugin_entry;
        current_plugin = current_entry->plugin;
        current_instance = current_entry->plugin_instance;
        current_handle = mem_plugin.handle;
    }

    while (current_pos < total_segments) {
        // Try to load/open something from current position to end, scanning backwards
        TryLoadData data = try_open_file(current_entry, total_segments, current_pos, components, scratch);

        if (data.error != VfsResolveError_None) {
            // A handle is only present when try_open_file opened one and then failed, as a short read
            // does; InvalidPath and FileNotFound never opened anything.
            if (data.handle) {
                close_plugin_handle(current_plugin, current_instance, data.handle);
            }
            mi_free(data.data);
            close_plugin_handle(current_plugin, current_instance, current_handle);
            return (FinalDriver) { .error = data.error };
        }

        if (data.is_directory) {
            // try_open_file only reports a directory when that directory consumed the whole remaining
            // path, so this is always the final layer. Directories carry no data.data.
            close_plugin_handle(current_plugin, current_instance, current_handle);
            return (
                FinalDriver) { .plugin_entry = current_entry, .handle = data.handle, .error = VfsResolveError_None };
        }

        // We found a file - try to open it as an archive using memory plugins
        DriverHandle mem_plugin = vfs_tree_find_plugin_for_memory(self, data.loaded_path, data.data, data.length);

        close_plugin_handle(current_plugin, current_instance, data.handle);
        close_plugin_handle(current_plugin, current_instance, current_handle);

        if (!mem_plugin.plugin_entry || !mem_plugin.handle) {
            // No memory plugin could open it - it's a regular file (not an archive)
            mi_free(data.data);
            return (FinalDriver) { .error = VfsResolveError_NotMountable };
        }

        // Memory plugin took ownership of data.data - it will free it when handle is closed
        current_entry = mem_plugin.plugin_entry;
        current_plugin = current_entry->plugin;
        current_instance = current_entry->plugin_instance;
        current_handle = mem_plugin.handle;
        current_pos = data.path_split_offset;
    }

    // All segments processed - verify final result is a directory/archive
    if (!current_plugin->is_directory(current_instance, current_handle)) {
        close_plugin_handle(current_plugin, current_instance, current_handle);
        return (FinalDriver) { .error = VfsResolveError_NotMountable };
    }

    return (FinalDriver) { .plugin_entry = current_entry, .handle = current_handle, .error = VfsResolveError_None };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FinalDriver vfs_tree_mount_path(VfsState* self, FlVfsMount* mount, FlString path, FlArena* scratch, int worker_index) {
    profile_function_auto_nc("vfs:vfs_tree_mount_path", PROFILE_COLOR_CYAN);
    UNUSED(mount);
    UNUSED(worker_index);

    PathComponents components = path_components(scratch, path);
    DriverHandle root_plugin = vfs_tree_find_root_plugin_for_path(self, scratch, &components, 0);

    if (!root_plugin.plugin_entry) {
        log_trace("No root plugin found for path: %S", path);
        return (FinalDriver) { .error = VfsResolveError_NoDriver };
    }

    FinalDriver result = vfs_tree_resolve_full_path(self, root_plugin.plugin_entry, root_plugin.handle, &components,
                                                    root_plugin.path_index, scratch);

    return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Archive loading

bool vfs_tree_try_load_as_archive(VfsState* self, FlVfsMount* mount, VfsTreeNode* node, VfsPluginEntry* plugin_entry,
                                  void* plugin_handle, FlString path, i64 size, FlArena* scratch) {
    if (size <= 0) {
        return false;
    }

    const FlVfsPlugin* plugin = plugin_entry->plugin;
    void* instance = plugin_entry->plugin_instance;

    FlVfsDriverCapabilities caps = plugin->get_capabilities();
    if ((caps & FlVfsDriverCapabilities_DirectPath) && plugin->get_file_path) {
        FlString file_path = plugin->get_file_path(instance, plugin_handle, path, scratch);

        if (file_path.length > 0) {
            // Registration order puts LibArchive ahead of LocalFS here.
            for (VfsPluginEntry* archive_entry = self->plugin_first; archive_entry != nullptr;
                 archive_entry = archive_entry->next) {
                const FlVfsPlugin* archive_plugin = archive_entry->plugin;

                if (!archive_plugin->can_open(file_path)) {
                    continue;
                }

                FlVfsOpenResult open_result = archive_plugin->open(archive_entry->plugin_instance, nullptr, file_path,
                                                                   0, (FlVfsMountOptions) { 0 });
                if (!open_result.handle) {
                    continue;
                }

                VfsHandles cloned_handles
                    = vfs_tree_clone_plugin_handle(archive_entry, archive_plugin, open_result.handle);
                if (!cloned_handles.handle[0]) {
                    close_plugin_handle(archive_plugin, archive_entry->plugin_instance, open_result.handle);
                    continue;
                }

                mutex_lock(&mount->tree_lock);
                // Releases any previous plugin handles / archive memory before the swap. No-op for a fresh node.
                close_node_resources(node);
                node->plugin_entry = archive_entry;
                node->handles = cloned_handles;
                node->memory_data = nullptr; // File-based, no memory copy
                node->memory_size = 0;
                node->is_directory = true;
                mutex_unlock(&mount->tree_lock);

                return true;
            }
        }

        // Fall through to memory-based loading if file-based loading failed
    }

    // Fallback: read the file into memory when file-based loading was unavailable or failed.
    u8* file_data = mi_malloc(size);
    FlVfsReadResult read_result = plugin->read(instance, plugin_handle, path, file_data, size);

    if (read_result.bytes_read != size) {
        mi_free(file_data);
        return false;
    }

    DriverHandle mem_plugin = vfs_tree_find_plugin_for_memory(self, path, file_data, read_result.bytes_read);

    if (!mem_plugin.plugin_entry) {
        mi_free(file_data);
        return false;
    }

    VfsHandles cloned_handles
        = vfs_tree_clone_plugin_handle(mem_plugin.plugin_entry, mem_plugin.plugin_entry->plugin, mem_plugin.handle);
    if (!cloned_handles.handle[0]) {
        // The plugin owns file_data now and frees it from close(); freeing it here too would be a double free.
        close_plugin_handle(mem_plugin.plugin_entry->plugin, mem_plugin.plugin_entry->plugin_instance,
                            mem_plugin.handle);
        return false;
    }

    mutex_lock(&mount->tree_lock);
    // Releases the previous plugin handles and archive memory before the swap; the old backing memory is
    // owned by the previous plugin instance and freed by its close(). No-op for a fresh node.
    close_node_resources(node);
    node->plugin_entry = mem_plugin.plugin_entry;
    node->handles = cloned_handles;
    node->memory_data = file_data;
    node->memory_size = read_result.bytes_read;
    node->is_directory = true;
    mutex_unlock(&mount->tree_lock);

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tree traversal

TreeWalkResult vfs_tree_walk_and_resolve(VfsState* self, FlVfsMount* mount, FlString path, FlArena* scratch,
                                         int thread_index, bool for_listing) {
    // Per-node plugin handle arrays hold MAX_JOB_THREADS entries and are indexed by worker. vfs_init()
    // rejects an oversized job system, but one created after the VFS reaches here unchecked.
    if (thread_index < 0 || thread_index >= MAX_JOB_THREADS) {
        logc_error(VFS_ID, "VFS tree walk from worker %d but plugin handle arrays hold %d entries", thread_index,
                   MAX_JOB_THREADS);
        return (TreeWalkResult) { 0 };
    }

    if (path.length == 0) {
        return (TreeWalkResult) { .node = mount->root_node,
                                  .plugin_entry = mount->root_node->plugin_entry,
                                  .plugin_handle = mount->root_node->handles.handle[thread_index],
                                  .path_from_node = S(""),
                                  .success = true };
    }

    PathComponents components = path_components(scratch, path);

    VfsTreeNode* current_node = mount->root_node;
    VfsPluginEntry* current_plugin_entry = mount->root_node->plugin_entry;
    void* current_plugin_handle = mount->root_node->handles.handle[thread_index];
    int segments_since_plugin = 0;

    // For listing: traverse all segments to reach the directory to list
    // For reading: traverse to parent of final segment (final segment is the file to read)
    int segments_to_process = for_listing ? (int)components.segments.count : (int)components.segments.count - 1;

    for_count(segment_idx, segments_to_process) {
        FlString segment = components.segments.items[segment_idx];

        mutex_lock(&mount->tree_lock);
        VfsTreeNode* child = vfs_tree_find_child_node(current_node, segment);

        if (child) {
            // Copy node properties while the lock is held; they are used after it is released.
            bool has_plugin = (child->plugin_entry != nullptr);
            bool is_directory = child->is_directory;
            VfsPluginEntry* child_plugin_entry = child->plugin_entry;
            void* child_plugin_handle = child->plugin_entry ? child->handles.handle[thread_index] : nullptr;

            // Release lock before doing potentially slow operations
            mutex_unlock(&mount->tree_lock);

            current_node = child;

            if (has_plugin) {
                current_plugin_entry = child_plugin_entry;
                current_plugin_handle = child_plugin_handle;
                segments_since_plugin = 0;
                continue;
            }

            if (!is_directory && current_plugin_entry) {
                // Build path from current plugin's perspective
                FlString path_to_check = path_reconstruct_from_segments(
                    &components, segment_idx - segments_since_plugin, segment_idx + 1, scratch);

                // Get file size (potentially slow - lock already released)
                FlVfsSizeResult size_result = current_plugin_entry->plugin->get_size(
                    current_plugin_entry->plugin_instance, current_plugin_handle, path_to_check);

                // Try to load as archive (handles its own locking)
                if (vfs_tree_try_load_as_archive(self, mount, child, current_plugin_entry, current_plugin_handle,
                                                 path_to_check, size_result.size, scratch)) {
                    // Re-read plugin fields (vfs_tree_try_load_as_archive locks internally)
                    current_plugin_entry = child->plugin_entry;
                    current_plugin_handle = child->handles.handle[thread_index];
                    segments_since_plugin = 0;
                    continue;
                }
            }

            // No plugin loaded, continue with parent plugin
            segments_since_plugin++;
            continue;
        }

        // Child doesn't exist - release lock before potentially slow operations
        mutex_unlock(&mount->tree_lock);

        if (!current_plugin_entry) {
            FlString remaining = path_get_remaining_from_segments(&components, segment_idx, scratch);
            return (TreeWalkResult) { .node = current_node, .path_from_node = remaining, .success = false };
        }

        if (!for_listing) {
            advance_to_virtual_directory(mount, &current_node, segment, &segments_since_plugin);
            continue;
        }

        // Build path from current plugin's perspective
        FlString path_to_check = path_reconstruct_from_segments(&components, segment_idx - segments_since_plugin,
                                                                segment_idx + 1, scratch);

        FlVfsSizeResult size_result = current_plugin_entry->plugin->get_size(current_plugin_entry->plugin_instance,
                                                                             current_plugin_handle, path_to_check);

        if (size_result.size < 0) {
            // Path doesn't exist as a file - but for HTTP, intermediate paths might be "virtual directories"
            // that don't exist as downloadable resources but are part of the URL structure.
            advance_to_virtual_directory(mount, &current_node, segment, &segments_since_plugin);
            continue;
        }

        child = ensure_child_node_exists(mount, current_node, segment, false);

        if (vfs_tree_try_load_as_archive(self, mount, child, current_plugin_entry, current_plugin_handle, path_to_check,
                                         size_result.size, scratch)) {
            current_node = child;
            current_plugin_entry = child->plugin_entry;
            current_plugin_handle = child->handles.handle[thread_index];
            segments_since_plugin = 0;
        } else {
            // Not an archive (or size is 0, or read failed) - treat as directory
            child->is_directory = true;
            current_node = child;
            segments_since_plugin++;
        }
    }

    // Reconstruct path from plugin root to current node so plugin knows what to list
    if (for_listing) {
        FlString list_path = S("");
        if (segments_since_plugin > 0) {
            list_path = path_reconstruct_from_segments(&components, segments_to_process - segments_since_plugin,
                                                       segments_to_process, scratch);
        }

        return (TreeWalkResult) { .node = current_node,
                                  .plugin_entry = current_plugin_entry,
                                  .plugin_handle = current_plugin_handle,
                                  .path_from_node = list_path,
                                  .success = true };
    }

    // For read mode, get the final filename to read
    FlString final_file = components.segments.items[components.segments.count - 1];

    // Build path from current plugin's perspective
    FlString read_path;
    if (segments_since_plugin == 0) {
        // Reading directly from current node
        read_path = final_file;
    } else {
        // Need to include intermediate segments
        read_path = path_reconstruct_from_segments(&components, components.segments.count - 1 - segments_since_plugin,
                                                   components.segments.count, scratch);
    }

    return (TreeWalkResult) { .node = current_node,
                              .plugin_entry = current_plugin_entry,
                              .plugin_handle = current_plugin_handle,
                              .path_from_node = read_path,
                              .success = true };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tree cleanup

void vfs_tree_cleanup_node(VfsTreeNode* node) {
    profile_function_auto_nc("vfs:vfs_tree_cleanup_node", PROFILE_COLOR_CYAN);
    FL_VALIDATE(node != nullptr);

    VfsTreeNode* child = node->first_child;
    while (child) {
        VfsTreeNode* next = child->next_sibling;
        vfs_tree_cleanup_node(child);
        child = next;
    }

    close_node_resources(node);
}
