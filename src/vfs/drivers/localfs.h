#pragma once

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// LocalFS - flowi's built-in VFS driver for the native filesystem.
//
// LocalFS is the one driver flowi ships, registered during vfs_init(). Drivers hosts add via
// vfs_register_driver() are always tried ahead of it, so LocalFS stays the catch-all fallback for plain
// filesystem paths.
//
// The driver is stateless - every open handle carries its own absolute OS path plus (for files) an OS file
// descriptor. All filesystem access goes through the platform-independent os.h primitives.

struct FlVfsPlugin;

// The singleton LocalFS driver.
const struct FlVfsPlugin* localfs_plugin(void);
