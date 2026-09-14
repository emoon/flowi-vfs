#include "localfs.h"

#include <core/arena.h>
#include <core/memory.h>
#include <core/os/os.h>
#include <core/path.h>
#include <core/string.h>
#include <flowi/vfs/vfs_plugin.h>
#include <flowi/vfs/vfs_types.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// One open resource. The driver is stateless: each handle carries the absolute OS path it was opened at, plus an
// OS file descriptor for an open file. The path buffer is heap-owned (mi_malloc) so it outlives the scratch
// arena the path was built on.

typedef struct LocalFsHandle {
    FlString path; // heap-owned absolute OS path (path.data from mi_malloc, NUL-terminated)
    i64 fd;        // >= 0 for an open file (read or write); -1 for a directory handle
    bool is_dir;
    bool is_write;
} LocalFsHandle;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static FlVfsDriverError localfs_ok(void) {
    return (FlVfsDriverError) { .vfs_status = FlVfsStatus_Success };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static FlVfsDriverError localfs_fail(void) {
    return (FlVfsDriverError) { .vfs_status = FlVfsStatus_FileNotFound };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Returns an empty string (data == nullptr) on OOM.

static FlString localfs_persist_path(FlString path) {
    char* buf = (char*)mi_malloc((size_t)path.length + 1);
    if (!buf) {
        return (FlString) { 0 };
    }
    if (path.length) {
        memcpy(buf, path.data, (size_t)path.length);
    }
    buf[path.length] = '\0';
    return (FlString) { .data = buf, .length = path.length, .is_static = 0, .is_ascii = 0 };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Resolve rel against a parent handle's OS path: an absolute rel is used verbatim, otherwise it is joined
// onto the parent directory.

static FlString localfs_resolve(FlArena* arena, const LocalFsHandle* parent, FlString rel) {
    FlString base = parent ? parent->path : (FlString) { 0 };
    return path_make_absolute(arena, base, rel);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Allocate a handle for os_path, opening an OS fd according to flags. Directories and flag-less opens get no
// fd. Returns nullptr on OOM or open failure.

static LocalFsHandle* localfs_make_handle(FlString os_path, bool is_dir, u32 flags) {
    i64 fd = -1;
    if (!is_dir) {
        if (flags & FlVfsOpenFlags_Write) {
            fd = file_open_write_os(os_path, (flags & FlVfsOpenFlags_Append) != 0);
        } else if (flags & FlVfsOpenFlags_Read) {
            fd = file_open_os(os_path, 0);
        }
        if ((flags & (FlVfsOpenFlags_Read | FlVfsOpenFlags_Write)) && fd < 0) {
            return nullptr;
        }
    }

    LocalFsHandle* h = mi_alloc(LocalFsHandle);
    if (!h) {
        if (fd >= 0) {
            file_close_handle_os(fd);
        }
        return nullptr;
    }

    h->path = localfs_persist_path(os_path);
    if (!h->path.data) {
        if (fd >= 0) {
            file_close_handle_os(fd);
        }
        mi_free(h);
        return nullptr;
    }

    h->fd = fd;
    h->is_dir = is_dir;
    h->is_write = (flags & FlVfsOpenFlags_Write) != 0;
    return h;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin callbacks. flowi's static vfs never calls create/destroy, so the driver instance is always nullptr -
// every callback ignores it.

static FlPluginCreateResult localfs_create(void) {
    return (FlPluginCreateResult) { .instance = nullptr, .error = localfs_ok() };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void localfs_destroy(void* instance) {
    (void)instance;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool localfs_supports_path(FlString path) {
    (void)path; // LocalFS is the catch-all driver for plain filesystem paths
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool localfs_can_open(FlString path) {
    return file_stat_os(path).is_valid;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static FlVfsOpenResult localfs_open(void* instance, void* parent_handle, FlString path, uint32_t flags,
                                    FlVfsMountOptions mount_options) {
    (void)instance;
    (void)mount_options;

    arena_scratch_auto(temp);
    FlString full = localfs_resolve(temp.arena, (const LocalFsHandle*)parent_handle, path);

    // A write open may target a file that does not exist yet; a read open requires an existing path.
    FileStat st = file_stat_os(full);
    bool want_write = (flags & FlVfsOpenFlags_Write) != 0;
    if (!st.is_valid && !want_write) {
        return (FlVfsOpenResult) { .handle = nullptr, .error = localfs_fail() };
    }

    bool is_dir = st.is_valid && st.is_directory;
    LocalFsHandle* h = localfs_make_handle(full, is_dir, flags);
    if (!h) {
        return (FlVfsOpenResult) { .handle = nullptr, .error = localfs_fail() };
    }
    return (FlVfsOpenResult) { .handle = h, .is_directory = h->is_dir, .error = localfs_ok() };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool localfs_is_directory(void* instance, void* handle) {
    (void)instance;
    LocalFsHandle* h = (LocalFsHandle*)handle;
    return h && h->is_dir;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static FlVfsSizeResult localfs_get_size(void* instance, void* handle, FlString relative_path) {
    (void)instance;
    LocalFsHandle* h = (LocalFsHandle*)handle;
    if (!h) {
        return (FlVfsSizeResult) { .size = -1, .error = localfs_fail() };
    }

    // Empty relative path: the size of the file this handle was opened on. Read from the stored path rather
    // than the fd so the fd's read offset is left undisturbed.
    if (relative_path.length == 0) {
        i64 size = file_get_size_os(h->path);
        if (size < 0) {
            return (FlVfsSizeResult) { .size = -1, .error = localfs_fail() };
        }
        return (FlVfsSizeResult) { .size = size, .error = localfs_ok() };
    }

    // Non-empty relative path: an existence/size probe against this directory handle. Directories report -1 so
    // the walk treats them as containers, not files.
    arena_scratch_auto(temp);
    FlString full = localfs_resolve(temp.arena, h, relative_path);
    FileStat st = file_stat_os(full);
    if (!st.is_valid || st.is_directory) {
        return (FlVfsSizeResult) { .size = -1, .error = localfs_fail() };
    }
    return (FlVfsSizeResult) { .size = (i64)st.size, .error = localfs_ok() };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static FlVfsReadResult localfs_read(void* instance, void* handle, FlString relative_path, uint8_t* buffer,
                                    int64_t size) {
    UNUSED(instance);
    LocalFsHandle* h = (LocalFsHandle*)handle;
    if (!h || !buffer || size < 0) {
        return (FlVfsReadResult) { .bytes_read = -1, .error = localfs_fail() };
    }

    // A non-empty relative path names a file under this handle and is read from its own fd starting at offset 0;
    // an empty one continues from the handle's current read offset. Flag-less handles carry no fd, so those open
    // one for the duration of the read too.
    i64 fd = h->fd;
    bool owns_fd = false;
    if (relative_path.length != 0) {
        arena_scratch_auto(temp);
        fd = file_open_os(localfs_resolve(temp.arena, h, relative_path), 0);
        owns_fd = true;
    } else if (h->is_dir) {
        return (FlVfsReadResult) { .bytes_read = -1, .error = localfs_fail() };
    } else if (fd < 0) {
        fd = file_open_os(h->path, 0);
        owns_fd = true;
    }
    if (fd < 0) {
        return (FlVfsReadResult) { .bytes_read = -1, .error = localfs_fail() };
    }

    // file_read_from_handle_os wraps one read(2), which may return short on large files, so loop until the
    // whole buffer is filled: the caller issues one read and trusts the returned count.
    i64 total = 0;
    while (total < size) {
        i64 remaining = size - total;
        i64 n = file_read_from_handle_os(buffer + total, fd, remaining, remaining);
        if (n < 0) {
            if (owns_fd) {
                file_close_handle_os(fd);
            }
            return (FlVfsReadResult) { .bytes_read = -1, .error = localfs_fail() };
        }
        if (n == 0) {
            break; // EOF
        }
        total += n;
    }
    if (owns_fd) {
        file_close_handle_os(fd);
    }
    return (FlVfsReadResult) { .bytes_read = total, .error = localfs_ok() };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void localfs_list_directory(void* instance, void* handle, FlString relative_path, FlVfsListingCallback callback,
                                   void* user_data) {
    (void)instance;
    LocalFsHandle* h = (LocalFsHandle*)handle;
    if (!h || !callback) {
        return;
    }

    arena_scratch_auto(temp);
    FlString dir_path = localfs_resolve(temp.arena, h, relative_path);
    DirHandle dir = dir_open_os(temp.arena, dir_path);
    if (!dir.is_valid) {
        return;
    }

    // dir_read_next_os already skips "." and ".." and stats each entry.
    for (FileStat e = dir_read_next_os(temp.arena, &dir); e.is_valid; e = dir_read_next_os(temp.arena, &dir)) {
        FlVfsEntry entry = {
            .name = e.name,
            .size = e.is_directory ? 0 : (i64)e.size,
            .attributes = 0,
            .is_directory = e.is_directory,
            .is_archive = false,
        };
        if (!callback(user_data, &entry)) {
            break;
        }
    }

    dir_close_os(&dir);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static FlVfsOpenResult localfs_clone(void* instance, void* handle) {
    (void)instance;
    LocalFsHandle* src = (LocalFsHandle*)handle;
    if (!src) {
        return (FlVfsOpenResult) { .handle = nullptr, .error = localfs_fail() };
    }

    // A cloned file handle needs its own independent offset, so the fd is reopened rather than shared. A write
    // handle reopens in append mode so the clone does not truncate what the source already wrote.
    u32 flags = 0;
    if (src->is_write) {
        flags = FlVfsOpenFlags_Write | FlVfsOpenFlags_Append;
    } else if (src->fd >= 0) {
        flags = FlVfsOpenFlags_Read;
    }
    LocalFsHandle* h = localfs_make_handle(src->path, src->is_dir, flags);
    if (!h) {
        return (FlVfsOpenResult) { .handle = nullptr, .error = localfs_fail() };
    }
    return (FlVfsOpenResult) { .handle = h, .is_directory = h->is_dir, .error = localfs_ok() };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void localfs_close(void* instance, void* handle) {
    (void)instance;
    LocalFsHandle* h = (LocalFsHandle*)handle;
    if (!h) {
        return;
    }
    if (h->fd >= 0) {
        file_close_handle_os(h->fd);
    }
    mi_free((void*)h->path.data);
    mi_free(h);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Hand back the real OS path for a resource (FlVfsDriverCapabilities_DirectPath), letting the core open
// on-disk archives by path instead of loading them into RAM. Empty relative_path means the handle's own file.

static FlString localfs_get_file_path(void* instance, void* handle, FlString relative_path, FlArena* arena) {
    (void)instance;
    LocalFsHandle* h = (LocalFsHandle*)handle;
    if (!h) {
        return (FlString) { 0 };
    }
    if (relative_path.length == 0) {
        return h->path; // heap-owned and stable; the caller copies if it needs to outlive the handle
    }
    return localfs_resolve(arena, h, relative_path);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// file_write_to_handle_os wraps a single write(2), which may short-write, so loop until the whole buffer is
// flushed or the OS reports an error.

static FlVfsWriteResult localfs_write(void* instance, void* handle, const uint8_t* buffer, int64_t size) {
    (void)instance;
    LocalFsHandle* h = (LocalFsHandle*)handle;
    if (!h || h->is_dir || h->fd < 0 || !h->is_write) {
        return (FlVfsWriteResult) { .bytes_written = -1, .error = { .vfs_status = FlVfsStatus_InvalidHandle } };
    }
    if (size < 0 || (!buffer && size > 0)) {
        return (FlVfsWriteResult) { .bytes_written = -1, .error = { .vfs_status = FlVfsStatus_PathInvalid } };
    }

    i64 total = 0;
    while (total < size) {
        i64 n = file_write_to_handle_os(h->fd, buffer + total, size - total);
        if (n < 0) {
            return (FlVfsWriteResult) { .bytes_written = -1, .error = { .vfs_status = FlVfsStatus_WriteError } };
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    return (FlVfsWriteResult) { .bytes_written = total, .error = localfs_ok() };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static FlVfsDriverCapabilities localfs_get_capabilities(void) {
    return (FlVfsDriverCapabilities)(FlVfsDriverCapabilities_Read | FlVfsDriverCapabilities_Write
                                     | FlVfsDriverCapabilities_List | FlVfsDriverCapabilities_Clone
                                     | FlVfsDriverCapabilities_DirectPath);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static FlVfsPlugin s_localfs_plugin = {
    .api_version = FL_VFS_PLUGIN_API_VERSION,
    .plugin_name = S_("LocalFS"),
    .plugin_info = nullptr,
    .create = localfs_create,
    .destroy = localfs_destroy,
    .supports_path = localfs_supports_path,
    .can_open = localfs_can_open,
    .open = localfs_open,
    .is_directory = localfs_is_directory,
    .read = localfs_read,
    .get_size = localfs_get_size,
    .list_directory = localfs_list_directory,
    .clone = localfs_clone,
    .close = localfs_close,
    .get_capabilities = localfs_get_capabilities,
    .open_memory = nullptr,
    .get_file_path = localfs_get_file_path,
    .is_cached = nullptr,
    .enable_watching = nullptr,
    .write = localfs_write,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const FlVfsPlugin* localfs_plugin(void) {
    return &s_localfs_plugin;
}
