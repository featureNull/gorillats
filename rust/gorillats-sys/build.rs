// Build the gorillats C core and link it into this crate.
use std::path::PathBuf;

fn main() {
    // Crate is at <repo>/rust/gorillats-sys; the C core lives at <repo>/src.
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..");
    let src = repo.join("src").join("gorillats.c");
    let include = repo.join("include");

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
