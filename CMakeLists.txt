cmake_minimum_required(VERSION 3.16)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(flowi_vfs C)

# An embedder adds this with add_subdirectory and links `flowi_vfs`.

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(EXTERNAL ${ROOT}/external)
set(SRC ${ROOT}/src)
set(VFS ${SRC}/vfs)

# flowi-core is the foundation: the arena, strings, the job system, paths, logging and the
# file watcher. The default points at a sibling checkout, which is both what a standalone
# clone gets and where flowi's own submodule already sits relative to this directory - so
# the same value is right whether this is built on its own or as part of flowi.
set(FLOWI_CORE_DIR "${ROOT}/../flowi-core" CACHE PATH "flowi-core checkout")

list(APPEND CMAKE_MODULE_PATH "${FLOWI_CORE_DIR}/cmake")
include(TargetConfig)

# An embedder that already added flowi-core (flowi does, before this) keeps its target;
# adding the same directory twice is a hard cmake error.
if(NOT TARGET flowi_core)
    add_subdirectory(${FLOWI_CORE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/_dep/flowi_core)
endif()

# LocalFS is not here: it is the built-in fallback, always compiled in, registered by
# vfs_init(). These three are archive formats with vendored third-party libraries behind
# them, so each is a real cost to carry and each is opt-out. flowi turns all three off -
# libflowi.so has never contained them and its curated export surface must not change.
option(FLOWI_VFS_DRIVER_LIBARCHIVE "Build the libarchive driver (ZIP, TAR, 7Z, RAR, ...)" ON)
option(FLOWI_VFS_DRIVER_LHASA "Build the lhasa driver (Amiga LHA)" ON)
option(FLOWI_VFS_DRIVER_UNLZX "Build the unlzx driver (Amiga LZX)" ON)
option(FLOWI_VFS_SHARED "Build the VFS as a shared library" OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

################################################################################
# Vendored driver libraries
#
# Each is added only when its driver is on, so a build without the driver neither
# configures nor compiles it.

if(FLOWI_VFS_DRIVER_LIBARCHIVE AND NOT TARGET archive_static)
    set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
    set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
    set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
    set(ENABLE_UNZIP OFF CACHE BOOL "" FORCE)
    set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_ACL OFF CACHE BOOL "" FORCE)
    set(ENABLE_XATTR OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    # Pin the codec set rather than taking whatever happens to be installed, so a clone
    # builds the same library everywhere and the shared build's DT_NEEDED list is a fact
    # rather than a property of the build machine.
    #
    # On is everything the driver's own extension list can reach: zlib for zip and tgz,
    # bzip2 for tar.bz2, lzma for tar.xz / tar.lzma / 7z, and the OpenSSL digests the
    # preconfig already selected, which is what reads an encrypted zip or 7z.
    set(ENABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(ENABLE_BZip2 ON CACHE BOOL "" FORCE)
    set(ENABLE_LZMA ON CACHE BOOL "" FORCE)
    set(ENABLE_OPENSSL ON CACHE BOOL "" FORCE)
    # Off is everything it cannot: libxml2 and expat serve XAR alone, and the remaining
    # codecs serve no extension the driver claims. Leaving libxml2 on is also what breaks
    # the build outright on a machine that has the library but puts its headers in
    # /usr/include/libxml2 - detection succeeds and the include path does not follow.
    set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
    set(ENABLE_EXPAT OFF CACHE BOOL "" FORCE)
    set(ENABLE_WIN32_XMLLITE OFF CACHE BOOL "" FORCE)
    set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
    set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
    set(ENABLE_LZO OFF CACHE BOOL "" FORCE)
    set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
    set(ENABLE_PCREPOSIX OFF CACHE BOOL "" FORCE)
    set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)

    # libarchive's own configure step probes ~200 headers and functions. The preconfig
    # files answer those probes up front, which is the difference between a two-second
    # and a forty-second cmake run, and is what makes cross-compilation possible at all.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        include(${EXTERNAL}/libarchive/libarchive_preconfig.cmake)
    elseif(APPLE)
        include(${EXTERNAL}/libarchive/libarchive_preconfig_macos.cmake)
    elseif(WIN32)
        include(${EXTERNAL}/libarchive/libarchive_preconfig_windows.cmake)
    endif()

    add_subdirectory(${EXTERNAL}/libarchive ${CMAKE_CURRENT_BINARY_DIR}/_dep/libarchive)
    if(TARGET archive_static)
        target_compile_options(archive_static PRIVATE -w)
    endif()
endif()

if(FLOWI_VFS_DRIVER_LHASA AND NOT TARGET lhasa)
    add_subdirectory(${EXTERNAL}/lhasa ${CMAKE_CURRENT_BINARY_DIR}/_dep/lhasa)
endif()

if(FLOWI_VFS_DRIVER_UNLZX AND NOT TARGET unlzx)
    add_subdirectory(${EXTERNAL}/unlzx ${CMAKE_CURRENT_BINARY_DIR}/_dep/unlzx)
endif()

################################################################################

set(VFS_SOURCES
    ${VFS}/vfs.c
    ${VFS}/vfs_ops.c
    ${VFS}/vfs_tree.c
    ${VFS}/drivers/localfs.c
    ${VFS}/drivers/registry.c)

if(FLOWI_VFS_DRIVER_LIBARCHIVE)
    list(APPEND VFS_SOURCES ${VFS}/drivers/libarchive.c)
endif()
if(FLOWI_VFS_DRIVER_LHASA)
    list(APPEND VFS_SOURCES ${VFS}/drivers/lhasa.c)
endif()
if(FLOWI_VFS_DRIVER_UNLZX)
    list(APPEND VFS_SOURCES ${VFS}/drivers/unlzx.c)
endif()

if(FLOWI_VFS_SHARED)
    set(FLOWI_VFS_TYPE SHARED)
else()
    set(FLOWI_VFS_TYPE STATIC)
endif()

add_library(flowi_vfs ${FLOWI_VFS_TYPE} ${VFS_SOURCES})

target_compile_definitions(flowi_vfs PRIVATE FL_API_IMPLEMENTATION=1)

# PUBLIC, and spelled out in both directions: <flowi/vfs/drivers.h> declares each accessor
# under `#if FLOWI_VFS_DRIVER_*`, so a consumer must see the same answer this library was
# built with or it will declare an accessor that does not exist.
foreach(driver LIBARCHIVE LHASA UNLZX)
    if(FLOWI_VFS_DRIVER_${driver})
        target_compile_definitions(flowi_vfs PUBLIC FLOWI_VFS_DRIVER_${driver}=1)
    else()
        target_compile_definitions(flowi_vfs PUBLIC FLOWI_VFS_DRIVER_${driver}=0)
    endif()
endforeach()

if(FLOWI_VFS_DRIVER_LIBARCHIVE)
    target_link_libraries(flowi_vfs archive_static)
    target_compile_definitions(flowi_vfs PRIVATE LIBARCHIVE_STATIC)
endif()
if(FLOWI_VFS_DRIVER_LHASA)
    target_link_libraries(flowi_vfs lhasa)
endif()
if(FLOWI_VFS_DRIVER_UNLZX)
    target_link_libraries(flowi_vfs unlzx)
endif()

if(FLOWI_VFS_SHARED)
    set_target_properties(flowi_vfs PROPERTIES VERSION 1.0.0 SOVERSION 1)
    if(WIN32)
        target_compile_definitions(flowi_vfs PUBLIC BASE_STATIC=1 FL_API=)
        set_target_properties(flowi_vfs PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
endif()

# ${SRC} resolves the <vfs/vfs.h> spelling flowi's own code uses for the internal header;
# ${ROOT}/include resolves the api_gen-emitted <flowi/vfs/x.h> public surface. fzy's
# match.h, which the fuzzy listing filter needs, arrives transitively from flowi_core.
target_include_directories(flowi_vfs PUBLIC ${SRC} ${ROOT} ${ROOT}/include)
target_link_libraries(flowi_vfs flowi_core)

apply_target_config(flowi_vfs)
