//! Files built into the app (old/src/core/embedded.cpp): launcher.json and the covers, found by the C++ resource name
//! of their path in launcher/ ("covers\gunmayhem2.png" is F_COVERS_GUNMAYHEM2_PNG). A real file next to the app wins.

include!(concat!(env!("OUT_DIR"), "/embedded_files.rs"));

pub fn embedded_name(relative_path: &str) -> String {
    let mut name = String::from("F_");
    for ch in relative_path.chars() {
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

/// The built-in copy of `relative_path`.
pub fn read_embedded(relative_path: &str) -> Option<&'static [u8]> {
    let name = embedded_name(relative_path);
    FILES.iter().find(|(n, _)| *n == name).map(|(_, bytes)| *bytes)
}
