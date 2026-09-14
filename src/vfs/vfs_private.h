#pragma once

// Private internal header shared between vfs.c, vfs_tree.c, and vfs_ops.c

#include <core/log.h>
#include <core/path.h>
#include "vfs_internal.h"

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Shared type definitions

// Result structure for resolve_full_path function
typedef struct {
    VfsPluginEntry* plugin_entry; // Final plugin that can handle the resolved path (nullptr on error)
    void* handle;                 // Handle to the final resource (nullptr on error)
    VfsResolveError error;
} FinalDriver;

typedef struct {
    VfsPluginEntry* plugin_entry;
    void* handle;
    int path_index;
} DriverHandle;

typedef struct {
    u8* data;
    i64 length;
    int error;
    int path_split_offset;
    void* handle;
    FlString loaded_path;
    bool is_directory;
} TryLoadData;

typedef struct {
    VfsTreeNode* node;            // Deepest node reached
    VfsPluginEntry* plugin_entry; // Plugin to use for reading
    void* plugin_handle;
    FlString path_from_node; // Remaining path from the node
    bool success;            // Whether path was fully resolved
} TreeWalkResult;

typedef struct {
    FlString needle;
    FlVfsEntry* output_entries;
    u32 output_count;
    u32 output_capacity;
} VfsDirFilterContext;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Global state access (defined in vfs.c)

extern LogChannelId VFS_ID;
extern VfsState* g_vfs_state;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tree management functions (defined in vfs_tree.c)

// Tree node operations
VfsTreeNode* vfs_tree_create_node(FlVfsMount* mount, FlString name);
void vfs_tree_free_node(FlVfsMount* mount, VfsTreeNode* node);
VfsTreeNode* vfs_tree_find_child_node(VfsTreeNode* parent, FlString name);
void vfs_tree_add_child_node(VfsTreeNode* parent, VfsTreeNode* child);
int vfs_tree_compare_nodes(VfsTreeNode* a, VfsTreeNode* b);
bool vfs_tree_remove_node_by_path(FlVfsMount* mount, FlString relative_path);
FlString vfs_tree_watcher_relative_path(FlString change_path, FlString source_path);

// Tree traversal and path resolution
TreeWalkResult vfs_tree_walk_and_resolve(VfsState* self, FlVfsMount* mount, FlString path, FlArena* scratch,
                                         int thread_index, bool for_listing);
bool vfs_tree_try_load_as_archive(VfsState* self, FlVfsMount* mount, VfsTreeNode* node, VfsPluginEntry* plugin_entry,
                                  void* plugin_handle, FlString path, i64 size, FlArena* scratch);

// Mount path resolution
FinalDriver vfs_tree_mount_path(VfsState* self, FlVfsMount* mount, FlString path, FlArena* scratch, int worker_index);

// Consumes root_handle and every container handle it opens while descending. On success exactly one
// handle is live, the one returned in FinalDriver.handle, and the caller owns it; on error no handle is
// live. root_handle is only left open when it is itself the returned handle.
FinalDriver vfs_tree_resolve_full_path(VfsState* self, VfsPluginEntry* root_entry, void* root_handle,
                                       PathComponents* components, int path_start, FlArena* scratch);

// Plugin utilities

// Fills a per-worker handle array from handle, cloning it once per extra worker. On success slot 0 is
// handle itself and the array owns every slot. On failure returns a zeroed array, having closed the
// clones it made; handle is left open and the caller still owns it.
VfsHandles vfs_tree_clone_plugin_handle(VfsPluginEntry* plugin_entry, const FlVfsPlugin* plugin, void* handle);
// Returns a freshly opened handle the caller owns, or a zeroed DriverHandle when no plugin matched.
DriverHandle vfs_tree_find_root_plugin_for_path(VfsState* self, FlArena* temp, const PathComponents* comp, int start);

// On success the plugin has taken ownership of data and frees it when the returned handle is closed, and the
// caller owns that handle. On failure data is untouched and still the caller's to free.
DriverHandle vfs_tree_find_plugin_for_memory(VfsState* self, FlString org_path, u8* data, u64 size);

// Tree cleanup
void vfs_tree_cleanup_node(VfsTreeNode* node);

// Tree debugging
void vfs_tree_dump_recursive(VfsTreeNode* node, int depth);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Operations functions (defined in vfs_ops.c)

// Operation implementations
void vfs_op_mount(VfsState* self, VfsHandleData* handle, FlArena* scratch, int worker_index);
void vfs_op_list(VfsState* self, VfsHandleData* handle, FlArena* scratch, int thread_index);
void vfs_op_file_open(VfsState* self, VfsHandleData* handle, FlArena* scratch, int thread_index);
void vfs_op_file_read(VfsState* self, VfsHandleData* handle, FlArena* scratch, int thread_index);
void vfs_op_file_write(VfsState* self, VfsHandleData* handle, FlArena* scratch, int thread_index);

// Job scheduling
void vfs_ops_do_job(void* data, FlJobsWorkerInfo info);
void vfs_ops_execute_sync(VfsHandleData* handle);
FlJobHandle vfs_ops_schedule_job(FlVfsMount* mount, FlJobsFunc func, void* user_data);

// Directory listing helpers
bool vfs_ops_check_if_archive(VfsState* self, FlString file_path);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Core VFS functions (defined in vfs.c)

// Error handling
FlString vfs_format_error_message(FlArena* scratch, const char* fmt, ...);
void vfs_free_error_message(VfsState* self, FlString error_message);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Cancellation support

static inline bool vfs_should_cancel(VfsHandleData* handle) {
    if (!handle) {
        return false;
    }
    return atomic_load_explicit(&handle->cancel_requested, memory_order_acquire);
}
