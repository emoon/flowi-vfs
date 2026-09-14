#pragma once

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Built-in VFS drivers
//
// LocalFS is always present and vfs_init() registers it as the catch-all fallback; it has no entry
// here. The archive drivers are optional: each is compiled in only when its FLOWI_VFS_DRIVER_* cmake
// option is on, and its accessor is declared only then, so a call to one that was left out is a
// compile error rather than a link error.
//
// vfs_register_builtin_drivers() registers every driver this build contains, which is what an
// embedder that statically links the library wants. An embedder that loads drivers as separate
// modules ignores all of this and calls vfs_register_driver() itself.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <flowi/vfs/vfs_plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

// Register every archive driver compiled into this build, in specificity order. Call after
// vfs_init(); LocalFS stays the fallback and is tried last regardless. A build with every
// FLOWI_VFS_DRIVER_* off leaves this a no-op.
void vfs_register_builtin_drivers(void);

// The descriptors, for an embedder that registers a subset or publishes plugin metadata.
//
// Each is a pointer to the driver's own static descriptor, not a copy: writing through it
// changes what every later registration sees. The one field meant to be written is
// `plugin_info`, which this library leaves null and never dereferences - it is an
// embedder-owned tag array (replay's RpPluginInfo) that only the embedder's own UI reads.
#if FLOWI_VFS_DRIVER_LIBARCHIVE
FlVfsPlugin* vfs_driver_libarchive(void);
#endif
#if FLOWI_VFS_DRIVER_LHASA
FlVfsPlugin* vfs_driver_lhasa(void);
#endif
#if FLOWI_VFS_DRIVER_UNLZX
FlVfsPlugin* vfs_driver_unlzx(void);
#endif

#ifdef __cplusplus
}
#endif
