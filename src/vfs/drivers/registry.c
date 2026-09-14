#include <flowi/vfs/drivers.h>

#include <vfs/vfs.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Bulk registration of the archive drivers this build contains.
//
// Order is specificity, most specific first: vfs_register_driver splices each one ahead of the
// LocalFS fallback, so the last registered is tried first. lhasa and unlzx handle Amiga archives
// libarchive either rejects or does not know, so they must be asked before it - hence libarchive
// registers first and ends up behind them.

void vfs_register_builtin_drivers(void) {
#if FLOWI_VFS_DRIVER_LIBARCHIVE
    vfs_register_driver(vfs_driver_libarchive(), nullptr);
#endif
#if FLOWI_VFS_DRIVER_LHASA
    vfs_register_driver(vfs_driver_lhasa(), nullptr);
#endif
#if FLOWI_VFS_DRIVER_UNLZX
    vfs_register_driver(vfs_driver_unlzx(), nullptr);
#endif
}
