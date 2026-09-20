#pragma once

#include <core/arena.h>
#include <core/core.h>
#include <core/string.h>
#include <core/hashmap.h>
#include <core/jobsys.h>
#include <core/pool_allocator.h>
#include <core/os/os.h>
#include "vfs.h"
#include <flowi/vfs/vfs_plugin.h>
#include <stdatomic.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Internal constants

#define VFS_MAX_MOUNTS 64
#define VFS_MAX_HANDLES 256
// Upper bound on job-system worker threads the VFS supports: per-node plugin handle arrays (VfsHandles)
// hold one pre-cloned handle per worker and are indexed by the worker index. vfs_init() rejects job
// systems with more workers, and vfs_tree_walk_and_resolve() rejects out-of-range worker indices.
#define MAX_JOB_THREADS 2

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Forward declarations

typedef struct VfsState VfsState;
typedef struct VfsHandleData VfsHandleData;
typedef struct StringAllocator StringAllocator;

// VFS Plugin Entry - a registered driver plus its (optional) instance handle, linked into VfsState's list.
typedef struct VfsPluginEntry {
    struct VfsPluginEntry* next;
    struct VfsPluginEntry* prev;
    const FlVfsPlugin* plugin;
    void* plugin_instance;
} VfsPluginEntry;

// Path resolution helpers
typedef struct PathResolution {
    VfsPluginEntry* plugin_entry;
    FlString resolved_path;
    FlString remaining_path;
} PathResolution;

typedef struct PathOpenResult {
    VfsPluginEntry* plugin_entry;
    void* handle;
    FlString resolved_path;
    FlString remaining_path;
} PathOpenResult;

// Full path resolution with nested mount support
typedef enum VfsResolveError {
    VfsResolveError_None = 0,
    VfsResolveError_FileNotFound,
    VfsResolveError_ReadFailed,
    VfsResolveError_NoDriver,
    VfsResolveError_InvalidMount,
    VfsResolveError_InvalidPath,
    VfsResolveError_InvalidFileSize, // The difference between stat and read buffer differed
    VfsResolveError_CorruptArchive,  // Archive file is corrupted/invalid (fatal, don't try other drivers)
    VfsResolveError_NotMountable,    // Path resolves to a regular file (not a directory or archive)
} VfsResolveError;

typedef struct FullPathResolution {
    VfsPluginEntry* final_plugin_entry; // Plugin handling the final segment (nullptr if failed)
    void* final_handle;                 // Handle to the final resource (nullptr if failed)
    FlString resolved_path;             // The portion that was successfully resolved (empty if failed)
    FlString remaining_path;            // Any unresolved portion (empty if fully resolved)
    FlString target_file;               // Final file within the last archive (empty if none)
    bool fully_resolved;
    VfsResolveError error;
} FullPathResolution;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Handle data for operations

typedef enum VfsOperationType {
    VfsOp_Mount,
    VfsOp_MountList,
    VfsOp_FileOpen,
    VfsOp_FileRead,
    VfsOp_FileWrite,
    VfsOp_ReadAll, // Monolithic read: open + read entire file + close (no separate file handle)
} VfsOperationType;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct VfsHandles {
    void* handle[MAX_JOB_THREADS];
} VfsHandles;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct VfsTreeNode {
    struct VfsTreeNode* parent;
    struct VfsTreeNode* first_child;
    struct VfsTreeNode* next_sibling;

    // Plugin for this node (nullptr if using parent's plugin)
    VfsPluginEntry* plugin_entry;
    // Array of plugin handles (one per worker thread for thread safety)
    VfsHandles handles;

    // Memory data for loaded archives (shared across thread-cloned plugins)
    u8* memory_data;
    u64 memory_size;

    FlString name;
    bool is_directory; // True if this is an actual OS directory
    bool is_archive;   // True if this file can be opened as an archive (navigable)
    i64 size;          // File size (0 for directories)
    u32 attributes;    // Platform-specific attributes

} VfsTreeNode;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct FlVfsMount {
    CACHE_ALIGNED _Atomic u32 status;
    struct FlVfsMount* next;
    struct FlArena* job_arena;

    struct FlArena* nodes_arena; // Arena for pool allocator and StringAllocator backing storage
    pool(VfsTreeNode) nodes_pool;
    struct StringAllocator* strings; // Thread-safe allocator for node name strings (supports free)

    FlString source_path; // The actual source path (e.g., "/home/user/data" or "https://example.com/file.zip")

    // Mount-level options (applied to all file operations from this mount)
    FlVfsMountOptions options;
    int ref_count;
    FlJobHandle job_handle;
    FlVfsHandle mount_handle_id; // VFS handle ID for mount operation (must be closed)
    VfsTreeNode* root_node;

    Mutex tree_lock; // Protects tree structure modifications

    // File watching for LocalFS mounts
    u32 watcher_handle;        // File watcher handle (0 = no watcher)
    _Atomic u32 version;       // Incremented when filesystem changes detected
    bool watching_enabled;     // Whether file watching is currently active
    bool enable_file_watching; // Whether to enable file watching (set by user)
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct VfsHandleData {
    FlJobHandle job_handle;
    // job_handle 0 means "no job, result already published" everywhere except between vfs_dispatch scheduling
    // the job and storing its handle. This marks that window so a handle whose job is queued or running is not
    // mistaken for a finished one.
    _Atomic bool dispatch_pending;
    // Closed while its job was still running. Reads as absent to every caller and is freed by the reclaim
    // sweep once the job has finished; until then the job keeps its raw pointer. Written under handle_lock.
    bool closed;
    FlVfsMount* mount;
    FlString path;
    _Atomic(void*) result_data; // Points to FlVfsData, FlVfsFileList, etc. (atomic for thread safety)
    FlArena* result_arena;      // FlArena for result data (destroyed on vfs_close for list operations)

    VfsOperationType op_type;
    u32 snapshot_version; // Mount version when this operation was created (for listing staleness check)
    u32 id;
    int depth;                        // Depth for directory listing operations
    _Atomic VfsOpStatus error_status; // Hierarchical VFS status for errors (atomic for thread safety)

    struct FlArena* target_arena; // Target arena for copying data (arena mode) or passing to callback
    FlVfsReadCallback callback;   // Callback for custom processing (callback mode)
    void* user_data;
    FlVfsReleaseCallback release; // Runs once with user_data when the handle is freed, after its job

    FlVfsLoadPriority priority;

    // Fuzzy filter fields
    FlVfsDirHandle source_list_handle;
    FlString filter_needle; // Optional fuzzy filter for the listing; mount-owned copy, like path above

    // File operation fields (for VfsOp_FileOpen)
    VfsPluginEntry* plugin_entry; // Resolved plugin entry (from tree walk)
    void* plugin_handle;          // Plugin container handle (e.g., opened zip file)
    void* plugin_file_handle;     // Plugin handle from open()
    _Atomic u32 open_flags;       // Flags used to open the file (accessed atomically via atomic_load/store_u32)
    FlJobHandle last_job;         // Last job scheduled on this file (for chaining)

    // File read fields (for VfsOp_FileRead)
    u8* read_buffer; // Buffer to read into (caller-provided)
    i64 read_size;
    i64 bytes_read; // Actual bytes read (set after operation completes)

    // File write fields (for VfsOp_FileWrite)
    FlVfsHandle file_handle; // Reference to file handle (used by both read and write)
    const u8* write_data;
    i64 write_size;
    bool write_data_owned; // True if VFS owns write_data (must free), false if caller owns

    /// Set if callback owns the result data (VFS should not free it)
    bool callback_owns_data;

    /// Cancellation support - cooperative cancellation flag
    _Atomic bool cancel_requested;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Main VFS state

typedef struct VfsState {
    FlVfsMount* mount_first;
    FlVfsMount* mount_last;

    struct FlArena* main_arena; // External arena (not owned by VFS, don't destroy!)

    // Registered drivers, in registration order. Handed in directly via vfs_register_driver().
    VfsPluginEntry* plugin_first;
    VfsPluginEntry* plugin_last;

    // The built-in LocalFS driver, registered by vfs_init(). It is the catch-all fallback (its can_open
    // accepts any existing path), so it must stay last: vfs_register_driver() inserts host drivers ahead of
    // it, keeping specific drivers (archives, network) tried before LocalFS.
    VfsPluginEntry* builtin_fallback;

    // Mount management
    Mutex mount_lock;

    // Handle management
    Mutex handle_lock;
    u32 next_handle_id;
    u32 next_mount_id;

    pool(VfsHandleData) handle_pool;
    pool(FlVfsMount) vfs_mounts;

    hashmap(u32, VfsHandleData*) handle_map;
    u32 closed_count; // Handles in the map with closed set, so the reclaim sweep can skip the walk

} VfsState;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Internal functions

// Global state access
VfsState* vfs_get_state(void);

// Handle management
VfsHandleData* vfs_allocate_handle(VfsState* self);
// Job-worker use ONLY: the returned raw pointer outlives handle_lock and stays valid only for the
// duration of an operation job.
VfsHandleData* vfs_get_handle_data(FlVfsHandle handle);
void vfs_free_handle(VfsState* self, VfsHandleData* handle_data);

// Mount management
struct FlArena* vfs_get_mount_arena(VfsState* self);
void vfs_return_mount_arena(VfsState* self, struct FlArena* arena);

// Plugin utilities
VfsPluginEntry* vfs_find_plugin_for_path(VfsState* self, FlString path);

// Job functions for async operations
void vfs_job_mount(void* user_data, FlJobsWorkerInfo info);
void vfs_job_mount_read(void* user_data, FlJobsWorkerInfo info);
void vfs_job_mount_list(void* user_data, FlJobsWorkerInfo info);

FullPathResolution vfs_resolve_full_path(VfsState* self, FlString path, struct FlArena* scratch);

void vfs_cleanup_full_path_resolution(FullPathResolution* resolution);
