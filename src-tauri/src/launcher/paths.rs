//! Windows paths as the C++ launcher handles them (old/src/core/fs.cpp): backslashes, `..` collapsed lexically, no
//! trailing '\'. The lookups (file attributes, wildcards, PATH, known folders) call the same Windows functions the
//! C++ launcher calls, so a path that is found there is found here.

#[cfg(windows)]
use std::ffi::OsString;
#[cfg(windows)]
use std::os::windows::ffi::{OsStrExt, OsStringExt};

#[cfg(windows)]
use windows_sys::Win32::Foundation::INVALID_HANDLE_VALUE;
#[cfg(windows)]
use windows_sys::Win32::Storage::FileSystem::{
    FILE_ATTRIBUTE_DIRECTORY, FindClose, FindFirstFileW, FindNextFileW, GetFileAttributesW, GetTempPathW,
    INVALID_FILE_ATTRIBUTES, SearchPathW, WIN32_FIND_DATAW,
};

use super::text::{iequals, trim};

#[cfg(windows)]
pub(crate) fn wide(s: &str) -> Vec<u16> {
    std::ffi::OsStr::new(s).encode_wide().chain(std::iter::once(0)).collect()
}

#[cfg(windows)]
pub(crate) fn from_wide(w: &[u16]) -> String {
    OsString::from_wide(w).to_string_lossy().into_owned()
}

fn is_sep(c: char) -> bool {
    c == '\\' || c == '/'
}

/// std::filesystem's lexically_normal on Windows: one separator between parts, "." dropped, "name\.." cancelled,
/// ".." at a root dropped; a trailing separator is kept when the path ended on one, a "." or a cancelled "..".
#[cfg(windows)]
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
#[cfg(windows)]
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

#[cfg(windows)]
pub fn is_absolute(p: &str) -> bool {
    let c: Vec<char> = p.chars().take(3).collect();
    if c.len() >= 3 && c[0].is_alphabetic() && c[1] == ':' && c[2] == '\\' {
        return true;
    }
    c.len() > 2 && c[0] == '\\' && c[1] == '\\'
}

/// `child` is `parent_dir` or inside it, compared as Windows compares paths.
#[cfg(windows)]
pub fn is_under(child: &str, parent_dir: &str) -> bool {
    let (c, mut p) = (normalize(child), normalize(parent_dir));
    if c.is_empty() || p.is_empty() {
        return false;
    }
    if iequals(&c, &p) {
        return true;
    }
    if !p.ends_with('\\') {
        p.push('\\');
    }
    super::text::starts_with_i(&c, &p)
}

#[cfg(windows)]
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

#[cfg(windows)]
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
#[cfg(windows)]
pub(crate) fn long_path(p: &str) -> String {
    if p.encode_utf16().count() < 240 || p.starts_with("\\\\?\\") {
        return p.to_string();
    }
    if let Some(r) = p.strip_prefix("\\\\") {
        return format!("\\\\?\\UNC\\{r}");
    }
    format!("\\\\?\\{p}")
}

#[cfg(windows)]
fn attrs(p: &str) -> u32 {
    if p.is_empty() {
        return INVALID_FILE_ATTRIBUTES;
    }
    let w = wide(&long_path(p));
    unsafe { GetFileAttributesW(w.as_ptr()) }
}

#[cfg(windows)]
pub fn is_dir(p: &str) -> bool {
    let a = attrs(p);
    a != INVALID_FILE_ATTRIBUTES && a & FILE_ATTRIBUTE_DIRECTORY != 0
}

#[cfg(windows)]
pub fn is_file(p: &str) -> bool {
    let a = attrs(p);
    a != INVALID_FILE_ATTRIBUTES && a & FILE_ATTRIBUTE_DIRECTORY == 0
}

#[cfg(windows)]
pub fn exists(p: &str) -> bool {
    attrs(p) != INVALID_FILE_ATTRIBUTES
}

/// A drive root, a share, Windows or one of the user's top folders: never replaced or emptied as an install folder.
#[cfg(windows)]
pub fn is_dangerous_root(path: &str) -> bool {
    let n = normalize(path);
    if n.encode_utf16().count() <= 3 {
        return true;
    }
    if n.starts_with("\\\\") && n.matches('\\').count() <= 3 {
        return true;
    }
    for r in ["Windows", "ProgramFiles", "ProgramFilesX86", "Profile", "Desktop", "Documents", "ProgramData"] {
        let k = known_folder(r);
        if !k.is_empty() && iequals(&normalize(&k), &n) {
            return true;
        }
    }
    let win = known_folder("Windows");
    !win.is_empty() && is_under(&n, &win)
}

/// MoveFileExW, with Windows' error code when it refuses.
#[cfg(windows)]
pub fn move_file(from: &str, to: &str, replace: bool) -> Result<(), u32> {
    use windows_sys::Win32::Storage::FileSystem::{MOVEFILE_REPLACE_EXISTING, MoveFileExW};
    let (f, t) = (wide(&long_path(from)), wide(&long_path(to)));
    if unsafe { MoveFileExW(f.as_ptr(), t.as_ptr(), if replace { MOVEFILE_REPLACE_EXISTING } else { 0 }) } != 0 {
        Ok(())
    } else {
        Err(unsafe { windows_sys::Win32::Foundation::GetLastError() })
    }
}

#[cfg(windows)]
pub fn delete_file(path: &str) {
    let w = wide(&long_path(path));
    unsafe { windows_sys::Win32::Storage::FileSystem::DeleteFileW(w.as_ptr()) };
}

/// std::filesystem::remove_all, errors ignored: a folder with everything in it, or a file. Links are removed, not
/// followed.
pub fn remove_tree(path: &str) {
    if path.is_empty() {
        return;
    }
    let lp = long_path(path);
    match std::fs::symlink_metadata(&lp) {
        Ok(m) if m.is_dir() => {
            let _ = std::fs::remove_dir_all(&lp);
        }
        Ok(_) => {
            let _ = std::fs::remove_file(&lp).or_else(|_| std::fs::remove_dir(&lp));
        }
        Err(_) => {}
    }
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

#[cfg(windows)]
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
#[cfg(windows)]
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
#[cfg(windows)]
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
#[cfg(windows)]
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

#[cfg(all(test, windows))]
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

// ---- Linux (old/src/core/fs.cpp's #else half and the Win32 layer under it) ------------------------------------
// '/' paths. A '\' from launcher.json or a mod spec ("${GAME_DIR}\Mods") is a separator here too, so the shared
// configs work unchanged; results always use '/'. Names are matched ignoring case, as on Windows (posix::os_path).

/// std::filesystem's lexically_normal on Linux.
#[cfg(unix)]
fn lexically_normal(s: &str) -> String {
    if s.is_empty() {
        return String::new();
    }
    let root = s.starts_with('/');
    let raw: Vec<&str> = s.split('/').collect();
    let mut parts: Vec<&str> = Vec::new();
    let mut trailing = false;
    let n = raw.len();
    for (k, p) in raw.iter().enumerate() {
        if p.is_empty() {
            if k + 1 == n && k > 0 {
                trailing = true;
            }
            continue;
        }
        trailing = false;
        match *p {
            "." => trailing = true,
            ".." if parts.last().is_some_and(|l| *l != "..") => {
                parts.pop();
                trailing = true;
            }
            ".." if root => trailing = true,
            _ => parts.push(p),
        }
    }
    let mut out = if root { "/".to_string() } else { String::new() };
    if parts.is_empty() {
        return if out.is_empty() { ".".into() } else { out };
    }
    out.push_str(&parts.join("/"));
    if trailing && parts.last() != Some(&"..") {
        out.push('/');
    }
    out
}

/// Trims quotes and space, uses '/', collapses `..`, drops a trailing '/' (not the root's).
#[cfg(unix)]
pub fn normalize(input: &str) -> String {
    let mut s = trim(input).to_string();
    while s.chars().count() >= 2 && s.starts_with('"') && s.ends_with('"') {
        s = trim(&s[1..s.len() - 1]).to_string();
    }
    if s.is_empty() {
        return String::new();
    }
    let mut out = lexically_normal(&s.replace('\\', "/"));
    while out.len() > 1 && out.ends_with('/') {
        out.pop();
    }
    out
}

#[cfg(unix)]
pub fn is_absolute(p: &str) -> bool {
    p.starts_with(is_sep)
}

/// `child` is `parent_dir` or inside it.
#[cfg(unix)]
pub fn is_under(child: &str, parent_dir: &str) -> bool {
    let (c, mut p) = (normalize(child), normalize(parent_dir));
    if c.is_empty() || p.is_empty() {
        return false;
    }
    if c == p {
        return true;
    }
    if !p.ends_with('/') {
        p.push('/');
    }
    c.starts_with(&p)
}

#[cfg(unix)]
pub fn join(a: &str, b: &str) -> String {
    if a.is_empty() {
        return b.to_string();
    }
    if b.is_empty() {
        return a.to_string();
    }
    let mut r = a.to_string();
    if !r.ends_with(is_sep) {
        r.push('/');
    }
    r.push_str(b.trim_start_matches(is_sep));
    r.replace('\\', "/")
}

#[cfg(unix)]
pub fn parent(path: &str) -> String {
    match path.rfind(is_sep) {
        None => String::new(),
        Some(0) if path.len() > 1 => "/".into(),
        Some(0) => String::new(),
        Some(pos) => path[..pos].to_string(),
    }
}

/// The path as Linux finds it (posix::os_path). The name comes from the Windows side, where it adds \\?\.
#[cfg(unix)]
pub(crate) fn long_path(p: &str) -> String {
    super::posix::os_path(p)
}

#[cfg(unix)]
fn stat(p: &str) -> Option<std::fs::Metadata> {
    if p.is_empty() {
        return None;
    }
    let lp = long_path(p);
    std::fs::metadata(&lp).or_else(|_| std::fs::symlink_metadata(&lp)).ok()
}

#[cfg(unix)]
pub fn is_dir(p: &str) -> bool {
    stat(p).is_some_and(|m| m.is_dir())
}

#[cfg(unix)]
pub fn is_file(p: &str) -> bool {
    stat(p).is_some_and(|m| !m.is_dir())
}

#[cfg(unix)]
pub fn exists(p: &str) -> bool {
    stat(p).is_some()
}

/// The root, a system folder or one of the user's top folders: never replaced or emptied as an install folder.
#[cfg(unix)]
pub fn is_dangerous_root(path: &str) -> bool {
    let n = normalize(path);
    if n.len() <= 1 {
        return true;
    }
    const TOP: [&str; 19] = [
        "/usr", "/etc", "/bin", "/sbin", "/lib", "/lib64", "/boot", "/dev", "/proc", "/sys", "/var", "/opt", "/home",
        "/root", "/run", "/srv", "/mnt", "/media", "/tmp",
    ];
    if TOP.contains(&n.as_str()) {
        return true;
    }
    for r in ["Profile", "Desktop", "Documents"] {
        let k = known_folder(r);
        if !k.is_empty() && normalize(&k) == n {
            return true;
        }
    }
    ["/usr", "/etc", "/bin", "/sbin", "/boot", "/proc", "/sys", "/dev"].iter().any(|s| is_under(&n, s))
}

/// MoveFileExW as the C++ launcher's Win32 layer does it, with the Windows error code when it refuses.
#[cfg(unix)]
pub fn move_file(from: &str, to: &str, replace: bool) -> Result<(), u32> {
    use super::posix::*;
    let (f, mut t) = (long_path(from), long_path(to));
    let src = std::fs::symlink_metadata(&f).map_err(|e| win32_from_io(&e))?;
    if let Ok(dst) = std::fs::symlink_metadata(&t) {
        if same_file(&src, &dst) {
            // Renaming to another case of the same name: use the name as asked.
            t = format!("{}{}", &t[..t.rfind('/').map_or(0, |i| i + 1)], file_name(to));
        } else if !replace {
            return Err(ERROR_ALREADY_EXISTS);
        } else if dst.is_dir() {
            return Err(ERROR_ACCESS_DENIED);
        }
    }
    match std::fs::rename(&f, &t) {
        Ok(()) => Ok(()),
        Err(e) if e.raw_os_error() == Some(libc::ENOENT) => {
            Err(if is_dir(&parent(&t)) { ERROR_FILE_NOT_FOUND } else { ERROR_PATH_NOT_FOUND })
        }
        Err(e) => Err(win32_from_io(&e)),
    }
}

#[cfg(unix)]
pub fn delete_file(path: &str) {
    let _ = std::fs::remove_file(long_path(path));
}

/// $TMPDIR (or $TMP, $TEMP) when it is absolute, else /tmp.
#[cfg(unix)]
pub fn temp_dir() -> String {
    let t = ["TMPDIR", "TMP", "TEMP"]
        .iter()
        .filter_map(|v| std::env::var(v).ok())
        .find(|v| v.starts_with('/'))
        .unwrap_or_else(|| "/tmp".into());
    normalize(&t)
}

#[cfg(unix)]
fn env_dir(var: &str, fallback: &str) -> String {
    std::env::var(var).ok().filter(|v| v.starts_with('/')).unwrap_or_else(|| fallback.to_string())
}

/// XDG_DESKTOP_DIR="$HOME/Desktop" in ~/.config/user-dirs.dirs, as xdg-user-dir reads it.
#[cfg(unix)]
fn user_dir(key: &str, fallback: &str) -> String {
    let home = env_dir("HOME", "/tmp");
    let file = format!("{}/user-dirs.dirs", env_dir("XDG_CONFIG_HOME", &format!("{home}/.config")));
    if let Ok(text) = std::fs::read_to_string(file) {
        let prefix = format!("{key}=\"");
        for line in text.lines() {
            let Some(v) = line.strip_prefix(&prefix).and_then(|r| r.strip_suffix('"')) else { continue };
            if let Some(rest) = v.strip_prefix("$HOME") {
                return format!("{home}{rest}");
            }
            if v.starts_with('/') {
                return v.to_string();
            }
        }
    }
    format!("{home}/{fallback}")
}

/// The Windows folder names launcher.json uses, mapped to their Linux places. Program Files, Windows and
/// ProgramData have none: a Windows game's copy of them is inside its Proton prefix.
#[cfg(unix)]
pub fn known_folder(name: &str) -> String {
    let home = env_dir("HOME", "/tmp");
    let data = env_dir("XDG_DATA_HOME", &format!("{home}/.local/share"));
    match name.to_ascii_lowercase().as_str() {
        "temp" => temp_dir(),
        "profile" => home,
        "desktop" => user_dir("XDG_DESKTOP_DIR", "Desktop"),
        "documents" => user_dir("XDG_DOCUMENTS_DIR", "Documents"),
        "localappdata" | "savedgames" => data,
        "roamingappdata" => env_dir("XDG_CONFIG_HOME", &format!("{home}/.config")),
        "startmenu" => format!("{data}/applications"),
        "commonstartmenu" => "/usr/share/applications".into(),
        _ => String::new(),
    }
}

/// The files (not folders) a wildcard pattern matches, as FindFirstFile matches them: ignoring case.
#[cfg(unix)]
pub fn find_files(pattern: &str) -> Vec<String> {
    let (dir, mask) = match pattern.rfind(is_sep) {
        Some(0) => ("/".to_string(), &pattern[1..]),
        Some(i) => (pattern[..i].to_string(), &pattern[i + 1..]),
        None => (".".to_string(), pattern),
    };
    let Ok(rd) = std::fs::read_dir(long_path(&dir)) else { return Vec::new() };
    let wild = mask.contains(['*', '?']);
    let mask_chars: Vec<char> = mask.chars().collect();
    let mut names = Vec::new();
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let hit = if wild { super::posix::wild_match(&mask_chars, &name.chars().collect::<Vec<_>>()) } else { iequals(&name, mask) };
        if hit && !std::fs::metadata(e.path()).is_ok_and(|m| m.is_dir()) {
            names.push(name);
        }
    }
    names
}

/// A bare program name looked up on PATH; "node.exe" is "node" here. Empty when it isn't anywhere.
#[cfg(unix)]
pub fn search_path(name: &str) -> String {
    use std::os::unix::fs::PermissionsExt;
    let mut names = vec![name.to_string()];
    if name.len() > 4 && name.is_char_boundary(name.len() - 4) && name[name.len() - 4..].eq_ignore_ascii_case(".exe") {
        names.push(name[..name.len() - 4].to_string());
    }
    let dirs = std::env::var("PATH").unwrap_or_else(|_| "/usr/local/bin:/usr/bin:/bin".into());
    for d in dirs.split([':', ';']).filter(|d| !d.is_empty()) {
        for n in &names {
            let full = format!("{d}/{n}");
            if std::fs::metadata(&full).is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0) {
                return full;
            }
        }
    }
    String::new()
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;

    #[test]
    fn normalize_like_the_cpp_launcher() {
        assert_eq!(normalize("  \"/games/x/../y/\" "), "/games/y");
        assert_eq!(normalize("/"), "/");
        assert_eq!(normalize("/a/../.."), "/");
        assert_eq!(normalize("a/.."), ".");
        assert_eq!(normalize("..\\x\\.\\y"), "../x/y");
        assert_eq!(normalize("${GAME_DIR}\\Mods\\"), "${GAME_DIR}/Mods");
        assert_eq!(normalize(""), "");
    }

    #[test]
    fn join_parent_and_name() {
        assert_eq!(join("/a", "\\b\\c"), "/a/b/c");
        assert_eq!(join("/a/", "b"), "/a/b");
        assert_eq!(parent("/a"), "/");
        assert_eq!(parent("/"), "");
        assert_eq!(parent("/a/b.swf"), "/a");
        assert_eq!(parent("x"), "");
        assert_eq!(file_name("/a/b.swf"), "b.swf");
        assert!(is_absolute("/x") && !is_absolute("covers/x.png"));
        assert!(is_under("/a/b/c", "/a/b") && !is_under("/a/bc", "/a/b"));
        assert!(is_dangerous_root("/") && is_dangerous_root("/usr/lib") && !is_dangerous_root("/home/x/Games"));
    }
}
