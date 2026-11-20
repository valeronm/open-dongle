use std::path::PathBuf;

fn main() {
    let dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let wire = dir.join("../wire");
    let config = cbindgen::Config::from_file(dir.join("cbindgen.toml")).unwrap();
    // The C build names the header's path, since it cannot find OUT_DIR.
    let header = PathBuf::from(
        std::env::var("DIAGWIRE_HEADER")
            .unwrap_or_else(|_| format!("{}/diagwire.h", std::env::var("OUT_DIR").unwrap())),
    );
    cbindgen::Builder::new()
        .with_config(config)
        .with_crate(&wire)
        .with_src(dir.join("src/lib.rs"))
        .generate()
        .unwrap()
        .write_to_file(&header);
    println!("cargo::rerun-if-changed=cbindgen.toml");
    println!("cargo::rerun-if-changed=src/lib.rs");
    println!("cargo::rerun-if-changed=../wire/src");
    println!("cargo::rerun-if-env-changed=DIAGWIRE_HEADER");
}
