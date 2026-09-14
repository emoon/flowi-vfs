# flowi-vfs

The virtual filesystem [flowi](https://github.com/emoon/flowi) reads through, and nothing
above it: mount a directory, an archive or a URL at a mount point and read it all back
through one path space, asynchronously, on the job system. Built on
[flowi-core](https://github.com/emoon/flowi-core).

It is a separate repository because more than one project needs exactly this much, and
because a process must contain exactly one copy of it: a host and the plugins it dlopens all
resolve `vfs_*` to the same symbols, and two copies would mean two mount tables and two
handle spaces.

MIT. C11.

## What it is

A mount is opened from a source path; every read, listing and size query against it returns
a handle you poll rather than block on. Drivers do the actual work behind a plugin vtable
(`FlVfsPlugin`), and the path grammar makes an archive a directory: `/music/pack.lha/song.mod`
reads through the LHA driver without the caller knowing there is an archive in the path.

LocalFS is built in and always registered, as the catch-all fallback tried after every driver
registered later. The archive drivers are optional and off-able:

| Option | Driver | Default |
|---|---|---|
| `FLOWI_VFS_DRIVER_LIBARCHIVE` | zip, tar, tgz, 7z, rar, cab, iso, cpio | ON |
| `FLOWI_VFS_DRIVER_LHASA` | Amiga LHA, including the headers libarchive rejects | ON |
| `FLOWI_VFS_DRIVER_UNLZX` | Amiga LZX | ON |
| `FLOWI_VFS_SHARED` | build a shared `libflowi_vfs.so` instead of a static archive | OFF |

Each vendors the third-party library behind it (see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)) and is compiled only when its option is
on. flowi itself turns all three off: `libflowi.so` has never contained them.

## Building

flowi-core is a sibling checkout by default:

```sh
git clone https://github.com/emoon/flowi-core ../flowi-core
cmake -S . -B build
cmake --build build
```

`FLOWI_CORE_DIR` points elsewhere. An embedding project that has already added flowi-core
keeps its `flowi_core` target; this one is only added when nothing else did.

An embedder adds this with `add_subdirectory` and links `flowi_vfs`. The public headers are
`include/flowi/vfs/`, spelled `<flowi/vfs/vfs_api.h>` and so on — the same `flowi/` prefix
flowi-core's and flowi's headers use, so a generated header's cross-include resolves
whichever side owns the type.

Registering the drivers a build contains is one call, after `vfs_init`:

```c
#include <flowi/vfs/drivers.h>

vfs_init(arena);
vfs_register_builtin_drivers();
```

`<flowi/vfs/drivers.h>` also declares each driver's descriptor individually, for an embedder
that registers a subset or fills in the `plugin_info` pointer the library leaves null.

## Rust

`rust/` is its own cargo workspace with two crates:

- **`flowi_vfs_sys`** — the raw ABI. Generated bindings plus the two hand-written entry
  points the IDL has no shape for. All the `unsafe`.
- **`flowi_vfs`** — the safe surface: `Vfs` opens a mount, `Mount` carries every operation,
  `WorkerMount` is the blocking view a job body reads through, and results arrive as a typed
  `Handle<T>` that polls to `Result<T, FlError>` and closes its C ticket on drop. A
  frame-ticked `LocalExecutor` turns those handles into awaitable leaves.

By default `flowi_vfs_sys` builds the C library itself with cmake. A host whose own build
already links something containing the VFS turns the `native` feature off, or sets one of
`FLOWI_VFS_SYS_SKIP_NATIVE_LINK`, `FLOWI_VFS_SYS_STATIC_LINK` or `FLOWI_VFS_SYS_LIB_DIR`.
Two copies of the VFS in one process is the thing to avoid.

## Codegen

The public headers, the Rust bindings and the linker export lists are generated from the IDL
in `api/*.def` by [api-gen](https://github.com/emoon/api-gen), consumed as a library from
`codegen/` — a dev-only crate in its own workspace, so the generator's dependencies never
reach anything this repository ships. `cargo regen` regenerates; `cargo regen-check` is the
drift gate CI runs. Generated files are committed, so building needs no Rust toolchain; only
regenerating does.

flowi-core's `api/` is named in api-gen's `reference_dirs`: its defs are parsed so the VFS's
own can embed a core type by value, and nothing is emitted for them. A project layered on top
of this one does the same with both directories.
