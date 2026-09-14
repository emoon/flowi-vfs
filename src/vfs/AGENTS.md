# src/vfs/ - Virtual File System Core

The VFS core layer that provides unified async file access across local files, archives, HTTP, and nested archives. Internal code - no `fl_` prefix.

This directory is the VFS *engine*. The drivers live beside it in `src/vfs/drivers/`.

**LocalFS** is always compiled in and registered automatically by `vfs_init()` — native-filesystem access works out of the box. It is the catch-all fallback and is always tried last (see `VfsState::builtin_fallback`).

The **archive drivers** (libarchive, lhasa, unlzx) are each compiled in only when their `FLOWI_VFS_DRIVER_*` cmake option is on, and are registered explicitly — one call to `vfs_register_builtin_drivers()`, or individually through the descriptors `<flowi/vfs/drivers.h>` declares. flowi itself builds with all three off.

A driver that lives outside this repository (httpfs, and replay_frontend's loadable modules) is handed in the same way, via `vfs_register_driver()`. Nothing here ever dynamically loads driver code itself; `dlopen` is the embedder's business.

## Architecture

Singleton `VfsState* g_vfs_state` owns all mounts, handles, and the registered drivers. All operations (mount, list, read, write) are async via the job system.

### Mounting

```c
FlVfsMountResult result = vfs_mount_with_options(S("/home/user/music"), options);
FlVfsMount* mount = result.mount;
// Mount completes asynchronously - use vfs_wait() or check vfs_mount_is_ready()
```

Mounts create a `VfsTreeNode` tree that acts as both a sorted cache and the data structure for listings. Plugin handles are cloned per worker thread (2 threads) to avoid contention.

### Async Operations

All operations go through the job system:
1. `vfs_mount_with_options()` / `vfs_get_listing()` / `vfs_mount_read_all()` allocate a `VfsHandleData` and schedule a job
2. If the mount job is still running, operations chain via `jobs_add_job_with_dependency()`
3. Results are published atomically (`memory_order_release`) by the worker
4. Consumers read with `vfs_is_ready()` / `vfs_get_data()` / `vfs_get_file_list()` (`memory_order_acquire`)
5. **Always use `vfs_wait(handle)`** to block - never busy-poll

```c
FlVfsDirHandle dir = vfs_get_listing(mount, S(""), 0);
vfs_wait(dir);
FlVfsFileList list = vfs_get_file_list(dir);
// ... use list ...
vfs_close(dir);  // Frees the listing arena
```

### Directory Listing Lifecycle

1. `vfs_get_listing()` schedules a list job
2. Worker calls the plugin's `list_directory()` callback, builds/updates `VfsTreeNode` tree (sorted: dirs first, then natural sort)
3. Two-pass collection: count under tree lock, copy strings outside lock
4. Optional fuzzy filter via `match.h` scoring
5. Result stored atomically, snapshot version captured for staleness detection
6. `vfs_is_listing_current(handle)` compares snapshot against mount version (incremented by file watcher)

### File Watching

- `vfs_mount_enable_watching(mount)` before mount completes
- `vfs_update()` (called each frame) polls watchers, increments `mount->version` on changes
- Check staleness with `vfs_is_listing_current()`, re-list when stale

## Key Types

| Type | Purpose |
|------|---------|
| `FlVfsMount` | Mount instance: tree root, plugin handles, watcher, source path |
| `FlVfsHandle` / `FlVfsDirHandle` | `uint32_t` handle IDs into the handle pool (`0` = invalid) |
| `VfsHandleData` | Internal state behind every handle: job, result, status, arena |
| `VfsTreeNode` | Internal sorted tree node: name, plugin handle, children, archive flag |

## Files

| File | Purpose |
|------|---------|
| `vfs.h` | Public internal API: mount, list, read, write, wait, close |
| `vfs_internal.h` | All internal structs (FlVfsMount, VfsHandleData, VfsTreeNode, VfsState) |
| `vfs_private.h` | Cross-file prototypes, `g_vfs_state` global |
| `vfs.c` | Init, mount management, handle pool, public query functions |
| `vfs_ops.c` | Job-dispatched operations: mount, list, read, write, read_all |
| `vfs_tree.c` | Tree structure, path resolution, archive loading, plugin selection |
| `vfs_api_impl.c` | Fills `FlVfsAPI` vtable for the service API |

## How VFS Core Uses VFS Plugins

Interacts exclusively through the `FlVfsPlugin` vtable:

- `open()` - path-based opening (first plugin that returns a handle wins)
- `open_memory()` - for in-memory archives (plugin owns buffer on success)
- `list_directory()` - callback-based entry enumeration
- `read()` / `write()` / `get_size()` - file I/O
- `clone()` - create per-worker-thread handle copies
- `close()` - cleanup
- `can_open()` - used during archive detection
- `get_file_path()` - OS path passthrough (LocalFS optimization, avoids loading archives into RAM)
- `get_capabilities()` - feature detection bitmask

## Key Patterns

- **Lock ordering:** `handle_lock` before `tree_lock` (never reversed). Don't hold locks across I/O
- **Per-worker plugin handles:** `VfsHandles` stores `MAX_JOB_THREADS` (2) pre-cloned handles per tree node, indexed by `worker_index`. `vfs_init()` fails if the job system has more workers, and `vfs_tree_walk_and_resolve()` rejects out-of-range worker indices
- **Per-listing arenas:** Each listing gets its own arena, bulk-freed on `vfs_close()`
- **Cooperative cancellation:** `vfs_cancel(handle)` sets atomic flag; long operations check `vfs_should_cancel()`
- **Nil object:** `fl_nil_vfs_entry` returned instead of nullptr from lookups
- **Archive-in-archive:** Transparent nesting. Tries file-path-based open first (avoids RAM copy), falls back to `open_memory()`
- **Two-phase prepare/dispatch:** `vfs_mount_read_all_prepare()` + `vfs_dispatch()` for capturing handle ID before job runs
