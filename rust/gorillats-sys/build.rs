// Build the gorillats C core and link it into this crate.
use std::path::PathBuf;

fn main() {
    // Vendored C core lives at <crate>/c/ so the crate is self-contained on crates.io.
    let crate_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let src = crate_dir.join("c").join("gorillats.c");
    let include = crate_dir.join("c");

    cc::Build::new()
        .file(&src)
        .include(&include)
        .std("c11")
        .warnings(true)
        .compile("gorillats");

    println!("cargo:rerun-if-changed={}", src.display());
    println!(
        "cargo:rerun-if-changed={}",
        include.join("gorillats").join("gorillats.h").display()
    );
}
