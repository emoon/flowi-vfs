//! flowi-vfs's api_gen configuration, shared by the regenerate binary and the drift test.
//!
//! The foundation's 12 defs are NOT here. They belong to flowi-core, which owns their
//! headers, bindings and exported symbols and has a `codegen/` and a drift gate of its
//! own. This target names that directory in `reference_dirs` instead: the VFS defs then
//! resolve the layout of a core type they embed by value - `FlString` in half the driver
//! signatures - while emitting nothing for it.

use std::path::{Path, PathBuf};

use api_gen::naming::Naming;
use api_gen::Config;

/// One codegen target: what it is called, which directories it owns, and how to configure it.
pub struct Target {
    /// Human name, used in drift reports.
    pub name: &'static str,
    /// Repo-relative directories this target writes into.
    pub outputs: &'static [&'static str],
    /// The config, with outputs rooted at `out_root`.
    pub config: Config,
}

/// Every codegen target, with outputs rooted at `out_root`. The IDL always comes from
/// `repo`; the two roots are the same tree for a real regeneration, and differ only for the
/// drift test, which writes to a temp dir and compares.
pub fn targets(repo: &Path, out_root: &Path) -> Vec<Target> {
    vec![Target {
        name: "VFS API",
        outputs: &[
            "include/flowi/vfs",
            "rust/flowi_vfs_sys/src/generated",
            "exports",
        ],
        config: Config {
            naming: Naming::new("Fl", "fl"),
            api_dir: repo.join("api"),
            // Parsed for their types, never emitted: flowi-core owns those headers,
            // bindings and exported symbols.
            reference_dirs: vec![core_dir(repo).join("api")],
            // The `flowi` prefix, not `flowi_vfs`: a VFS header includes a core type's
            // header as <flowi/core/string.h>, and all three repositories install under
            // that one prefix.
            c_include_root: Some(out_root.join("include/flowi")),
            c_include_prefix: Some("flowi".to_owned()),
            c_extern_c: true,
            rust_dir: Some(out_root.join("rust/flowi_vfs_sys/src/generated")),
            export_dir: Some(out_root.join("exports")),
            export_stem: Some("flowi_vfs".to_owned()),
            rust_allow_dead_code: true,
            gen_script: Some("cargo regen".to_owned()),
            ..Default::default()
        },
    }]
}

/// The repository root, one level above this crate.
pub fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("codegen/ sits at the repo root")
        .to_path_buf()
}

/// flowi-core's checkout. A sibling of this repository by default, which is both what a
/// standalone clone gets and where flowi's own submodule sits relative to `flowi-vfs/`.
/// `FLOWI_CORE_DIR` overrides it, matching the cmake cache variable of the same name.
pub fn core_dir(repo: &Path) -> PathBuf {
    std::env::var_os("FLOWI_CORE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| repo.join("../flowi-core"))
}
