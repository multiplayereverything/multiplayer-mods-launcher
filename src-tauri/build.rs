use std::path::{Path, PathBuf};

fn main() {
    embed_launcher_files();
    tauri_build::build()
}

/// launcher.json and the covers go into the app as they went into the C++ exe (old/src/core/embedded.h), so the
/// app still lists its games when no launcher.json sits next to it. Each file is keyed by the same resource name
/// the C++ build gives it ("F_" + its path in launcher/, upper-cased, every other character '_').
fn embed_launcher_files() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.parent().unwrap();
    let launcher = root.join("launcher");
    let mut files: Vec<PathBuf> = vec![launcher.join("launcher.json")];
    if let Ok(dir) = std::fs::read_dir(launcher.join("covers")) {
        let mut covers: Vec<PathBuf> = dir.filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| p.is_file()).collect();
        covers.sort();
        files.extend(covers);
    }
    println!("cargo:rerun-if-changed={}", launcher.join("launcher.json").display());
    println!("cargo:rerun-if-changed={}", launcher.join("covers").display());
    let mut out = String::from("pub static FILES: &[(&str, &[u8])] = &[\n");
    for f in files.iter().filter(|f| f.is_file()) {
        let rel = f.strip_prefix(&launcher).unwrap().to_string_lossy().to_string();
        out.push_str(&format!("    ({:?}, include_bytes!({:?})),\n", resource_name(&rel), f.to_string_lossy()));
    }
    out.push_str("];\n");
    // The version the C++ launcher reports (its CMake project version) stays the one the window shows while both
    // ship; once old/ is gone the app's own version takes over.
    let version = std::fs::read_to_string(root.join("old").join("CMakeLists.txt"))
        .ok()
        .and_then(|t| cmake_version(&t))
        .unwrap_or_else(|| std::env::var("CARGO_PKG_VERSION").unwrap());
    println!("cargo:rerun-if-changed={}", root.join("old").join("CMakeLists.txt").display());
    out.push_str(&format!("pub const LAUNCHER_VERSION: &str = {version:?};\n"));
    let dest = Path::new(&std::env::var("OUT_DIR").unwrap()).join("embedded_files.rs");
    std::fs::write(dest, out).unwrap();
}

fn resource_name(rel: &str) -> String {
    let mut name = String::from("F_");
    for ch in rel.chars() {
        if ch.is_ascii_lowercase() {
            name.push(ch.to_ascii_uppercase());
        } else if ch.is_ascii_uppercase() || ch.is_ascii_digit() {
            name.push(ch);
        } else {
            name.push('_');
        }
    }
    name
}

fn cmake_version(text: &str) -> Option<String> {
    let line = text.lines().find(|l| l.trim_start().starts_with("project("))?;
    let rest = &line[line.find(" VERSION ")? + 9..];
    let v: String = rest.chars().take_while(|c| c.is_ascii_digit() || *c == '.').collect();
    (!v.is_empty()).then_some(v)
}
