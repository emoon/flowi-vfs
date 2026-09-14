#pragma once

#include <core/arena.h>
#include <core/string.h>
#include <flowi/core/string.h>
#include <flowi/vfs/vfs_plugin.h>
#include <flowi/vfs/vfs_types.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Common types and helpers shared between archive-based VFS plugins (libarchive, lhasa, unlzx)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Cached archive entry for directory listing (linked list node)

typedef struct VfsCachedEntry {
    struct VfsCachedEntry* next;
    FlString path;     // Full path within archive (without trailing slash)
    int64_t size;      // File size (0 for directories)
    bool is_directory; // Whether this entry is a directory
} VfsCachedEntry;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: Check if archive entry path matches target path (handles trailing slash differences)

static inline bool vfs_archive_entry_path_matches(const char* entry_path, const char* target_cstr, size_t target_len) {
    if (!entry_path || !target_cstr) {
        return false;
    }

    // Exact match
    if (strcmp(entry_path, target_cstr) == 0) {
        return true;
    }

    size_t entry_len = strlen(entry_path);

    // Entry has trailing slash, target doesn't: "foo/" matches "foo"
    if (entry_len == target_len + 1 && entry_path[entry_len - 1] == '/'
        && strncmp(entry_path, target_cstr, target_len) == 0) {
        return true;
    }

    // Target has trailing slash, entry doesn't: "foo" matches "foo/"
    if (target_len > 0 && target_cstr[target_len - 1] == '/' && entry_len == target_len - 1
        && strncmp(entry_path, target_cstr, target_len - 1) == 0) {
        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper: enumerate the immediate children of `path` within a cached archive entry list.
//
// Emits each immediate child once (collapsing shared directory prefixes to a single entry) via
// `callback`, consistent with filesystem directory semantics. Passing "." or "" lists the archive
// root. Stops early if the callback returns false.

static inline void vfs_archive_list_children(VfsCachedEntry* entries, FlString path, FlVfsListingCallback callback,
                                             void* user_data) {
    // Normalize "." to empty string (root directory)
    if (string_equals(path, S("."))) {
        path = S("");
    }

    arena_scratch_auto(temp);

    // The count of unique immediate children can never exceed the total number of cached entries,
    // so sizing the de-dup set to that count guarantees no child is ever dropped.
    uint64_t entry_count = 0;
    for (VfsCachedEntry* cached = entries; cached; cached = cached->next) {
        entry_count++;
    }

    FlString* seen_names = arena_alloc_array(temp.arena, FlString, entry_count);
    uint64_t seen_count = 0;

    // Iterate through cached entries and filter for immediate children of the requested path
    for (VfsCachedEntry* cached = entries; cached; cached = cached->next) {
        FlString full_path = cached->path;

        // Filter by path prefix
        if (path.length > 0) {
            if (!string_begins_with(full_path, path)) {
                continue;
            }
            if (full_path.length == path.length) {
                continue;
            }
            if (full_path.data[path.length] != '/') {
                continue;
            }
            // Adjust to get relative path after prefix
            full_path = string_substr(full_path, path.length + 1, full_path.length - path.length - 1);
        }

        // Get immediate child name (first path component)
        int64_t slash_idx = string_find_char(full_path, '/');
        FlString child_name;
        bool is_dir;

        if (slash_idx >= 0) {
            child_name = string_substr(full_path, 0, (uint64_t)slash_idx);
            is_dir = true;
        } else {
            child_name = full_path;
            is_dir = cached->is_directory;
        }

        if (child_name.length == 0) {
            continue;
        }

        // Skip names already emitted (multiple entries can share the same directory prefix)
        bool already_seen = false;
        for (uint64_t j = 0; j < seen_count; j++) {
            if (string_equals(seen_names[j], child_name)) {
                already_seen = true;
                break;
            }
        }

        if (already_seen) {
            continue;
        }

        seen_names[seen_count++] = string_copy(temp.arena, child_name);

        FlVfsEntry vfs_entry = {
            .name = child_name,
            .is_directory = is_dir,
            .is_archive = false,
            .size = is_dir ? 0 : cached->size,
            .attributes = 0,
        };

        if (!callback(user_data, &vfs_entry)) {
            break;
        }
    }
}
