# Third-party licenses

flowi-vfs itself is MIT (see [LICENSE](LICENSE)). The three libraries vendored under
`external/` back the optional archive drivers and keep their own licenses. All three permit
redistribution in source and binary form provided their notices are retained.

| Library | License | Copyright | License text | Built when |
|---|---|---|---|---|
| [libarchive](https://github.com/libarchive/libarchive) | BSD 2-Clause | Tim Kientzle and contributors | `external/libarchive/COPYING` | `FLOWI_VFS_DRIVER_LIBARCHIVE` |
| [lhasa](https://github.com/fragglet/lhasa) | ISC | 2011-2025 Simon Howard | `external/lhasa/COPYING.md` | `FLOWI_VFS_DRIVER_LHASA` |
| unlzx | Public domain | Erik Meusel, Dan Fraser; LZX by Jonathan Forbes and Tomi Poutanen | notice at the top of `external/unlzx/unlzx.c` | `FLOWI_VFS_DRIVER_UNLZX` |

Each is added to the build only when its driver option is on, so a build with all three off
compiles none of this code and links none of it.

zlib, bzip2 and liblzma are system dependencies of libarchive, found rather than vendored,
so their notices travel with whatever copies the platform provides. Which of them libarchive
is allowed to use is pinned in `CMakeLists.txt` rather than probed, so the dependency set is
the same everywhere.

lhasa is vendored without its `test/` directory, which is 12 MB of archive fixtures for
upstream's own test suite and is not part of the library. Everything the build compiles -
`lib/`, with the hand-written `CMakeLists.txt` beside it - is an upstream copy. libarchive
and unlzx are vendored whole, libarchive with the `libarchive_preconfig*.cmake` files that
answer its configure probes up front.

The VFS engine and the LocalFS driver are flowi's own code and carry no third-party notice.
