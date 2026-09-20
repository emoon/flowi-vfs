#pragma once

#include <core/string.h>
#include <core/jobsys.h>
#include <core/types.h>
#include <flowi/vfs/vfs_api.h>
#include <flowi/vfs/vfs_plugin.h>

struct FlArena;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VFS Operation Status - Hierarchical 32-bit error system (internal use only)

typedef u32 VfsOpStatus;

// Bits 0-7: Core VFS state (256 values)
#define VFS_STATE_MASK 0x000000FF
#define VFS_STATE_SUCCESS 0x00000000
#define VFS_STATE_PENDING 0x00000001
#define VFS_STATE_ERROR 0x00000002
#define VFS_STATE_CLOSED 0x00000003
#define VFS_STATE_NONE 0x00000004

// Bits 8-15: VFS subsystem errors (256 values)
#define VFS_SUBSYSTEM_MASK 0x0000FF00
#define VFS_SUBSYSTEM_SHIFT 8
#define VFS_MOUNT_ERROR 0x00000100
#define VFS_DRIVER_ERROR 0x00000200
#define VFS_CACHE_ERROR 0x00000300
#define VFS_NETWORK_ERROR 0x00000400
#define VFS_ARCHIVE_ERROR 0x00000500
#define VFS_PATH_ERROR 0x00000600
#define VFS_PERMISSION_ERROR 0x00000700
#define VFS_CANCELED_ERROR 0x00000800
#define VFS_THREADING_ERROR 0x00000900

// Bits 16-31: Available for callback/user errors (65536 values)
#define VFS_USER_MASK 0xFFFF0000
#define VFS_USER_SHIFT 16

// Common error combinations for convenience
#define VFS_ERROR_MOUNT (VFS_STATE_ERROR | VFS_MOUNT_ERROR)
#define VFS_ERROR_DRIVER (VFS_STATE_ERROR | VFS_DRIVER_ERROR)
#define VFS_ERROR_CACHE (VFS_STATE_ERROR | VFS_CACHE_ERROR)
#define VFS_ERROR_NETWORK (VFS_STATE_ERROR | VFS_NETWORK_ERROR)
#define VFS_ERROR_ARCHIVE (VFS_STATE_ERROR | VFS_ARCHIVE_ERROR)
#define VFS_ERROR_PATH (VFS_STATE_ERROR | VFS_PATH_ERROR)
#define VFS_ERROR_PERMISSION (VFS_STATE_ERROR | VFS_PERMISSION_ERROR)
// Additional error codes for driver use
#define VFS_ERROR_FILE_NOT_FOUND (VFS_STATE_ERROR | VFS_DRIVER_ERROR)
#define VFS_ERROR_READ (VFS_STATE_ERROR | VFS_DRIVER_ERROR)
#define VFS_ERROR_INVALID_PATH (VFS_STATE_ERROR | VFS_PATH_ERROR)
#define VFS_ERROR_CANCELED (VFS_STATE_ERROR | VFS_CANCELED_ERROR)
#define VFS_ERROR_FILE_TOO_LARGE (VFS_STATE_ERROR | VFS_DRIVER_ERROR)
// The op could only have proceeded by waiting on a job worker, which the job system forbids
#define VFS_ERROR_WOULD_BLOCK (VFS_STATE_ERROR | VFS_THREADING_ERROR)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Callback invoked once per discovered entry during directory listing

typedef void (*VfsListingCallback)(FlVfsEntry entry, void* user_data);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Initialize the VFS system. Registers the built-in LocalFS driver, which stays the catch-all fallback,
// tried after every driver registered later via vfs_register_driver().
// Returns true if this call initialized the VFS (the caller owns it and should call vfs_destroy),
// false if it was already initialized (first-caller-wins; this call was a no-op).
bool vfs_init(struct FlArena* arena);

void vfs_destroy(void);

// Pump the VFS: process deferred/rate-limited operations and poll file watchers
// for filesystem changes. Call once per frame to advance async I/O.
void vfs_update(void);

// Convenience macro for mounting with default options
#define vfs_mount(source_path) vfs_mount_with_options(source_path, (FlVfsMountOptions) { 0 })

// Returns 0 if the mount completed synchronously or mount is null
FlJobHandle vfs_mount_get_job_handle(FlVfsMount* mount);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct VfsReadOptions {
    FlVfsReadCallback callback;
    void* user_data;
    FlVfsReleaseCallback release; // Runs once with user_data when the handle is freed (see FlVfsReleaseCallback)
    FlVfsHandle reuse_handle;     // Handle to reuse (or FL_VFS_HANDLE_INVALID/0)
} VfsReadOptions;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// File Operations on Mounts

// Get size of an open file (synchronous)
// Returns: Size result with file size (negative on error)
FlVfsSizeResult vfs_get_size(FlVfsHandle file_handle);

// Two-phase read: allocate handle without dispatching the job.
// Must be followed by vfs_dispatch() to actually start the operation.
FlVfsHandle vfs_mount_read_all_prepare(FlVfsMount* mount, FlString relative_path, FlVfsReadCallback callback,
                                       void* user_data, FlVfsReleaseCallback release, FlVfsHandle reuse_handle);

// Dispatch a previously prepared handle (from vfs_mount_read_all_prepare).
void vfs_dispatch(FlVfsHandle handle);

#define vfs_mount_read_all(mount, path, ...)                                                         \
    ({                                                                                               \
        VfsReadOptions _opts = { __VA_ARGS__ };                                                      \
        vfs_mount_read_all_with_options(mount, path, _opts.callback, _opts.user_data, _opts.release, \
                                        _opts.reuse_handle);                                         \
    })

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Handle Operations
//
// Thread contract: fl_jobs_wait() does nothing on a job worker, because blocking one on a job that itself
// needs a worker can deadlock the pool. A worker can therefore only finish an operation that has already
// finished, and the calls below fail closed rather than pretend otherwise: vfs_wait() logs and returns,
// vfs_close() logs and leaves the handle open instead of freeing state the running job still uses, and a
// read/write chained onto an unfinished predecessor completes with VFS_ERROR_WOULD_BLOCK instead of running
// against a file that is not open yet. Wait for, close, and chain onto in-flight handles from the main
// thread. Handles a worker opened itself are unaffected - the job system runs those inline, so they are
// already finished. vfs_mount_close() and vfs_destroy() are main-thread teardown and are not guarded.
//
// vfs_close() never waits on the main thread either. Closing a handle whose job is still running marks it
// closed: it reads as absent from then on, its id is not recycled, and the VFS frees it once the job has
// finished - from vfs_update(), vfs_wait_all() or a later vfs_close(). The job itself runs to completion, so
// a read-all's user_data must stay valid past the close; the read's release hook is what tells its owner when
// it may go. The exception is a handle from vfs_read() / vfs_write() / vfs_write_no_copy(): those run
// against memory the caller lent for the op and closing one is the caller's signal that the memory may go, so
// that close waits for the op first (and is refused on a worker, as above).

void vfs_wait_all(void);

VfsOpStatus vfs_get_status(FlVfsHandle handle);

// Note: This is cooperative cancellation - the operation will check for cancellation
//       at strategic points and bail out gracefully. The handle must still be closed
//       with vfs_close() after cancellation.
bool vfs_cancel(FlVfsHandle handle);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Debug and Introspection

void vfs_dump_tree(FlVfsMount* mount);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Fuzzy filtering

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static inline VfsOpStatus vfs_get_core_state(VfsOpStatus status) {
    return status & VFS_STATE_MASK;
}

static inline u32 vfs_get_subsystem_error(VfsOpStatus status) {
    return status & VFS_SUBSYSTEM_MASK;
}

static inline u32 vfs_get_user_error(VfsOpStatus status) {
    return status & VFS_USER_MASK;
}

static inline bool vfs_is_success(VfsOpStatus status) {
    return vfs_get_core_state(status) == VFS_STATE_SUCCESS;
}

static inline bool vfs_is_error(VfsOpStatus status) {
    return vfs_get_core_state(status) == VFS_STATE_ERROR;
}

static inline bool vfs_is_pending(VfsOpStatus status) {
    return vfs_get_core_state(status) == VFS_STATE_PENDING;
}

static inline bool vfs_is_closed(VfsOpStatus status) {
    return vfs_get_core_state(status) == VFS_STATE_CLOSED;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Driver Registration (for extending VFS)
//
// Hand a constructed driver to the VFS. Call after vfs_init(). The plugin pointer must outlive the VFS.
// instance: Optional driver instance handle passed to every driver op (NULL for stateless drivers)
void vfs_register_driver(const FlVfsPlugin* plugin, void* instance);
