// Emits the link directives cargo needs to bind the flowi-vfs C library. By default
// it builds the library itself with cmake; a driving build system can instead point it at
// a prebuilt shared library, hand it a static archive closure, or tell it to link nothing
// at all (see the three environment variables below).
//
// The common "link nothing" case has a cargo feature of its own rather than an
// environment variable: a host whose own build already links a library that bundles the
// VFS - flowi's libflowi.so does - turns `native` off, and every `vfs_*` call resolves
// from there. Two copies of the VFS in one process would mean two mount tables and two
// handle spaces, which is exactly the kind of thing the plugin export check exists to
// prevent.
//
// The library this builds bundles flowi-core as well, so a dependent turns
// flowi_core_sys's `native` off for the same reason - see this crate's Cargo.toml.

use std::path::Path;

fn main() {
    // Hosts that statically link the VFS into their executable and export it unversioned
    // load plugins whose calls must stay plain undefined symbols the dynamic loader
    // resolves at dlopen. Linking the library into such a plugin cdylib would both add an
    // unresolvable DT_NEEDED and version-tag the references, so those plugin builds opt
    // out of the native build + link entirely.
    println!("cargo:rerun-if-env-changed=FLOWI_VFS_SYS_SKIP_NATIVE_LINK");
    if std::env::var_os("FLOWI_VFS_SYS_SKIP_NATIVE_LINK").is_some_and(|v| v == "1") {
        return;
    }
    // The same decision, taken by a cargo dependent instead of a build system.
    if std::env::var_os("CARGO_FEATURE_NATIVE").is_none() {
        return;
    }

    // FLOWI_VFS_SYS_STATIC_LINK is the other host shape: the VFS built by the driving build
    // system as a static archive and linked into the binary cargo produces, together with
    // the third-party archives and system libraries that archive needs. The value is a
    // colon-separated list of link inputs, each either an absolute path to a static
    // archive or shared library, `framework=<Name>`, or a bare library name for the
    // system linker. Nothing is bundled into the rlib: the archives are named by search
    // path and linked once, at the final link, in the order given.
    println!("cargo:rerun-if-env-changed=FLOWI_VFS_SYS_STATIC_LINK");
    if let Some(inputs) = std::env::var_os("FLOWI_VFS_SYS_STATIC_LINK").filter(|v| !v.is_empty()) {
        let inputs = inputs.to_string_lossy().into_owned();
        for input in inputs.split(':').filter(|s| !s.is_empty()) {
            emit_static_link_input(input);
        }
        return;
    }

    // FLOWI_VFS_SYS_LIB_DIR names a directory holding a prebuilt shared library: the
    // cmake build is skipped and the link directives point there instead, so every
    // consumer in the process links that ONE library rather than a second copy in cargo's
    // OUT_DIR. The host owns the library's configuration in this mode.
    println!("cargo:rerun-if-env-changed=FLOWI_VFS_SYS_LIB_DIR");
    if let Some(lib_dir) = std::env::var_os("FLOWI_VFS_SYS_LIB_DIR").filter(|v| !v.is_empty()) {
        let lib_dir = Path::new(&lib_dir);
        assert!(
            lib_dir.is_dir(),
            "FLOWI_VFS_SYS_LIB_DIR does not name a directory: {} — build the VFS \
             shared library there first (or unset the variable to let this script \
             build it)",
            lib_dir.display()
        );
        // The driving build system rebuilds the library in place; watch the dir so cargo
        // re-evaluates when it (or its symlinks) change.
        println!("cargo:rerun-if-changed={}", lib_dir.display());
        println!("cargo:rustc-link-search=native={}", lib_dir.display());
        println!("cargo:rustc-link-lib=dylib=flowi_vfs");
        // Same rpath as the self-built branch below: this crate's own artifacts run
        // outside cargo's library-path injection.
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
        return;
    }

    let repo_root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent() // rust/
        .and_then(Path::parent) // repo root
        .expect("flowi_vfs_sys must live at <repo>/rust/flowi_vfs_sys")
        .to_path_buf();

    // flowi-core is a sibling checkout, not a subdirectory. cmake defaults to the same
    // relative location, so this only has to be passed on when the caller overrode it.
    let mut config = cmake::Config::new(&repo_root);
    if let Some(core_dir) = std::env::var_os("FLOWI_CORE_DIR").filter(|v| !v.is_empty()) {
        println!("cargo:rerun-if-env-changed=FLOWI_CORE_DIR");
        config.define("FLOWI_CORE_DIR", core_dir);
    }
    let dst = config
        // Keep the allocator the C side would pick on its own; a consumer that wants
        // mimalloc configures the cmake build directly instead.
        .define("FLOWI_VFS_SHARED", "ON")
        .build_target("flowi_vfs")
        .build();
    let build = dst.join("build");

    // Cargo cannot infer dependencies consumed by cmake outside this crate, so track
    // every source/configuration tree that can change the VFS's code or public ABI.
    for input in [
        "src/vfs",
        "include/flowi",
        "api",
        "external",
        "CMakeLists.txt",
    ] {
        let path = repo_root.join(input);
        assert!(
            path.exists(),
            "native build input does not exist: {}",
            path.display()
        );
        println!("cargo:rerun-if-changed={}", path.display());
    }

    // Its third-party deps are bundled inside it and its system deps are DT_NEEDED, so
    // this is the only link directive. The search dir propagates transitively (via the
    // links = "flowi_vfs" key), and cargo adds every build-script link-search=native dir
    // to the dynamic-library path when it runs this workspace's tests/binaries - so cargo
    // test finds the library at runtime without LD_LIBRARY_PATH.
    println!("cargo:rustc-link-search=native={}", build.display());
    println!("cargo:rustc-link-lib=dylib=flowi_vfs");

    // For running THIS crate's own artifacts directly (outside cargo, where the path
    // injection above does not apply): bake the build dir as an rpath. Link args do not
    // cross crate bounds, so downstream binaries rely on the cargo path injection.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", build.display());
}

// One FLOWI_VFS_SYS_STATIC_LINK entry as cargo link directives. A path is split into its
// directory (a native search path) and the library name the linker expects (`libfoo.a` ->
// `foo`); a name is handed to the system linker as-is.
fn emit_static_link_input(input: &str) {
    if let Some(framework) = input.strip_prefix("framework=") {
        println!("cargo:rustc-link-lib=framework={framework}");
        return;
    }
    let path = Path::new(input);
    if !path.is_absolute() {
        println!("cargo:rustc-link-lib={input}");
        return;
    }
    let dir = path
        .parent()
        .unwrap_or_else(|| panic!("FLOWI_VFS_SYS_STATIC_LINK entry has no directory: {input}"));
    let file = path
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or_else(|| panic!("FLOWI_VFS_SYS_STATIC_LINK entry has no file name: {input}"));
    let stem = file.strip_prefix("lib").unwrap_or(file);
    println!("cargo:rerun-if-changed={}", path.display());
    println!("cargo:rustc-link-search=native={}", dir.display());
    if let Some(name) = stem.strip_suffix(".a") {
        println!("cargo:rustc-link-lib=static:-bundle={name}");
    } else {
        // `libfoo.so`, `libfoo.so.1`, `libfoo.dylib`: a shared library linked by name.
        let name = stem.split(".so").next().unwrap_or(stem);
        let name = name.strip_suffix(".dylib").unwrap_or(name);
        println!("cargo:rustc-link-lib=dylib={name}");
    }
}
