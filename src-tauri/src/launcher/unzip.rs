//! Linux: unpacks a download into a folder, as Windows' tar.exe does there (old/src/core/posix/unzip.cpp). A .zip
//! (stored or deflated, Zip64 too) is read here; anything else (.tar.gz, .tar.xz) goes to the system's tar. Entries
//! that would land outside the folder, and links, are refused. A Unix x bit kept in the zip is kept.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::fs::OpenOptionsExt;

use super::http::win32_error_message;
use super::posix::{os_path, win32_from_io};

fn u16_at(p: &[u8]) -> u64 {
    u16::from_le_bytes([p[0], p[1]]) as u64
}
fn u32_at(p: &[u8]) -> u64 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]]) as u64
}
fn u64_at(p: &[u8]) -> u64 {
    u64::from_le_bytes(p[..8].try_into().unwrap())
}

fn read_at(f: &mut File, at: u64, buf: &mut [u8]) -> bool {
    f.seek(SeekFrom::Start(at)).is_ok() && f.read_exact(buf).is_ok()
}

struct Entry {
    name: String,
    method: u64,
    made_by: u64,
    flags: u64,
    crc: u32,
    external: u64,
    packed: u64,
    size: u64,
    local: u64,
}

const DAMAGED_INDEX: &str = "The zip file's index is damaged";

/// The central directory, from the end record (and its Zip64 twin when the archive is that big).
fn read_directory(f: &mut File) -> Result<Vec<Entry>, String> {
    let incomplete = || "The download isn't a complete zip file".to_string();
    let file_size = f.seek(SeekFrom::End(0)).map_err(|_| incomplete())?;
    let tail = file_size.min(65557) as usize;
    let mut buf = vec![0u8; tail];
    if tail < 22 || !read_at(f, file_size - tail as u64, &mut buf) {
        return Err(incomplete());
    }
    let eocd = (0..=tail - 22).rev().find(|&i| u32_at(&buf[i..]) == 0x06054b50).ok_or_else(incomplete)?;
    let (mut count, mut dir_size, mut dir_at) = (u16_at(&buf[eocd + 10..]), u32_at(&buf[eocd + 12..]), u32_at(&buf[eocd + 16..]));
    if (count == 0xFFFF || dir_at == 0xFFFFFFFF) && eocd >= 20 && u32_at(&buf[eocd - 20..]) == 0x07064b50 {
        let mut rec = [0u8; 56];
        if !read_at(f, u64_at(&buf[eocd - 20 + 8..]), &mut rec) || u32_at(&rec) != 0x06064b50 {
            return Err(DAMAGED_INDEX.into());
        }
        count = u64_at(&rec[32..]);
        dir_size = u64_at(&rec[40..]);
        dir_at = u64_at(&rec[48..]);
    }
    if dir_at + dir_size > file_size || dir_size > 1 << 30 {
        return Err(DAMAGED_INDEX.into());
    }
    let mut dir = vec![0u8; dir_size as usize];
    if !read_at(f, dir_at, &mut dir) {
        return Err("The zip file is damaged".into());
    }
    let mut out = Vec::new();
    let mut p = 0usize;
    for _ in 0..count {
        if p + 46 > dir.len() || u32_at(&dir[p..]) != 0x02014b50 {
            return Err(DAMAGED_INDEX.into());
        }
        let h = &dir[p..];
        let (name_len, extra_len, comment_len) = (u16_at(&h[28..]) as usize, u16_at(&h[30..]) as usize, u16_at(&h[32..]) as usize);
        if p + 46 + name_len + extra_len + comment_len > dir.len() {
            return Err(DAMAGED_INDEX.into());
        }
        let mut e = Entry {
            name: String::from_utf8_lossy(&h[46..46 + name_len]).into_owned(),
            made_by: u16_at(&h[4..]),
            flags: u16_at(&h[8..]),
            method: u16_at(&h[10..]),
            crc: u32_at(&h[16..]) as u32,
            packed: u32_at(&h[20..]),
            size: u32_at(&h[24..]),
            external: u32_at(&h[38..]),
            local: u32_at(&h[42..]),
        };
        // Zip64: the sizes and offset that didn't fit, in this order.
        let extra = &h[46 + name_len..46 + name_len + extra_len];
        let mut x = 0;
        while x + 4 <= extra.len() {
            let (id, len) = (u16_at(&extra[x..]), u16_at(&extra[x + 2..]) as usize);
            if x + 4 + len > extra.len() {
                break;
            }
            if id == 1 {
                let mut v = &extra[x + 4..x + 4 + len];
                for field in [&mut e.size, &mut e.packed, &mut e.local] {
                    if *field == 0xFFFFFFFF && v.len() >= 8 {
                        *field = u64_at(v);
                        v = &v[8..];
                    }
                }
            }
            x += 4 + len;
        }
        out.push(e);
        p += 46 + name_len + extra_len + comment_len;
    }
    Ok(out)
}

/// A safe relative path inside the archive, with '/' separators; empty when it isn't one.
fn safe_name(name: &str) -> String {
    let name = name.replace('\\', "/");
    if name.is_empty() || name.starts_with('/') || name.contains(':') {
        return String::new();
    }
    let mut parts = Vec::new();
    for part in name.split('/') {
        if part == ".." {
            return String::new();
        }
        if !part.is_empty() && part != "." {
            parts.push(part);
        }
    }
    parts.join("/")
}

fn extract(f: &mut File, e: &Entry, path: &str) -> Result<(), String> {
    let mut local = [0u8; 30];
    if !read_at(f, e.local, &mut local) || u32_at(&local) != 0x04034b50 {
        return Err("The zip file is damaged".into());
    }
    let data_at = e.local + 30 + u16_at(&local[26..]) + u16_at(&local[28..]);
    if e.flags & 1 != 0 {
        return Err("The zip file is password-protected".into());
    }
    if e.method != 0 && e.method != 8 {
        return Err(format!("The zip file uses a compression this launcher can't unpack (method {})", e.method));
    }
    let damaged = || format!("The zip file is damaged ({})", e.name);
    f.seek(SeekFrom::Start(data_at)).map_err(|_| damaged())?;
    // Made on Unix with an x bit: a Linux program stays runnable.
    let mode = if e.made_by >> 8 == 3 && (e.external >> 16) & 0o111 != 0 { 0o755 } else { 0o644 };
    let mut out = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(mode)
        .open(path)
        .map_err(|err| format!("Cannot write {path}: {}", win32_error_message(win32_from_io(&err))))?;
    let packed = Read::take(&mut *f, e.packed);
    let mut reader: Box<dyn Read> = if e.method == 0 { Box::new(packed) } else { Box::new(flate2::read::DeflateDecoder::new(packed)) };
    let mut crc = crc32fast::Hasher::new();
    let (mut written, mut buf) = (0u64, vec![0u8; 256 * 1024]);
    let ok = loop {
        match reader.read(&mut buf) {
            Ok(0) => break true,
            Ok(n) => {
                crc.update(&buf[..n]);
                written += n as u64;
                if out.write_all(&buf[..n]).is_err() || written > e.size {
                    break false;
                }
            }
            Err(_) => break false,
        }
    };
    drop(out);
    if !ok || written != e.size || crc.finalize() != e.crc {
        let _ = std::fs::remove_file(path);
        return Err(damaged());
    }
    Ok(())
}

fn unzip(archive: &str, dest: &str) -> Result<(), String> {
    let mut f = File::open(archive).map_err(|e| win32_error_message(win32_from_io(&e)))?;
    for e in read_directory(&mut f)? {
        let rel = safe_name(&e.name);
        if rel.is_empty() {
            if e.name.chars().any(|c| !matches!(c, '.' | '/' | '\\')) {
                return Err(format!("The zip file has a path outside its folder ({})", e.name));
            }
            continue; // "./" or the like: nothing to create
        }
        // A symbolic link stored by a Unix zip: refused, as on Windows.
        if e.made_by >> 8 == 3 && (e.external >> 16) & 0o170000 == 0o120000 {
            return Err(format!("The zip file contains a link ({})", e.name));
        }
        let path = format!("{dest}/{rel}");
        if e.name.ends_with(['/', '\\']) {
            std::fs::create_dir_all(&path).map_err(|_| format!("Cannot create {path}"))?;
            continue;
        }
        let dir = &path[..path.rfind('/').unwrap()];
        std::fs::create_dir_all(dir).map_err(|_| format!("Cannot create {dir}"))?;
        extract(&mut f, &e, &path)?;
    }
    Ok(())
}

fn is_zip(path: &str) -> bool {
    let mut head = [0u8; 4];
    File::open(path).is_ok_and(|mut f| f.read_exact(&mut head).is_ok()) && u32_at(&head) == 0x04034b50
}

/// Unpacks `archive` into the existing folder `dest_dir`.
pub fn unpack_archive(archive: &str, dest_dir: &str) -> Result<(), String> {
    let (a, d) = (os_path(archive), os_path(dest_dir));
    if is_zip(&a) {
        return unzip(&a, &d);
    }
    let p = super::process::start_process("/usr/bin/tar", &["-xf".into(), a, "-C".into(), d], dest_dir, true, &[])?;
    match p.wait_exit(180000).unwrap_or(1) {
        0 => Ok(()),
        code => Err(format!("tar couldn't unpack it (exit code {code})")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_stay_inside_the_folder() {
        assert_eq!(safe_name("a\\b/./c"), "a/b/c");
        assert_eq!(safe_name("../x"), "");
        assert_eq!(safe_name("/etc/passwd"), "");
        assert_eq!(safe_name("C:x"), "");
        assert_eq!(safe_name("./"), "");
    }
}
