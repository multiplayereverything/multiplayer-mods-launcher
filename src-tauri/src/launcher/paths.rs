//! Windows paths as the C++ launcher handles them (old/src/core/fs.cpp): backslashes, `..` collapsed lexically, no
//! trailing '\'. The lookups (file attributes, wildcards, PATH, known folders) call the same Windows functions the
//! C++ launcher calls, so a path that is found there is found here.

use std::ffi::OsString;
use std::os::windows::ffi::{OsStrExt, OsStringExt};

use windows_sys::Win32::Foundation::INVALID_HANDLE_VALUE;
use windows_sys::Win32::Storage::FileSystem::{
    FILE_ATTRIBUTE_DIRECTORY, FindClose, FindFirstFileW, FindNextFileW, GetFileAttributesW, GetTempPathW,
    INVALID_FILE_ATTRIBUTES, SearchPathW, WIN32_FIND_DATAW,
};

use super::text::{iequals, trim};

fn wide(s: &str) -> Vec<u16> {
    std::ffi::OsStr::new(s).encode_wide().chain(std::iter::once(0)).collect()
}

fn from_wide(w: &[u16]) -> String {
    OsString::from_wide(w).to_string_lossy().into_owned()
}

fn is_sep(c: char) -> bool {
    c == '\\' || c == '/'
}

/// std::filesystem's lexically_normal on Windows: one separator between parts, "." dropped, "name\.." cancelled,
/// ".." at a root dropped; a trailing separator is kept when the path ended on one, a "." or a cancelled "..".
fn lexically_normal(s: &str) -> String {
    if s.is_empty() {
        return String::new();
    }
    let chars: Vec<char> = s.chars().collect();
    let mut i = 0;
    let mut root_name = String::new();
    if chars.len() >= 2 && is_sep(chars[0]) && is_sep(chars[1]) && chars.get(2).is_some_and(|c| !is_sep(*c)) {
        // \\server
        i = 2;
        while i < chars.len() && !is_sep(chars[i]) {
            i += 1;
        }
        root_name = format!("\\\\{}", chars[2..i].iter().collect::<String>());
    } else if chars.len() >= 2 && chars[1] == ':' && chars[0].is_ascii_alphabetic() {
        root_name = chars[..2].iter().collect();
        i = 2;
    }
    let root_dir = i < chars.len() && is_sep(chars[i]);
    let rest: String = chars[i..].iter().collect();
    let raw: Vec<&str> = rest.split(is_sep).collect();
    let mut parts: Vec<&str> = Vec::new();
    let mut trailing = false;
    let n = raw.len();
    for (k, p) in raw.iter().enumerate() {
        let last = k + 1 == n;
        if p.is_empty() {
            if last && k > 0 {
                trailing = true;
            }
            continue;
        }
        trailing = false;
        if *p == "." {
            trailing = true;
            continue;
        }
        if *p == ".." {
            if parts.last().is_some_and(|l| *l != "..") {
                parts.pop();
                trailing = true;
            } else if root_dir {
                trailing = true;
            } else {
                parts.push("..");
            }
            continue;
        }
        parts.push(p);
    }
    let mut out = root_name;
    if root_dir {
        out.push('\\');
    }
    if parts.is_empty() {
        if out.is_empty() {
            return ".".into();
        }
        return out;
    }
    out.push_str(&parts.join("\\"));
    if trailing && parts.last() != Some(&"..") {
        out.push('\\');
    }
    out
}

/// Trims quotes and space, uses backslashes, collapses `..`, drops a trailing '\' (not a drive root's).
pub fn normalize(input: &str) -> String {
    let mut s = trim(input).to_string();
    while s.chars().count() >= 2 && s.starts_with('"') && s.ends_with('"') {
        s = trim(&s[1..s.len() - 1]).to_string();
    }
    if s.is_empty() {
        return String::new();
    }
    s = s.replace('/', "\\");
    if let Some(r) = s.strip_prefix("\\\\?\\UNC\\") {
        s = format!("\\\\{r}");
    } else if let Some(r) = s.strip_prefix("\\\\?\\") {
        s = r.to_string();
    }
    let mut out = lexically_normal(&s);
    while out.encode_utf16().count() > 3 && out.ends_with('\\') {
        out.pop();
    }
    let b = out.as_bytes();
    if b.len() == 2 && b[1] == b':' {
        out.push('\\');
    }
    let b = out.as_bytes();
    if b.len() >= 2 && b[1] == b':' {
        let first = out[..1].to_uppercase();
        out.replace_range(..1, &first);
    }
    out
}

pub fn is_absolute(p: &str) -> bool {
    let c: Vec<char> = p.chars().take(3).collect();
    if c.len() >= 3 && c[0].is_alphabetic() && c[1] == ':' && c[2] == '\\' {
        return true;
    }
    c.len() > 2 && c[0] == '\\' && c[1] == '\\'
}

pub fn join(a: &str, b: &str) -> String {
    if a.is_empty() {
        return b.to_string();
    }
    if b.is_empty() {
        return a.to_string();
    }
    let mut r = a.to_string();
    if !r.ends_with(is_sep) {
        r.push('\\');
    }
    r.push_str(b.trim_start_matches(is_sep));
    r.replace('/', "\\")
}

pub fn parent(path: &str) -> String {
    let Some(pos) = path.rfind(is_sep) else { return String::new() };
    let b = path.as_bytes();
    if pos == 2 && b.len() > 1 && b[1] == b':' {
        return if b.len() > 3 { path[..3].to_string() } else { String::new() };
    }
    if pos < 2 && path.starts_with("\\\\") {
        return String::new();
    }
    path[..pos].to_string()
}

pub fn file_name(path: &str) -> String {
    match path.rfind(is_sep) {
        Some(pos) => path[pos + 1..].to_string(),
        None => path.to_string(),
    }
}

/// Paths of 240 characters and more go to Windows with the \\?\ prefix, as in the C++ launcher.
fn long_path(p: &str) -> String {
    if p.encode_utf16().count() < 240 || p.starts_with("\\\\?\\") {
        return p.to_string();
    }
    if let Some(r) = p.strip_prefix("\\\\") {
        return format!("\\\\?\\UNC\\{r}");
    }
    format!("\\\\?\\{p}")
}

fn attrs(p: &str) -> u32 {
    if p.is_empty() {
        return INVALID_FILE_ATTRIBUTES;
    }
    let w = wide(&long_path(p));
    unsafe { GetFileAttributesW(w.as_ptr()) }
}

pub fn is_dir(p: &str) -> bool {
    let a = attrs(p);
    a != INVALID_FILE_ATTRIBUTES && a & FILE_ATTRIBUTE_DIRECTORY != 0
}

pub fn is_file(p: &str) -> bool {
    let a = attrs(p);
    a != INVALID_FILE_ATTRIBUTES && a & FILE_ATTRIBUTE_DIRECTORY == 0
}

/// Creates `dir` and its missing parents; false when a file is in the way or Windows refuses.
pub fn ensure_dir(dir: &str) -> bool {
    if dir.is_empty() || is_dir(dir) {
        return true;
    }
    if is_file(dir) {
        return false;
    }
    std::fs::create_dir_all(long_path(dir)).is_ok() || is_dir(dir)
}

pub fn read_file(path: &str) -> Option<Vec<u8>> {
    if path.is_empty() {
        return None;
    }
    std::fs::read(long_path(path)).ok()
}

pub fn write_file(path: &str, data: &[u8]) -> bool {
    std::fs::write(long_path(path), data).is_ok()
}

/// The file's SHA-256 in lower-case hex, empty when it can't be read.
pub fn sha256_file(path: &str) -> String {
    use sha2::{Digest, Sha256};
    use std::io::Read;
    let Ok(mut f) = std::fs::File::open(long_path(path)) else { return String::new() };
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        match f.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => h.update(&buf[..n]),
            Err(_) => return String::new(),
        }
    }
    h.finalize().iter().map(|b| format!("{b:02x}")).collect()
}

pub fn temp_dir() -> String {
    let mut buf = [0u16; 261];
    let n = unsafe { GetTempPathW(buf.len() as u32, buf.as_mut_ptr()) } as usize;
    normalize(&from_wide(&buf[..n.min(buf.len())]))
}

/// The folder this program runs from.
pub fn self_exe_dir() -> String {
    std::env::current_exe().map(|p| parent(&p.to_string_lossy())).unwrap_or_default()
}

/// GetFullPathName, as the C++ launcher resolves `--config`: relative to the current folder.
pub fn full_path(p: &str) -> String {
    if p.is_empty() {
        return String::new();
    }
    let n = normalize(p);
    match std::path::absolute(&n) {
        Ok(abs) => normalize(&abs.to_string_lossy()),
        Err(_) => n,
    }
}

/// Desktop, StartMenu, Documents, LocalAppData, RoamingAppData, ProgramFiles, ProgramFilesX86, SavedGames, Profile,
/// Windows, ProgramData, PublicDesktop, CommonStartMenu and Temp, for `${folder:...}`.
pub fn known_folder(name: &str) -> String {
    use windows_sys::Win32::System::Com::CoTaskMemFree;
    use windows_sys::Win32::UI::Shell::*;
    if iequals(name, "Temp") {
        return temp_dir();
    }
    let table = [
        ("Desktop", &FOLDERID_Desktop),
        ("StartMenu", &FOLDERID_Programs),
        ("Documents", &FOLDERID_Documents),
        ("LocalAppData", &FOLDERID_LocalAppData),
        ("RoamingAppData", &FOLDERID_RoamingAppData),
        ("ProgramFiles", &FOLDERID_ProgramFiles),
        ("ProgramFilesX86", &FOLDERID_ProgramFilesX86),
        ("SavedGames", &FOLDERID_SavedGames),
        ("Profile", &FOLDERID_Profile),
        ("Windows", &FOLDERID_Windows),
        ("ProgramData", &FOLDERID_ProgramData),
        ("PublicDesktop", &FOLDERID_PublicDesktop),
        ("CommonStartMenu", &FOLDERID_CommonPrograms),
    ];
    let Some((_, id)) = table.iter().find(|(n, _)| iequals(name, n)) else { return String::new() };
    let mut p: windows_sys::core::PWSTR = std::ptr::null_mut();
    let hr = unsafe { SHGetKnownFolderPath(*id, 0, std::ptr::null_mut(), &mut p) };
    let mut out = String::new();
    if hr >= 0 && !p.is_null() {
        let len = (0..).take_while(|&i| unsafe { *p.add(i) } != 0).count();
        out = from_wide(unsafe { std::slice::from_raw_parts(p, len) });
    }
    if !p.is_null() {
        unsafe { CoTaskMemFree(p as *const _) };
    }
    out
}

/// The files (not folders) a wildcard pattern matches, as FindFirstFile matches them.
pub fn find_files(pattern: &str) -> Vec<String> {
    let w = wide(&long_path(pattern));
    let mut fd: WIN32_FIND_DATAW = unsafe { std::mem::zeroed() };
    let h = unsafe { FindFirstFileW(w.as_ptr(), &mut fd) };
    if h == INVALID_HANDLE_VALUE {
        return Vec::new();
    }
    let mut names = Vec::new();
    loop {
        if fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY == 0 {
            let len = fd.cFileName.iter().position(|&c| c == 0).unwrap_or(fd.cFileName.len());
            names.push(from_wide(&fd.cFileName[..len]));
        }
        if unsafe { FindNextFileW(h, &mut fd) } == 0 {
            break;
        }
    }
    unsafe { FindClose(h) };
    names
}

/// A bare program name looked up the way SearchPath does (the program's folder, the current folder, the system
/// folders, then PATH). Empty when it isn't anywhere.
pub fn search_path(name: &str) -> String {
    let w = wide(name);
    let mut buf = vec![0u16; 260 * 4];
    let n = unsafe { SearchPathW(std::ptr::null(), w.as_ptr(), std::ptr::null(), buf.len() as u32, buf.as_mut_ptr(), std::ptr::null_mut()) }
        as usize;
    if n == 0 || n >= buf.len() {
        return String::new();
    }
    from_wide(&buf[..n])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalize_like_the_cpp_launcher() {
        assert_eq!(normalize("  \"c:/Games/x/../y/\" "), "C:\\Games\\y");
        assert_eq!(normalize("C:\\"), "C:\\");
        assert_eq!(normalize("c:"), "C:\\");
        assert_eq!(normalize("C:\\a\\..\\.."), "C:\\");
        assert_eq!(normalize("a\\.."), ".");
        assert_eq!(normalize("..\\x\\.\\y"), "..\\x\\y");
        assert_eq!(normalize("\\\\server\\share\\a\\..\\b"), "\\\\server\\share\\b");
        assert_eq!(normalize("\\\\?\\C:\\x\\\\y"), "C:\\x\\y");
        assert_eq!(normalize(""), "");
    }

    #[test]
    fn join_parent_and_name() {
        assert_eq!(join("C:\\a", "/b/c"), "C:\\a\\b\\c");
        assert_eq!(join("C:\\a\\", "b"), "C:\\a\\b");
        assert_eq!(parent("C:\\a"), "C:\\");
        assert_eq!(parent("C:\\"), "");
        assert_eq!(parent("C:\\a\\b.swf"), "C:\\a");
        assert_eq!(parent("x"), "");
        assert_eq!(file_name("C:\\a\\b.swf"), "b.swf");
        assert!(is_absolute("C:\\x") && is_absolute("\\\\srv\\x") && !is_absolute("covers\\x.png"));
    }
}
