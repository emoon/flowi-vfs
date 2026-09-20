#include "vfs.h"
#include <core/arena.h>
#include <core/assert.h>
#include <core/error_report.h>
#include <core/file_watcher.h>
#include <core/log.h>
#include <core/path.h>
#include <core/profile.h>
#include <core/sprintf.h>
#include <core/string_allocator.h>
#include <core/string.h>
#include <core/jobsys.h>
#include <core/os/os.h>
#include "vfs_internal.h"
#include "vfs_private.h"
#include "drivers/localfs.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "match.h"

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Global state (exported to vfs_tree.c and vfs_ops.c)

LogChannelId VFS_ID = LOG_CHANNEL_INVALID;
VfsState* g_vfs_state = nullptr;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Nil VFS Entry (Read-Only Global)
//
// Represents invalid/empty VFS entry. Functions return pointers to this instead of nullptr.

const FlVfsEntry fl_nil_vfs_entry = { 0 };

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void vfs_reclaim_closed_handles(VfsState* self);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Caller must hold handle_lock. Finds a closed-but-unreclaimed handle too: vfs_close() and the reclaim sweep
// need to see it, and a chained op reaches its file handle through it.
static inline VfsHandleData* vfs_lookup_handle_locked(VfsState* self, FlVfsHandle handle) {
    VfsHandleData** handle_ptr = hashmap_get(&self->handle_map, handle);
    return (handle_ptr && *handle_ptr) ? *handle_ptr : nullptr;
}

// Caller must hold handle_lock. The lookup every caller-facing entry point uses: a handle closed while its job
// was still running reads as absent.
static inline VfsHandleData* vfs_lookup_open_handle_locked(VfsState* self, FlVfsHandle handle) {
    VfsHandleData* handle_data = vfs_lookup_handle_locked(self, handle);
    return (handle_data && !handle_data->closed) ? handle_data : nullptr;
}

// Value-copy of the handle fields non-job callers need, taken under handle_lock so a concurrent vfs_close
// can't free the struct mid-read. The job-written fields are valid only when ready (see vfs_snapshot_handle).
// What a snapshot points at - result_data, path, plugin_file_handle - stays valid until vfs_close; closing a
// handle concurrently with any call that snapshotted it is caller error.
typedef struct VfsHandleSnapshot {
    VfsOperationType op_type;
    FlJobHandle job_handle;
    FlJobHandle last_job;
    FlVfsMount* mount;
    FlString path;
    int depth;
    u32 snapshot_version;
    bool ready;            // Job finished (or none scheduled)
    bool dispatch_pending; // A job that can reach this handle is scheduled but not yet recorded on it
    void* result_data;
    VfsOpStatus error_status;
    VfsPluginEntry* plugin_entry;
    void* plugin_file_handle;
} VfsHandleSnapshot;

typedef struct VfsHandleJobs {
    FlJobHandle handles[2];
    u32 count;
} VfsHandleJobs;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static VfsHandleJobs vfs_handle_jobs(FlJobHandle job_handle, FlJobHandle last_job) {
    VfsHandleJobs jobs = { 0 };
    if (job_handle) {
        jobs.handles[jobs.count++] = job_handle;
    }
    if (last_job && last_job != job_handle) {
        jobs.handles[jobs.count++] = last_job;
    }
    return jobs;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool vfs_jobs_finished(VfsHandleJobs jobs) {
    for (u32 i = 0; i < jobs.count; ++i) {
        if (fl_jobs_is_finished(jobs.handles[i]) == FlJobsResult_NotFinished) {
            return false;
        }
    }
    return true;
}

// Returns false if a job is still running afterwards: fl_jobs_wait() is a no-op on a job worker, so a
// caller that would free the handle on the strength of the wait must bail instead.
static bool vfs_wait_handle_jobs(VfsHandleJobs jobs) {
    for (u32 i = 0; i < jobs.count; ++i) {
        fl_jobs_wait(jobs.handles[i]);
    }
    return vfs_jobs_finished(jobs);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void vfs_append_handle_jobs(FlJobHandle* dst, u64* count, VfsHandleJobs jobs) {
    for (u32 i = 0; i < jobs.count; ++i) {
        dst[(*count)++] = jobs.handles[i];
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Collects the handles a walk is interested in, so the caller can act on them once handle_lock is released.
// A null predicate takes every handle.
//
// Every walk over the handle map is two-pass, and not incidentally:
//   - hashmap_remove unlinks the node for_each_hashmap is standing on, so nothing may be freed mid-walk;
//   - fl_jobs_wait must not run under handle_lock, which the job itself takes to reach its handle;
//   - a release hook is caller code, and one that reaches back into the VFS would deadlock on the lock.
//
// The pointers are only valid while the lock is held, so whatever a caller still needs after the unlock -
// an id, a job handle - must be copied out of them inside the same critical section.
//
// Caller holds handle_lock. out must have room for hashmap_count(&self->handle_map) entries.
typedef bool (*VfsHandlePredicate)(const VfsHandleData* handle_data, void* ctx);

static u64 vfs_collect_handles_locked(VfsState* self, VfsHandlePredicate pred, void* ctx, VfsHandleData** out) {
    u64 count = 0;
    for_each_hashmap(&self->handle_map, key, val) {
        (void)key;
        VfsHandleData* handle_data = *val;
        if (handle_data && (!pred || pred(handle_data, ctx))) {
            out[count++] = handle_data;
        }
    }
    return count;
}

static bool vfs_snapshot_handle(FlVfsHandle handle, VfsHandleSnapshot* out) {
    VfsState* self = g_vfs_state;

    if (!self || handle == FL_VFS_HANDLE_INVALID) {
        return false;
    }

    mutex_lock_auto(&self->handle_lock);
    VfsHandleData* handle_data = vfs_lookup_open_handle_locked(self, handle);
    if (!handle_data) {
        return false;
    }

    out->op_type = handle_data->op_type;
    out->job_handle = handle_data->job_handle;
    out->last_job = handle_data->last_job;
    out->mount = handle_data->mount;
    out->path = handle_data->path;
    out->depth = handle_data->depth;
    out->dispatch_pending = atomic_load_explicit(&handle_data->pending_dispatches, memory_order_acquire) != 0;
    out->ready = !out->dispatch_pending
                 && ((handle_data->job_handle == 0)
                     || (fl_jobs_is_finished(handle_data->job_handle) != FlJobsResult_NotFinished));

    // The fields below are plain-written by the handle's own job worker, so they may only be read once the
    // job is finished (the completion check above gives the happens-before). Before that they read as unset.
    if (out->ready) {
        out->plugin_entry = handle_data->plugin_entry;
        out->plugin_file_handle = handle_data->plugin_file_handle;
        out->snapshot_version = handle_data->snapshot_version;
    } else {
        out->plugin_entry = nullptr;
        out->plugin_file_handle = nullptr;
        out->snapshot_version = 0;
    }
    out->result_data = atomic_load_explicit(&handle_data->result_data, memory_order_acquire);
    out->error_status = atomic_load_explicit(&handle_data->error_status, memory_order_acquire);
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Job-worker use ONLY (vfs_ops.c): the returned pointer outlives the lock and stays valid only for the
// duration of an operation job. Non-job code uses vfs_snapshot_handle() instead.

VfsHandleData* vfs_get_handle_data(FlVfsHandle handle) {
    VfsState* self = g_vfs_state;

    if (!self || handle == FL_VFS_HANDLE_INVALID) {
        return nullptr;
    }

    mutex_lock_auto(&self->handle_lock);
    return vfs_lookup_handle_locked(self, handle);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool vfs_is_listing_current(FlVfsDirHandle handle) {
    profile_function_auto_nc("vfs:vfs_is_listing_current", PROFILE_COLOR_CYAN);
    VfsHandleSnapshot snap;
    if (!vfs_snapshot_handle(handle, &snap)) {
        return false;
    }

    if (snap.op_type != VfsOp_MountList || !snap.ready) {
        return false;
    }

    FL_VALIDATE_RET(snap.mount != nullptr, false);

    u32 current_version = atomic_load_explicit(&snap.mount->version, memory_order_acquire);
    return snap.snapshot_version == current_version;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool vfs_init(FlArena* arena) {
    profile_function_auto_nc("vfs:vfs_init", PROFILE_COLOR_CYAN);
    VFS_ID = fl_log_register_channel(S("VFS"));

    VfsState* self = g_vfs_state;

    if (self != nullptr) {
        return false;
    }

    // Per-node plugin handle arrays hold MAX_JOB_THREADS entries and are indexed by the job-system
    // worker index, so a job system with more workers would index past the arrays on every tree walk.
    if (jobs_global_exists()) {
        const int num_workers = fl_jobs_num_threads();
        if (num_workers > MAX_JOB_THREADS) {
            logc_error(VFS_ID, "vfs_init: job system has %d worker threads but the VFS supports at most %d",
                       num_workers, MAX_JOB_THREADS);
            return false;
        }
    }

    if (!arena) {
        arena = arena_new();
    }

    self = arena_alloc_zero(arena, VfsState);

    self->main_arena = arena;
    self->plugin_first = nullptr;
    self->plugin_last = nullptr;

    mutex_init(&self->mount_lock);

    mutex_init(&self->handle_lock);
    self->next_handle_id = 1;
    self->next_mount_id = 1;

    pool_new(&self->handle_pool, self->main_arena);
    pool_new(&self->vfs_mounts, self->main_arena);
    hashmap_new(&self->handle_map, self->main_arena, 256);

    self->mount_first = nullptr;
    self->mount_last = nullptr;

    g_vfs_state = self;

    // Register the built-in LocalFS driver so the VFS can read real files off disk out of the box. It is the
    // catch-all fallback and stays last (see builtin_fallback).
    vfs_register_driver(localfs_plugin(), nullptr);
    self->builtin_fallback = self->plugin_last;

    logc_info(VFS_ID, "VFS initialized");

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_register_driver(const FlVfsPlugin* plugin, void* instance) {
    profile_function_auto_nc("vfs:vfs_register_driver", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    if (self == nullptr) {
        logc_error(VFS_ID, "vfs_register_driver called before vfs_init");
        return;
    }
    if (plugin == nullptr) {
        return;
    }

    VfsPluginEntry* entry = arena_alloc_zero(self->main_arena, VfsPluginEntry);
    entry->plugin = plugin;
    entry->plugin_instance = instance;

    VfsPluginEntry* fallback = self->builtin_fallback;
    if (fallback != nullptr && plugin != fallback->plugin) {
        // Keep the built-in LocalFS fallback last: splice the new driver in immediately before it so specific
        // drivers are always tried first during path resolution.
        entry->next = fallback;
        entry->prev = fallback->prev;
        if (fallback->prev != nullptr) {
            fallback->prev->next = entry;
        } else {
            self->plugin_first = entry;
        }
        fallback->prev = entry;
    } else {
        // Append at the tail (the fallback itself during vfs_init, or before any fallback exists).
        entry->next = nullptr;
        entry->prev = self->plugin_last;
        if (self->plugin_last != nullptr) {
            self->plugin_last->next = entry;
        } else {
            self->plugin_first = entry;
        }
        self->plugin_last = entry;
    }

    logc_info(VFS_ID, "VFS driver registered: %S", plugin->plugin_name);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Handle allocation helpers

// Everything a handle is born with, so every birth states the same set of fields. Filled with designated
// initializers at the call site: the tail of this was a positional run of four interchangeable pointers,
// of which most callers pass nothing, and a swapped pair compiled clean. A zeroed field is the "not
// applicable" case for every one of them - FL_VFS_HANDLE_INVALID is 0, as is an empty FlString.
typedef struct VfsOpParams {
    FlVfsMount* mount;
    FlString path;
    VfsOperationType op_type;
    int depth;
    FlArena* target_arena;
    FlVfsReadCallback callback;
    void* user_data;
    FlVfsReleaseCallback release;
    FlVfsHandle reuse_handle;
    FlString filter_needle;
    u32 open_flags;
} VfsOpParams;

// Allocates and initializes a handle WITHOUT publishing it in the handle map. Until vfs_publish_handle()
// runs, the handle is private to the calling thread, so field writes need no lock. Callers finish op-specific
// setup (and job scheduling) on the private handle, then publish as the last step.
//
// This is the only place a VfsHandleData is born: every op type goes through it, including the read and
// write chained onto an open file handle, so a field added here cannot miss one of them.
static VfsHandleData* allocate_handle(VfsState* self, const VfsOpParams* params) {
    FL_VALIDATE_RET(self != nullptr, nullptr);

    FlVfsMount* mount = params->mount;

    // handle_lock protects the pool, next_handle_id, and the reuse check
    mutex_lock(&self->handle_lock);

    VfsHandleData* handle = pool_alloc(&self->handle_pool);

    // Zero the handle to avoid garbage values from pool reuse
    memory_zero(handle, sizeof(VfsHandleData));

    if (params->reuse_handle != FL_VFS_HANDLE_INVALID) {
        VfsHandleData** existing = hashmap_get(&self->handle_map, params->reuse_handle);
        if (existing && *existing) {
            logc_warning(VFS_ID, "Handle %u still exists in map - cannot reuse. Call vfs_close() first!",
                         params->reuse_handle);
            pool_free(&self->handle_pool, handle);
            mutex_unlock(&self->handle_lock);
            return nullptr;
        }
        handle->id = params->reuse_handle;
    } else {
        handle->id = self->next_handle_id++;
    }

    mutex_unlock(&self->handle_lock);

    // StringAllocator is thread-safe (has internal mutex), so the copies need no tree_lock. The needle is
    // copied for the same reason as the path: the job matches against it long after the caller's frame is
    // gone, and callers filter with scratch strings. An empty or static string copies to itself, without
    // allocating, which is what an op with no path or no filter gets.
    handle->path = string_allocator_copy(mount->strings, params->path);
    handle->filter_needle = string_allocator_copy(mount->strings, params->filter_needle);

    handle->mount = mount;
    handle->op_type = params->op_type;
    handle->callback = params->callback;
    handle->user_data = params->user_data;
    handle->release = params->release;
    handle->target_arena = params->target_arena;
    handle->depth = params->depth;
    handle->result_arena = nullptr; // Will be set for list operations
    handle->job_handle = 0;
    handle->priority = FlVfsLoadPriority_Normal;

    atomic_init(&handle->result_data, nullptr);
    atomic_init(&handle->error_status, VFS_STATE_PENDING);
    atomic_init(&handle->cancel_requested, false);
    // Set before any scheduling the caller does, so the job cannot read it unset.
    atomic_init(&handle->open_flags, params->open_flags);

    return handle;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Inserts a fully initialized handle into the handle map, making it visible to lookups. This is the LAST step
// of every allocation path: after this, fields may only change under handle_lock (or from the handle's own
// job).

static void vfs_publish_handle(VfsState* self, VfsHandleData* handle) {
    mutex_lock(&self->handle_lock);
    hashmap_insert(&self->handle_map, handle->id, handle);
    mutex_unlock(&self->handle_lock);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Allocate, schedule and publish an op against a mount, in that order.
static FlVfsHandle allocate_op(VfsState* self, const VfsOpParams* params) {
    VfsHandleData* handle = allocate_handle(self, params);

    FL_VALIDATE_RET(handle != nullptr, FL_VFS_HANDLE_INVALID);

    // Scheduling can execute the job inline on a worker thread, so it happens before publication - the
    // job holds the raw pointer and never needs the map entry. No field of a visible handle is ever
    // written without handle_lock.
    handle->job_handle = vfs_ops_schedule_job(params->mount, vfs_ops_do_job, handle);
    handle->last_job = handle->job_handle;

    FlVfsHandle id = handle->id;
    vfs_publish_handle(self, handle);

    return id;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mount management

FlVfsMountResult vfs_mount_with_options(FlString source_path, FlVfsMountOptions options) {
    logc_debug(VFS_ID, "mounting %S", source_path);

    profile_function_auto_nc("vfs:vfs_mount_with_options", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    FlVfsMountResult result
        = { .mount = nullptr, .status = FlVfsMountErrorStatus_Success, .error_message = string_empty() };

    if (self == nullptr) {
        result.status = FlVfsMountErrorStatus_NotInitialized;
        result.error_message = S("VFS system not initialized");
        logc_error(VFS_ID, "VFS mount failed: %S", result.error_message);
        return result;
    }

    if (string_is_empty(source_path)) {
        result.status = FlVfsMountErrorStatus_InvalidPath;
        result.error_message = S("Source path is empty");
        logc_error(VFS_ID, "VFS mount failed: %S", result.error_message);
        return result;
    }

    mutex_lock(&self->mount_lock);

    FlVfsMount* mount = pool_alloc(&self->vfs_mounts);

    // Zero the mount to avoid garbage values from pool reuse
    memory_zero(mount, sizeof(FlVfsMount));

    sll_queue_push(self->mount_first, self->mount_last, mount);

    mount->nodes_arena = arena_new();
    pool_new(&mount->nodes_pool, mount->nodes_arena);
    mount->strings = string_allocator_new(mount->nodes_arena);
    mutex_init(&mount->tree_lock);

    mount->root_node = vfs_tree_create_root_node(mount, source_path);

    VfsHandleData* handle = allocate_handle(self, &(VfsOpParams) {
                                                      .mount = mount,
                                                      .path = source_path,
                                                      .op_type = VfsOp_Mount,
                                                  });

    mount->source_path = string_allocator_copy(mount->strings, source_path);

    mount->options.cache_validation = options.cache_validation;
    if (options.cache_dir.length > 0) {
        mount->options.cache_dir = string_allocator_copy(mount->strings, options.cache_dir);
    }

    // Set the flag BEFORE scheduling: the worker can finish vfs_op_mount() and read it before
    // vfs_mount_with_options() has even returned to the caller.
    mount->enable_file_watching = options.enable_file_watching;

    if (!fl_jobs_is_main_thread()) {
        vfs_ops_execute_sync(handle);
        mount->job_handle = 0;
        handle->job_handle = 0;
    } else {
        mount->job_handle = fl_jobs_add_job(vfs_ops_do_job, handle);
        // Set handle's job_handle so vfs_wait_all() properly waits for mounts
        handle->job_handle = mount->job_handle;
    }
    handle->last_job = handle->job_handle;

    mount->mount_handle_id = handle->id;

    // Publish only after the job fields are final - no mutation of a visible handle without handle_lock
    vfs_publish_handle(self, handle);

    mutex_unlock(&self->mount_lock);

    result.mount = mount;
    result.status = FlVfsMountErrorStatus_Success;

    return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Public API

void vfs_set_priority(FlVfsHandle handle, FlVfsLoadPriority priority) {
    profile_function_auto_nc("vfs:vfs_set_priority", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    FL_VALIDATE(self != nullptr);

    // The lock covers the lookup and every handle_data access, guarding against a concurrent vfs_close.
    // fl_jobs_set_priority is atomics-only and never blocks, so it is safe to call under it.
    mutex_lock_auto(&self->handle_lock);
    VfsHandleData* handle_data = vfs_lookup_open_handle_locked(self, handle);

    if (handle_data) {
        handle_data->priority = priority;

        if (handle_data->job_handle) {
            // Map VFS priority to job priority (clamp Highest to High)
            int prio = (int)priority;
            JobPriority job_priority = (prio > (int)JobPriority_High) ? JobPriority_High : (JobPriority)prio;
            fl_jobs_set_priority(handle_data->job_handle, job_priority);
        }
    }
    // Note: Silently ignore invalid handles - this is expected for already-loaded images
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsDirHandle vfs_get_listing_filtered(FlVfsMount* mount, FlString relative_path, int depth, FlString filter_needle) {
    profile_function_auto_nc("vfs:vfs_get_listing", PROFILE_COLOR_CYAN);

    FL_VALIDATE_LOG(mount != nullptr, FL_VFS_DIR_HANDLE_INVALID, logc_error(VFS_ID, "Invalid mount for get_listing"));

    return allocate_op(g_vfs_state, &(VfsOpParams) {
                                        .mount = mount,
                                        .path = relative_path,
                                        .op_type = VfsOp_MountList,
                                        .depth = depth,
                                        .filter_needle = filter_needle,
                                    });
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsDirHandle vfs_get_listing(FlVfsMount* mount, FlString relative_path, int depth) {
    return vfs_get_listing_filtered(mount, relative_path, depth, (FlString) { 0 });
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsDirHandle vfs_apply_fuzzy_filter(FlVfsDirHandle source_handle, FlString needle) {
    profile_function_auto_nc("vfs:vfs_apply_fuzzy_filter", PROFILE_COLOR_CYAN);

    VfsHandleSnapshot snap;
    if (!vfs_snapshot_handle(source_handle, &snap) || snap.op_type != VfsOp_MountList) {
        logc_error(VFS_ID, "Invalid source handle for fuzzy filter (must be a listing handle)");
        return FL_VFS_DIR_HANDLE_INVALID;
    }

    return vfs_get_listing_filtered(snap.mount, snap.path, snap.depth, needle);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_wait(FlVfsHandle handle) {
    profile_function_auto_nc("vfs:vfs_wait", PROFILE_COLOR_CYAN);
    VfsHandleSnapshot snap;
    if (!vfs_snapshot_handle(handle, &snap)) {
        return;
    }

    // Wait lock-free: the job itself takes handle_lock (see vfs_close). last_job covers reads/writes
    // chained onto a file handle after its open job finished.
    if (!vfs_wait_handle_jobs(vfs_handle_jobs(snap.job_handle, snap.last_job)) || snap.dispatch_pending) {
        logc_error(VFS_ID,
                   "vfs_wait(%u) from a job worker cannot block; the operation is still running. Wait for an "
                   "in-flight handle from the main thread.",
                   handle);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_wait_all(void) {
    profile_function_auto_nc("vfs:vfs_wait_all", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    FL_VALIDATE(self != nullptr);

    // Snapshot the jobs while the map is stable, then wait without handle_lock. Handles published after this
    // snapshot belong to a later wait_all call. Two job slots per handle: its own job, and whatever was
    // chained onto that after.
    arena_scratch_auto(temp);
    mutex_lock(&self->handle_lock);
    u64 handle_count = hashmap_count(&self->handle_map);
    VfsHandleData** handles = arena_alloc_array(temp.arena, VfsHandleData*, handle_count);
    FlJobHandle* jobs = arena_alloc_array(temp.arena, FlJobHandle, handle_count * 2);

    u64 collected = vfs_collect_handles_locked(self, nullptr, nullptr, handles);
    u64 job_count = 0;
    for (u64 i = 0; i < collected; ++i) {
        vfs_append_handle_jobs(jobs, &job_count, vfs_handle_jobs(handles[i]->job_handle, handles[i]->last_job));
    }
    mutex_unlock(&self->handle_lock);

    for (u64 i = 0; i < job_count; ++i) {
        fl_jobs_wait(jobs[i]);
    }

    vfs_reclaim_closed_handles(self);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool vfs_is_ready(FlVfsHandle handle) {
    profile_function_auto_nc("vfs:vfs_is_ready", PROFILE_COLOR_CYAN);
    // ready covers both the synchronous case (job_handle 0) and a finished/recycled async job
    VfsHandleSnapshot snap;
    return vfs_snapshot_handle(handle, &snap) && snap.ready;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Snapshots a completed read/list op and reports whether its result payload is present. Returns true with a
// valid snap->result_data (the caller casts it to its op's result type) when the op finished and produced
// data. Otherwise returns false and sets *out_success to the op's outcome: false when the op isn't ready or
// doesn't exist, else the error-status success. Shared empty-result path of vfs_get_data / vfs_get_file_list.

static bool vfs_snapshot_ready_result(FlVfsHandle handle, VfsHandleSnapshot* snap, bool* out_success) {
    if (!vfs_snapshot_handle(handle, snap) || !snap->ready) {
        *out_success = false;
        return false;
    }

    if (snap->result_data) {
        return true;
    }

    *out_success = vfs_is_success(snap->error_status);
    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsData vfs_get_data(FlVfsHandle handle) {
    profile_function_auto_nc("vfs:vfs_get_data", PROFILE_COLOR_CYAN);
    FlVfsData empty = { 0 };

    VfsHandleSnapshot snap;
    if (vfs_snapshot_ready_result(handle, &snap, &empty.success)) {
        return *(FlVfsData*)snap.result_data;
    }
    return empty;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

VfsOpStatus vfs_get_status(FlVfsHandle handle) {
    profile_function_auto_nc("vfs:vfs_get_status", PROFILE_COLOR_CYAN);
    VfsHandleSnapshot snap;
    if (!vfs_snapshot_handle(handle, &snap)) {
        return VFS_STATE_ERROR;
    }

    if (!snap.ready) {
        return VFS_STATE_PENDING;
    }

    return snap.error_status;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsFileList vfs_get_file_list(FlVfsDirHandle handle) {
    profile_function_auto_nc("vfs:vfs_get_file_list", PROFILE_COLOR_CYAN);
    FlVfsFileList empty = { 0 };

    VfsHandleSnapshot snap;
    if (vfs_snapshot_ready_result(handle, &snap, &empty.success)) {
        return *(FlVfsFileList*)snap.result_data;
    }
    return empty;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Error handling

FlString vfs_format_error_message(const char* fmt, ...) {
    profile_function_auto_nc("vfs:vfs_format_error", PROFILE_COLOR_CYAN);

    va_list args;
    va_start(args, fmt);
    FlString result = error_message_vformat_no_log(fmt, args);
    va_end(args);

    return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// An error_message needs no freeing here: error_report owns that storage and bulk-frees it in
// error_report_cleanup().
static void vfs_free_data_result(FlVfsData* result, bool free_data_buffer) {
    if (free_data_buffer && result->data) {
        mi_free((void*)result->data);
    }
    mi_free(result);
}

// Handle teardown. Caller holds handle_lock, or is in single-threaded teardown. Does NOT remove the handle
// from the map/pool, and does NOT touch a chained op's file_handle: that handle is caller-owned.
static void vfs_free_handle_resources(VfsState* self, VfsHandleData* handle_data) {
    FlVfsData* result = atomic_load_explicit(&handle_data->result_data, memory_order_acquire);

    // Switched rather than chained so a new op type fails the -Werror build here until it says what it owns.
    switch (handle_data->op_type) {
        case VfsOp_Mount: {
            // Owns nothing of its own: a mount's storage belongs to the FlVfsMount, not to this handle.
            break;
        }

        case VfsOp_MountList: {
            if (result) {
                FlVfsFileList* list = (FlVfsFileList*)result;
                // With a result_arena the listing lives in that arena, destroyed below; without one this is
                // the error path, where the struct was mi_alloc'd on its own.
                if (!handle_data->result_arena) {
                    mi_free(list);
                }
            }
            break;
        }

        case VfsOp_FileOpen: {
            if (handle_data->plugin_file_handle && handle_data->plugin_entry
                && handle_data->plugin_entry->plugin->close) {
                handle_data->plugin_entry->plugin->close(handle_data->plugin_entry->plugin_instance,
                                                         handle_data->plugin_file_handle);
            }
            break;
        }

        case VfsOp_FileRead: {
            if (result) {
                // result->data points at the caller-provided read buffer (vfs_read rejects a null buffer),
                // so the data is never ours to free - only the result struct.
                vfs_free_data_result(result, false);
            }
            break;
        }

        case VfsOp_FileWrite: {
            // Only free if VFS made a copy (vfs_write), not if caller retained ownership (vfs_write_no_copy)
            if (handle_data->write_data && handle_data->write_data_owned) {
                mi_free((void*)handle_data->write_data);
            }
            // The payload is write_data (freed above); the write result struct never owns a data buffer.
            if (result) {
                vfs_free_data_result(result, false);
            }
            break;
        }

        case VfsOp_ReadAll: {
            if (result) {
                vfs_free_data_result(result, !handle_data->callback_owns_data);
            }
            break;
        }
    }

    // Destroy result arena if it exists (for successful directory listings)
    if (handle_data->result_arena) {
        arena_destroy(handle_data->result_arena);
    }

    // Free the handle's strings (allocated from the mount's StringAllocator). Both free calls no-op on the
    // empty and static strings a handle without a path or filter holds.
    if (handle_data->mount) {
        string_allocator_free(handle_data->mount->strings, handle_data->path);
        string_allocator_free(handle_data->mount->strings, handle_data->filter_needle);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VFS close

// A read-all's release hook and its argument, taken off the handle at free time and run once handle_lock is
// released: the hook is caller code, and one that reaches back into the VFS would deadlock on the lock.
typedef struct VfsRelease {
    FlVfsReleaseCallback fn;
    void* user_data;
} VfsRelease;

// The one place a release hook is invoked. It must never run under handle_lock: the hook is caller code,
// and one that reaches back into the VFS would deadlock on the lock.
static void vfs_run_release(VfsRelease release) {
    if (release.fn) {
        release.fn(release.user_data);
    }
}

// The hooks taken off a batch of freed handles: filled under handle_lock, drained after it is released.
typedef struct VfsReleaseList {
    VfsRelease* items;
    u64 count;
    u64 capacity;
} VfsReleaseList;

static VfsReleaseList vfs_release_list_new(FlArena* arena, u64 capacity) {
    return (VfsReleaseList) { .items = arena_alloc_array(arena, VfsRelease, capacity), .capacity = capacity };
}

static void vfs_release_list_push(VfsReleaseList* list, VfsRelease release) {
    FL_ASSERT(list->count < list->capacity);
    list->items[list->count++] = release;
}

// Call with handle_lock released.
static void vfs_release_list_run_all(VfsReleaseList* list) {
    for (u64 i = 0; i < list->count; ++i) {
        vfs_run_release(list->items[i]);
    }
    list->count = 0;
}

// Caller holds handle_lock and runs the returned release after letting it go. A chained read/write handle
// references a file_handle but never owns it, and a VfsOp_ReadAll handle has no chained file handle at all,
// so no free here recurses into another handle.
static VfsRelease vfs_free_handle_locked(VfsState* self, VfsHandleData* handle_data) {
    VfsRelease release = { .fn = handle_data->release, .user_data = handle_data->user_data };
    if (handle_data->closed) {
        self->closed_count--;
    }
    vfs_free_handle_resources(self, handle_data);
    hashmap_remove(&self->handle_map, handle_data->id);
    pool_free(&self->handle_pool, handle_data);
    return release;
}

static bool vfs_handle_is_reclaimable(const VfsHandleData* handle_data, void* ctx) {
    UNUSED(ctx);
    return handle_data->closed && atomic_load_explicit(&handle_data->pending_dispatches, memory_order_acquire) == 0
           && vfs_jobs_finished(vfs_handle_jobs(handle_data->job_handle, handle_data->last_job));
}

// Frees every handle that was closed while its job was still running and whose jobs have finished since. A
// closed handle stays in the map until then, so its id is neither reachable nor recycled and the job keeps
// its raw pointer.
static void vfs_reclaim_closed_handles(VfsState* self) {
    mutex_lock(&self->handle_lock);
    if (self->closed_count == 0) {
        mutex_unlock(&self->handle_lock);
        return;
    }

    arena_scratch_auto(temp);
    VfsHandleData** done = arena_alloc_array(temp.arena, VfsHandleData*, hashmap_count(&self->handle_map));
    u64 done_count = vfs_collect_handles_locked(self, vfs_handle_is_reclaimable, nullptr, done);
    VfsReleaseList releases = vfs_release_list_new(temp.arena, done_count);

    for (u64 i = 0; i < done_count; ++i) {
        vfs_release_list_push(&releases, vfs_free_handle_locked(self, done[i]));
    }
    mutex_unlock(&self->handle_lock);

    vfs_release_list_run_all(&releases);
}

// The ops that run against memory the caller lent for their duration: vfs_read's buffer and the data of
// vfs_write_no_copy. vfs_write's copy is VFS-owned, but the two writes are not told apart here.
static bool vfs_op_borrows_caller_memory(VfsOperationType op_type) {
    return op_type == VfsOp_FileRead || op_type == VfsOp_FileWrite;
}

void vfs_close(FlVfsHandle handle) {
    profile_function_auto_nc("vfs:vfs_close", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    if (!self || handle == FL_VFS_HANDLE_INVALID) {
        return;
    }

    vfs_reclaim_closed_handles(self);

    VfsHandleSnapshot snap;
    if (!vfs_snapshot_handle(handle, &snap)) {
        return;
    }

    // Closing an op that borrows the caller's memory is the caller's signal that the memory may go, so it
    // cannot be deferred: wait first. The wait runs without handle_lock, which the job takes to look its
    // file handle up. last_job covers the reads and writes chained onto a file handle.
    bool borrows_caller_memory = vfs_op_borrows_caller_memory(snap.op_type);
    if (borrows_caller_memory) {
        vfs_wait_handle_jobs(vfs_handle_jobs(snap.job_handle, snap.last_job));
    }

    mutex_lock(&self->handle_lock);

    // Re-look up under the lock: the snapshot and wait ran without it, and a concurrent close may have
    // taken the handle already.
    VfsHandleData* handle_data = vfs_lookup_open_handle_locked(self, handle);
    if (!handle_data) {
        mutex_unlock(&self->handle_lock);
        return;
    }

    // An open scheduling window has a job that can reach this handle created but not yet recorded on it,
    // so it counts as running - for a file handle that covers a read or write mid-chain onto it.
    bool finished = atomic_load_explicit(&handle_data->pending_dispatches, memory_order_acquire) == 0
                    && vfs_jobs_finished(vfs_handle_jobs(handle_data->job_handle, handle_data->last_job));

    if (!finished) {
        // On a job worker the wait above did nothing, and a borrowing op cannot be deferred on any thread:
        // freeing now would pull VfsHandleData, the result buffer and the plugin file handle out from
        // under the running job. Both refuse and leave the handle open.
        if (!fl_jobs_is_main_thread()) {
            mutex_unlock(&self->handle_lock);
            logc_error(VFS_ID,
                       "vfs_close(%u) from a job worker cannot wait out the running operation; the handle stays "
                       "open. Close an in-flight handle from the main thread.",
                       handle);
            return;
        }
        if (borrows_caller_memory) {
            mutex_unlock(&self->handle_lock);
            logc_error(VFS_ID,
                       "vfs_close(%u): a read or write into caller memory was scheduled after the close began; "
                       "the handle stays open. Wait for it and close again.",
                       handle);
            return;
        }

        // Deferred: the job finishes on its own, uncancelled, and the reclaim sweep frees the handle after.
        handle_data->closed = true;
        self->closed_count++;
        mutex_unlock(&self->handle_lock);
        return;
    }

    VfsRelease release = vfs_free_handle_locked(self, handle_data);

    mutex_unlock(&self->handle_lock);

    vfs_run_release(release);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool vfs_cancel(FlVfsHandle handle) {
    profile_function_auto_nc("vfs:vfs_cancel", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    if (!self || handle == FL_VFS_HANDLE_INVALID) {
        return false;
    }

    // The store must happen under the lock: a concurrent vfs_close could otherwise free the struct
    // between lookup and store
    mutex_lock_auto(&self->handle_lock);
    VfsHandleData* handle_data = vfs_lookup_open_handle_locked(self, handle);

    if (!handle_data) {
        return false;
    }

    atomic_store_explicit(&handle_data->cancel_requested, true, memory_order_release);

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsHandle vfs_mount_read_all_with_options(FlVfsMount* mount, FlString relative_path, FlVfsReadCallback callback,
                                            void* user_data, FlVfsReleaseCallback release, FlVfsHandle reuse_handle) {
    FL_VALIDATE_RET(mount != nullptr, FL_VFS_HANDLE_INVALID);

    return allocate_op(g_vfs_state, &(VfsOpParams) {
                                        .mount = mount,
                                        .path = relative_path,
                                        .op_type = VfsOp_ReadAll,
                                        .callback = callback,
                                        .user_data = user_data,
                                        .release = release,
                                        .reuse_handle = reuse_handle,
                                    });
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Two-phase read: allocate handle first, dispatch later.

FlVfsHandle vfs_mount_read_all_prepare(FlVfsMount* mount, FlString relative_path, FlVfsReadCallback callback,
                                       void* user_data, FlVfsReleaseCallback release, FlVfsHandle reuse_handle) {
    FL_VALIDATE_RET(mount != nullptr, FL_VFS_HANDLE_INVALID);

    VfsHandleData* handle = allocate_handle(g_vfs_state, &(VfsOpParams) {
                                                             .mount = mount,
                                                             .path = relative_path,
                                                             .op_type = VfsOp_ReadAll,
                                                             .callback = callback,
                                                             .user_data = user_data,
                                                             .release = release,
                                                             .reuse_handle = reuse_handle,
                                                         });
    FL_VALIDATE_RET(handle != nullptr, FL_VFS_HANDLE_INVALID);

    FlVfsHandle id = handle->id;
    vfs_publish_handle(g_vfs_state, handle);

    return id;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_dispatch(FlVfsHandle handle) {
    VfsState* self = g_vfs_state;

    if (!self || handle == FL_VFS_HANDLE_INVALID) {
        return;
    }

    mutex_lock(&self->handle_lock);
    VfsHandleData* handle_data = vfs_lookup_open_handle_locked(self, handle);
    FlVfsMount* mount = handle_data ? handle_data->mount : nullptr;
    if (handle_data) {
        // Before scheduling: the worker can reach the job, and a caller can observe the handle, while
        // job_handle below is still 0.
        atomic_fetch_add_explicit(&handle_data->pending_dispatches, 1, memory_order_release);
    }
    mutex_unlock(&self->handle_lock);

    if (!handle_data) {
        return;
    }

    // Scheduling can execute the job inline on a worker thread, so it must run outside handle_lock.
    // Closing the handle while dispatching it is caller error: this raw pointer outlives the unlock.
    FlJobHandle job = vfs_ops_schedule_job(mount, vfs_ops_do_job, handle_data);

    mutex_lock(&self->handle_lock);
    if (vfs_lookup_handle_locked(self, handle) == handle_data) {
        handle_data->job_handle = job;
        handle_data->last_job = job;
        atomic_fetch_sub_explicit(&handle_data->pending_dispatches, 1, memory_order_release);
    }
    mutex_unlock(&self->handle_lock);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsHandle vfs_mount_open(FlVfsMount* mount, FlString relative_path, u32 flags) {
    FL_VALIDATE_RET(mount != nullptr, FL_VFS_HANDLE_INVALID);

    return allocate_op(g_vfs_state, &(VfsOpParams) {
                                        .mount = mount,
                                        .path = relative_path,
                                        .op_type = VfsOp_FileOpen,
                                        .open_flags = flags,
                                    });
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsSizeResult vfs_get_size(FlVfsHandle file_handle) {
    FlVfsSizeResult error_result = { .size = -1, .error = { .vfs_status = FlVfsStatus_GenericError } };

    if (file_handle == FL_VFS_HANDLE_INVALID) {
        return error_result;
    }

    VfsHandleSnapshot snap;
    if (!vfs_snapshot_handle(file_handle, &snap) || snap.op_type != VfsOp_FileOpen) {
        logc_error(VFS_ID, "Invalid file handle for get_size");
        return error_result;
    }

    if (!snap.plugin_file_handle) {
        logc_error(VFS_ID, "File not opened");
        return error_result;
    }

    VfsPluginEntry* plugin_entry = snap.plugin_entry;
    if (!plugin_entry || !plugin_entry->plugin->get_size) {
        logc_error(VFS_ID, "Plugin does not support get_size");
        return error_result;
    }

    // The empty relative path selects the already-open file the handle carries.
    return plugin_entry->plugin->get_size(plugin_entry->plugin_instance, snap.plugin_file_handle, (FlString) { 0 });
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Shared body of vfs_read / vfs_write_internal: allocate an op handle chained onto an open file handle.
// No raw file pointer escapes handle_lock, and the op handle is filled in and scheduled while still
// private, outside the lock. The publishing pass re-looks the file up: closing it while issuing an op is
// caller error, but must not corrupt the map - the op then fails cleanly when its own lookup misses.
typedef struct VfsFileOpParams {
    VfsOperationType op_type;
    u8* read_buffer;
    i64 read_size;
    const u8* write_data;
    i64 write_size;
    bool write_data_owned;
} VfsFileOpParams;

static FlVfsHandle vfs_chain_file_op(FlVfsHandle file_handle, const VfsFileOpParams* params, const char* op_name) {
    VfsState* self = g_vfs_state;

    FL_VALIDATE_RET(self != nullptr, FL_VFS_HANDLE_INVALID);

    mutex_lock(&self->handle_lock);

    VfsHandleData* file_data = vfs_lookup_open_handle_locked(self, file_handle);
    if (!file_data || file_data->op_type != VfsOp_FileOpen) {
        mutex_unlock(&self->handle_lock);
        logc_error(VFS_ID, "Invalid file handle for %s operation", op_name);
        return FL_VFS_HANDLE_INVALID;
    }

    FlVfsMount* mount = file_data->mount;
    FlJobHandle chain_after = file_data->last_job;

    // Raised before the unlock and lowered only once this op's job is recorded as the file handle's
    // last_job. Across that window last_job still names the *previous* op, so a vfs_close landing here with
    // that predecessor finished would otherwise see an idle handle and free VfsHandleData, its result buffer
    // and its plugin_file_handle - all of which the job scheduled below still reaches through file_handle.
    atomic_fetch_add_explicit(&file_data->pending_dispatches, 1, memory_order_release);

    mutex_unlock(&self->handle_lock);

    // Born through the one handle-birth path like every other op, so it gets the same atomic_init and the
    // same initial status rather than relying on memory_zero to stand in for them. A chained op carries no
    // path, no filter and no callback of its own: it is named by the file handle it chains onto.
    VfsHandleData* op_handle = allocate_handle(self, &(VfsOpParams) {
                                                         .mount = mount,
                                                         .op_type = params->op_type,
                                                     });
    if (!op_handle) {
        mutex_lock(&self->handle_lock);
        VfsHandleData* file_failed = vfs_lookup_handle_locked(self, file_handle);
        if (file_failed) {
            atomic_fetch_sub_explicit(&file_failed->pending_dispatches, 1, memory_order_release);
        }
        mutex_unlock(&self->handle_lock);
        return FL_VFS_HANDLE_INVALID;
    }

    // The rest is op-specific and stays here: the handle is private until published, so no lock is needed.
    u32 op_id = op_handle->id;
    op_handle->file_handle = file_handle;
    op_handle->read_buffer = params->read_buffer;
    op_handle->read_size = params->read_size;
    op_handle->write_data = params->write_data;
    op_handle->write_size = params->write_size;
    op_handle->write_data_owned = params->write_data_owned;

    bool chain_refused = false;

    if (!fl_jobs_is_main_thread()) {
        // A worker runs the op inline, which is only correct once chain_after has finished: it cannot wait
        // for the predecessor, and running anyway would read the file handle before its open filled it in.
        if (chain_after != 0 && fl_jobs_is_finished(chain_after) == FlJobsResult_NotFinished) {
            logc_error(VFS_ID,
                       "vfs_%s from a job worker cannot wait for the operation it chains onto; issue it from "
                       "the main thread",
                       op_name);
            atomic_store_explicit(&op_handle->error_status, VFS_ERROR_WOULD_BLOCK, memory_order_release);
            chain_refused = true;
        } else {
            vfs_ops_execute_sync(op_handle);
        }
        op_handle->job_handle = 0;
    } else {
        op_handle->job_handle = fl_jobs_add_job_with_dependency(vfs_ops_do_job, op_handle, chain_after);
    }
    op_handle->last_job = op_handle->job_handle;

    mutex_lock(&self->handle_lock);
    // The guard above keeps the file handle in the map for the whole window - a close inside it defers
    // rather than frees - so this lookup finds it whether or not it was closed meanwhile.
    VfsHandleData* file_now = vfs_lookup_handle_locked(self, file_handle);
    if (file_now) {
        if (!chain_refused) {
            // Next operation (or close) chains after this op; 0 for the synchronous path. A refused chain
            // never ran, so the predecessor it could not wait for is still what the file handle must wait
            // for.
            file_now->last_job = op_handle->job_handle;
        }
        // Lowered last: from here last_job covers this op, so the handle is free to be judged idle again.
        // A deferred close left the handle closed, and the reclaim sweep takes it once this reaches 0.
        atomic_fetch_sub_explicit(&file_now->pending_dispatches, 1, memory_order_release);
    }
    // Published last, as on every other path, and in the same critical section that drops the guard.
    hashmap_insert(&self->handle_map, op_id, op_handle);
    mutex_unlock(&self->handle_lock);

    return op_id;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsHandle vfs_read(FlVfsHandle file_handle, void* buffer, i64 size) {
    if (file_handle == FL_VFS_HANDLE_INVALID || !buffer || size <= 0) {
        return FL_VFS_HANDLE_INVALID;
    }

    VfsFileOpParams params = {
        .op_type = VfsOp_FileRead,
        .read_buffer = buffer,
        .read_size = size,
    };
    return vfs_chain_file_op(file_handle, &params, "read");
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static FlVfsHandle vfs_write_internal(FlVfsHandle file_handle, const u8* data, i64 size, bool data_owned) {
    if (file_handle == FL_VFS_HANDLE_INVALID || !data || size <= 0) {
        return FL_VFS_HANDLE_INVALID;
    }

    VfsFileOpParams params = {
        .op_type = VfsOp_FileWrite,
        .write_data = data,
        .write_size = size,
        .write_data_owned = data_owned,
    };
    return vfs_chain_file_op(file_handle, &params, "write");
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsHandle vfs_write(FlVfsHandle file_handle, const void* data, i64 size) {
    if (!data || size <= 0) {
        return FL_VFS_HANDLE_INVALID;
    }

    u8* data_copy = mi_malloc(size);
    FL_VALIDATE_RET(data_copy != nullptr, FL_VFS_HANDLE_INVALID);

    memory_copy(data_copy, size, data, size);
    return vfs_write_internal(file_handle, data_copy, size, true); // VFS owns the copy
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsHandle vfs_write_no_copy(FlVfsHandle file_handle, void* data, i64 size) {
    return vfs_write_internal(file_handle, (const u8*)data, size, false); // Caller owns the data
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mount close and lifecycle

static bool vfs_handle_is_on_mount(const VfsHandleData* handle_data, void* ctx) {
    return handle_data->mount == (const FlVfsMount*)ctx;
}

// One handle to be torn down with its mount, as vfs_mount_close needs it after handle_lock is released:
// the id to re-look it up by, and the jobs to drain before the mount's arena goes.
typedef struct VfsMountVictim {
    FlVfsHandle id;
    VfsHandleJobs jobs;
} VfsMountVictim;

// Snapshots every handle belonging to mount. Handles the caller issues against the mount after this are a
// close-ordering bug and are not reaped: they must be vfs_close()'d before the mount. Ids and jobs are
// copied out rather than the pointers, because a concurrent vfs_close() on a leaked handle may free one
// before the teardown pass gets to it.
static u64 vfs_snapshot_mount_victims(VfsState* self, FlVfsMount* mount, FlArena* arena, VfsMountVictim** out) {
    mutex_lock(&self->handle_lock);

    u64 map_count = hashmap_count(&self->handle_map);
    VfsHandleData** found = arena_alloc_array(arena, VfsHandleData*, map_count);
    VfsMountVictim* victims = arena_alloc_array(arena, VfsMountVictim, map_count);

    u64 count = vfs_collect_handles_locked(self, vfs_handle_is_on_mount, mount, found);
    for (u64 i = 0; i < count; i++) {
        victims[i].id = found[i]->id;
        victims[i].jobs = vfs_handle_jobs(found[i]->job_handle, found[i]->last_job);
    }

    mutex_unlock(&self->handle_lock);

    *out = victims;
    return count;
}

// Waits out this mount's in-flight ops before its tree, strings and arena are destroyed. Runs WITHOUT
// handle_lock, which the jobs themselves take to look their handles up.
static void vfs_drain_mount_victims(FlVfsMount* mount, const VfsMountVictim* victims, u64 count) {
    int inflight_jobs = 0;
    for (u64 i = 0; i < count; i++) {
        for (u32 j = 0; j < victims[i].jobs.count; j++) {
            FlJobHandle job = victims[i].jobs.handles[j];
            if (job && fl_jobs_is_finished(job) == FlJobsResult_NotFinished) {
                fl_jobs_wait(job);
                inflight_jobs++;
            }
        }
    }

    if (inflight_jobs > 0) {
        log_warning("vfs_mount_close: mount %p had %d in-flight job(s) still referencing it - "
                    "close handles before closing the mount",
                    (void*)mount, inflight_jobs);
    }

    // The mount's own job only after its operations are done, which settles shutdown ordering.
    if (mount->job_handle) {
        fl_jobs_wait(mount->job_handle);
    }
}

// Removes and frees the snapshotted handles. Their paths live in mount->strings, so this must run before
// nodes_arena is destroyed, and it frees resources directly rather than through vfs_close(), which would
// re-take handle_lock and re-wait the jobs. Each id is re-looked up because a concurrent vfs_close() on a
// leaked handle may already have removed it.
static void vfs_free_mount_victims(VfsState* self, const VfsMountVictim* victims, u64 count, VfsReleaseList* releases) {
    for (u64 i = 0; i < count; i++) {
        VfsHandleData** victim_ptr = hashmap_get(&self->handle_map, victims[i].id);
        VfsHandleData* victim_data = victim_ptr ? *victim_ptr : nullptr;
        if (victim_data) {
            vfs_release_list_push(releases, vfs_free_handle_locked(self, victim_data));
        }
    }
}

// True when mount is still in the mount list. A mount that is not is a double-close or a use-after-free.
static bool vfs_mount_is_listed(VfsState* self, const FlVfsMount* mount) {
    mutex_lock_auto(&self->mount_lock);
    for_each_list(m, self->mount_first) {
        if (m == mount) {
            return true;
        }
    }
    return false;
}

// Unlinks the mount from the list and destroys the storage it owns. Everything that could still reach it
// has been drained by now.
static void vfs_destroy_mount(VfsState* self, FlVfsMount* mount) {
    mutex_lock_auto(&self->mount_lock);

    sll_queue_remove(self->mount_first, self->mount_last, mount);

    // Jobs were drained above, so no other thread can hold tree_lock; destroy it
    // before the struct is zeroed and returned to the pool.
    mutex_destroy(&mount->tree_lock);

    // StringAllocator is allocated within nodes_arena, so destroying nodes_arena frees both
    arena_destroy(mount->nodes_arena);
    memory_zero(mount, sizeof(FlVfsMount));

    pool_free(&self->vfs_mounts, mount);
}

void vfs_mount_close(FlVfsMount* mount) {
    profile_function_auto_nc("vfs:vfs_mount_close", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    FL_VALIDATE(self != nullptr);
    FL_VALIDATE(mount != nullptr);

    if (!vfs_mount_is_listed(self, mount)) {
        logc_error(VFS_ID, "vfs_mount_close: mount %p not in list (double-close or use-after-free)", (void*)mount);
        return;
    }

    arena_scratch_auto(temp);
    VfsMountVictim* victims = nullptr;
    u64 victim_count = vfs_snapshot_mount_victims(self, mount, temp.arena, &victims);
    VfsReleaseList releases = vfs_release_list_new(temp.arena, victim_count);

    vfs_drain_mount_victims(mount, victims, victim_count);

    mutex_lock(&self->handle_lock);

    vfs_free_mount_victims(self, victims, victim_count, &releases);

    if (mount->watching_enabled && mount->watcher_handle != 0) {
        file_watcher_stop(mount->watcher_handle);
        mount->watcher_handle = 0;
        mount->watching_enabled = false;
    }

    vfs_tree_unlink_from_parent(mount);

    if (mount->root_node) {
        vfs_tree_cleanup_node(mount->root_node);
    }

    // Release handle_lock before acquiring mount_lock to maintain lock ordering
    // (vfs_mount_with_options acquires mount_lock -> handle_lock, so we must respect that order)
    mutex_unlock(&self->handle_lock);

    vfs_release_list_run_all(&releases);

    vfs_destroy_mount(self, mount);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool vfs_mount_enable_watching(FlVfsMount* mount) {
    profile_function_auto_nc("vfs:vfs_mount_enable_watching", PROFILE_COLOR_CYAN);
    FL_VALIDATE_RET(mount != nullptr, false);

    // Protect with tree_lock since op_mount reads this field while holding the lock
    mutex_lock(&mount->tree_lock);
    mount->enable_file_watching = true;
    mutex_unlock(&mount->tree_lock);
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Process file watcher changes for a mount - removes deleted nodes from tree

static void process_file_watcher_changes(FlVfsMount* mount, u32 watcher, FlArena* scratch) {
    FileChangeIterator iter = file_watcher_get_changes(watcher, scratch);

    while (file_change_iterator_has_next(&iter)) {
        FileChange change = file_change_iterator_next(&iter);

        // Process deletions - remove tree nodes to free name strings
        if (!(change.change_types & FlFileChangeType_Deleted)) {
            continue;
        }

        // Convert the (possibly absolute) watcher path to a mount-relative path before removing the node.
        FlString relative = vfs_tree_watcher_relative_path(change.path, mount->source_path);
        if (relative.length > 0) {
            vfs_tree_remove_node_by_path(mount, relative);
        }
    }

    // Increment version to invalidate cached listings
    atomic_fetch_add_explicit(&mount->version, 1, memory_order_release);
    file_watcher_clear_changes(watcher);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// VFS update and destroy

void vfs_update(void) {
    profile_function_auto_nc("vfs:vfs_update", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    FL_VALIDATE(self != nullptr);

    arena_scratch_auto(temp);

    // Collect mounts while holding mount_lock to avoid race with vfs_mount_with_options
    u32 mount_count = 0;
    mutex_lock(&self->mount_lock);
    for_each_list(m, self->mount_first) {
        mount_count++;
    }

    FlVfsMount** mounts = arena_alloc_array(temp.arena, FlVfsMount*, mount_count);
    u32 mount_idx = 0;
    for_each_list(m, self->mount_first) {
        mounts[mount_idx++] = m;
    }
    mutex_unlock(&self->mount_lock);

    // Now process file watchers without holding mount_lock
    for_count(j, mount_count) {
        FlVfsMount* mount = mounts[j];
        // Use tree_lock to safely read watcher fields (set by op_mount)
        mutex_lock(&mount->tree_lock);
        bool watching = mount->watching_enabled;
        u32 watcher = mount->watcher_handle;
        mutex_unlock(&mount->tree_lock);

        if (watching && watcher != 0 && file_watcher_has_changes(watcher)) {
            process_file_watcher_changes(mount, watcher, temp.arena);
        }
    }

    vfs_reclaim_closed_handles(self);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void vfs_destroy(void) {
    profile_function_auto_nc("vfs:vfs_destroy", PROFILE_COLOR_CYAN);
    VfsState* self = g_vfs_state;

    FL_VALIDATE(self != nullptr);

    vfs_wait_all();

    // No handle sweep here: every handle references a mount, and vfs_mount_close below frees every handle
    // still registered against it. Sweeping first would double-free, since vfs_free_handle_resources does
    // not clear the fields it frees.

    while (self->mount_first) {
        FlVfsMount* mount = self->mount_first;
        vfs_mount_close(mount);
    }

    // Pools and hashmaps are allocated from main_arena, which is owned by the caller (core system)
    hashmap_clear(&self->handle_map);

    // All mounts are closed and no jobs remain, so nothing can take these locks
    // again; destroy them before the state becomes unreachable.
    mutex_destroy(&self->mount_lock);
    mutex_destroy(&self->handle_lock);

    // Note: VFS does not own the plugin registry - the application manages plugin lifecycle

    g_vfs_state = nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mount status and info

FlVfsMountStatus vfs_mount_get_status(FlVfsMount* mount) {
    profile_function_auto_nc("vfs:vfs_mount_get_status", PROFILE_COLOR_CYAN);
    FL_VALIDATE_RET(mount != nullptr, FlVfsMountStatus_Error);

    if (mount->job_handle) {
        FlJobsResult result = fl_jobs_is_finished(mount->job_handle);
        if (result == FlJobsResult_NotFinished) {
            return FlVfsMountStatus_Pending;
        }
    }

    if (vfs_tree_root_is_resolved(mount)) {
        return FlVfsMountStatus_Ready;
    }

    return FlVfsMountStatus_Error;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlVfsMountInfo vfs_mount_get_info(FlVfsMount* mount) {
    FL_VALIDATE_RET(mount != nullptr, ((FlVfsMountInfo) {
                                          .path = S(""),
                                          .is_ready = false,
                                      }));

    return (FlVfsMountInfo) {
        .path = mount->source_path,
        .is_ready = (vfs_mount_get_status(mount) == FlVfsMountStatus_Ready),
    };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool vfs_mount_is_ready(FlVfsMount* mount) {
    profile_function_auto_nc("vfs:vfs_mount_is_ready", PROFILE_COLOR_CYAN);
    return vfs_mount_get_status(mount) == FlVfsMountStatus_Ready;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FlJobHandle vfs_mount_get_job_handle(FlVfsMount* mount) {
    FL_VALIDATE_RET(mount != nullptr, 0);
    return mount->job_handle;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
VfsState* vfs_get_state(void) {
    profile_function_auto_nc("vfs:vfs_get_state", PROFILE_COLOR_CYAN);
    return g_vfs_state;
}
