//! Linux: what the C++ launcher's Win32 layer (old/src/core/posix/posix.cpp) does for the paths and error codes the
//! shared code sees. A '\' is a separator, a part whose case differs from the disk's is found anyway (Windows names
//! in launcher.json and mod specs), and errors carry the Windows code and text the messages and tests expect.

use std::os::unix::fs::MetadataExt;

pub const ERROR_SUCCESS: u32 = 0;
pub const ERROR_FILE_NOT_FOUND: u32 = 2;
pub const ERROR_PATH_NOT_FOUND: u32 = 3;
pub const ERROR_TOO_MANY_OPEN_FILES: u32 = 4;
pub const ERROR_ACCESS_DENIED: u32 = 5;
pub const ERROR_INVALID_HANDLE: u32 = 6;
pub const ERROR_NOT_ENOUGH_MEMORY: u32 = 8;
pub const ERROR_WRITE_PROTECT: u32 = 19;
pub const ERROR_NOT_SAME_DEVICE: u32 = 17;
pub const ERROR_SHARING_VIOLATION: u32 = 32;
pub const ERROR_NOT_SUPPORTED: u32 = 50;
pub const ERROR_FILE_EXISTS: u32 = 80;
pub const ERROR_INVALID_PARAMETER: u32 = 87;
pub const ERROR_BROKEN_PIPE: u32 = 109;
pub const ERROR_DISK_FULL: u32 = 112;
pub const ERROR_DIR_NOT_EMPTY: u32 = 145;
pub const ERROR_ALREADY_EXISTS: u32 = 183;
pub const ERROR_FILENAME_EXCED_RANGE: u32 = 206;
pub const ERROR_TIMEOUT: u32 = 1460;

pub fn win32_from_errno(e: i32) -> u32 {
    match e {
        0 => ERROR_SUCCESS,
        libc::ENOENT => ERROR_FILE_NOT_FOUND,
        libc::ENOTDIR => ERROR_PATH_NOT_FOUND,
        libc::EACCES | libc::EPERM | libc::EISDIR => ERROR_ACCESS_DENIED,
        libc::EEXIST => ERROR_ALREADY_EXISTS,
        libc::ENOTEMPTY => ERROR_DIR_NOT_EMPTY,
        libc::ENOSPC | libc::EDQUOT => ERROR_DISK_FULL,
        libc::EROFS => ERROR_WRITE_PROTECT,
        libc::EBUSY | libc::ETXTBSY => ERROR_SHARING_VIOLATION,
        libc::EXDEV => ERROR_NOT_SAME_DEVICE,
        libc::ENAMETOOLONG => ERROR_FILENAME_EXCED_RANGE,
        libc::EMFILE | libc::ENFILE => ERROR_TOO_MANY_OPEN_FILES,
        libc::ENOMEM => ERROR_NOT_ENOUGH_MEMORY,
        libc::EINVAL => ERROR_INVALID_PARAMETER,
        libc::EBADF => ERROR_INVALID_HANDLE,
        libc::EPIPE => ERROR_BROKEN_PIPE,
        libc::ETIMEDOUT => ERROR_TIMEOUT,
        _ => ERROR_NOT_SUPPORTED,
    }
}

pub fn win32_from_io(e: &std::io::Error) -> u32 {
    e.raw_os_error().map(win32_from_errno).unwrap_or(ERROR_NOT_SUPPORTED)
}

/// The text Windows gives for the codes this layer reports; None for any other code.
pub fn win32_text(code: u32) -> Option<&'static str> {
    Some(match code {
        ERROR_SUCCESS => "The operation completed successfully.",
        ERROR_FILE_NOT_FOUND => "The system cannot find the file specified.",
        ERROR_PATH_NOT_FOUND => "The system cannot find the path specified.",
        ERROR_TOO_MANY_OPEN_FILES => "The system cannot open the file.",
        ERROR_ACCESS_DENIED => "Access is denied.",
        ERROR_INVALID_HANDLE => "The handle is invalid.",
        ERROR_NOT_ENOUGH_MEMORY => "Not enough memory resources are available to process this command.",
        ERROR_WRITE_PROTECT => "The media is write protected.",
        ERROR_NOT_SAME_DEVICE => "The system cannot move the file to a different disk drive.",
        ERROR_SHARING_VIOLATION => "The process cannot access the file because it is being used by another process.",
        33 => "The process cannot access the file because another process has locked a portion of the file.",
        ERROR_NOT_SUPPORTED => "The request is not supported.",
        ERROR_FILE_EXISTS => "The file exists.",
        ERROR_INVALID_PARAMETER => "The parameter is incorrect.",
        ERROR_BROKEN_PIPE => "The pipe has been ended.",
        ERROR_DISK_FULL | 39 => "There is not enough space on the disk.",
        122 => "The data area passed to a system call is too small.",
        ERROR_DIR_NOT_EMPTY => "The directory is not empty.",
        ERROR_ALREADY_EXISTS => "Cannot create a file when that file already exists.",
        ERROR_FILENAME_EXCED_RANGE => "The filename or extension is too long.",
        267 => "The directory name is invalid.",
        ERROR_TIMEOUT => "This operation returned because the timeout period expired.",
        _ => return None,
    })
}

fn lexists(p: &str) -> bool {
    std::fs::symlink_metadata(p).is_ok()
}

/// The real name in `dir` that matches `name` ignoring case.
fn match_in_dir(dir: &str, name: &str) -> Option<String> {
    let rd = std::fs::read_dir(if dir.is_empty() { "/" } else { dir }).ok()?;
    let want = name.to_lowercase();
    rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).find(|n| n.to_lowercase() == want)
}

/// The path to hand Linux: '\' becomes '/', and each part missing as written is looked up ignoring case, down to the
/// first part that isn't there at all (the rest is kept as given, for a file about to be created).
pub fn os_path(path: &str) -> String {
    let mut p = path.replace('\\', "/");
    if p.starts_with("//?/") {
        p.drain(..3);
    }
    if p.is_empty() || lexists(&p) {
        return p;
    }
    if p.eq_ignore_ascii_case("NUL") {
        return "/dev/null".into();
    }
    let absolute = p.starts_with('/');
    let mut out = if absolute { String::new() } else { ".".to_string() };
    let mut missing = false;
    for part in p.split('/').filter(|s| !s.is_empty()) {
        let mut next = format!("{out}/{part}");
        if !missing && part != "." && part != ".." && !lexists(&next) {
            match match_in_dir(if out.is_empty() { "/" } else { &out }, part) {
                Some(real) => next = format!("{out}/{real}"),
                None => missing = true,
            }
        }
        out = next;
    }
    if !absolute {
        out.drain(..2.min(out.len()));
    }
    if p.ends_with('/') && !out.ends_with('/') {
        out.push('/');
    }
    out
}

/// The same file under another case of its name: a rename on Windows, never a replace.
pub fn same_file(a: &std::fs::Metadata, b: &std::fs::Metadata) -> bool {
    a.dev() == b.dev() && a.ino() == b.ino()
}

/// The '*' and '?' match FindFirstFile does, ignoring case.
pub fn wild_match(pattern: &[char], name: &[char]) -> bool {
    match pattern.first() {
        None => name.is_empty(),
        Some('*') => {
            let rest = &pattern[pattern.iter().take_while(|c| **c == '*').count()..];
            (0..=name.len()).any(|i| wild_match(rest, &name[i..]))
        }
        Some(p) => match name.first() {
            None => false,
            Some(n) => (*p == '?' || p.to_lowercase().eq(n.to_lowercase())) && wild_match(&pattern[1..], &name[1..]),
        },
    }
}

/// The Winsock calls the relay and the web server make, on Linux sockets (old/src/core/posix/win32.h's sockets
/// part): the same names and shapes, so the socket code above them is the Windows code. An error is errno, as
/// WSAGetLastError is there; select() runs on poll(), so a socket's number never limits it.
#[allow(non_camel_case_types, non_snake_case, clippy::missing_safety_doc)]
pub mod winsock {
    pub type SOCKET = i32;
    pub const INVALID_SOCKET: SOCKET = -1;
    pub const SOCKET_ERROR: i32 = -1;
    pub const AF_INET: u16 = libc::AF_INET as u16;
    pub const SOCK_STREAM: i32 = libc::SOCK_STREAM;
    pub const IPPROTO_TCP: i32 = libc::IPPROTO_TCP;
    pub const SOL_SOCKET: i32 = libc::SOL_SOCKET;
    /// Linux never lets two sockets listen on one port (SO_REUSEPORT aside); SO_REUSEADDR only lets a relay that
    /// just stopped be started again at once, as Windows allows.
    pub const SO_EXCLUSIVEADDRUSE: i32 = libc::SO_REUSEADDR;
    pub const TCP_NODELAY: i32 = libc::TCP_NODELAY;
    pub const FIONBIO: i32 = 0x8004667e_u32 as i32;
    pub const INADDR_LOOPBACK: u32 = 0x7f00_0001;
    pub const WSAEWOULDBLOCK: i32 = libc::EWOULDBLOCK;
    pub const WSAEADDRINUSE: i32 = libc::EADDRINUSE;
    pub const WSAEACCES: i32 = libc::EACCES;

    pub type SOCKADDR = libc::sockaddr;

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub union S_UN {
        pub S_addr: u32,
    }
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct IN_ADDR {
        pub S_un: S_UN,
    }
    /// sockaddr_in's layout under Winsock's names.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct SOCKADDR_IN {
        pub sin_family: u16,
        pub sin_port: u16,
        pub sin_addr: IN_ADDR,
        pub sin_zero: [u8; 8],
    }

    #[derive(Default)]
    pub struct WSADATA;
    pub unsafe fn WSAStartup(_version: u16, _data: *mut WSADATA) -> i32 {
        0
    }
    pub unsafe fn WSACleanup() -> i32 {
        0
    }
    pub unsafe fn WSAGetLastError() -> i32 {
        std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
    }

    pub struct FD_SET {
        pub fd_count: u32,
        pub fd_array: [SOCKET; 64],
    }
    impl Default for FD_SET {
        fn default() -> Self {
            FD_SET { fd_count: 0, fd_array: [INVALID_SOCKET; 64] }
        }
    }
    pub struct TIMEVAL {
        pub tv_sec: i32,
        pub tv_usec: i32,
    }

    pub unsafe fn socket(af: i32, kind: i32, protocol: i32) -> SOCKET {
        unsafe { libc::socket(af, kind | libc::SOCK_CLOEXEC, protocol) }
    }
    pub unsafe fn setsockopt(s: SOCKET, level: i32, name: i32, value: *const u8, len: i32) -> i32 {
        unsafe { libc::setsockopt(s, level, name, value.cast(), len as libc::socklen_t) }
    }
    pub unsafe fn bind(s: SOCKET, addr: *const SOCKADDR, len: i32) -> i32 {
        unsafe { libc::bind(s, addr, len as libc::socklen_t) }
    }
    pub unsafe fn listen(s: SOCKET, backlog: i32) -> i32 {
        unsafe { libc::listen(s, backlog.min(libc::SOMAXCONN)) }
    }
    pub unsafe fn getsockname(s: SOCKET, addr: *mut SOCKADDR, len: *mut i32) -> i32 {
        let mut l = unsafe { *len } as libc::socklen_t;
        let r = unsafe { libc::getsockname(s, addr, &mut l) };
        unsafe { *len = l as i32 };
        r
    }
    pub unsafe fn accept(s: SOCKET, addr: *mut SOCKADDR, len: *mut i32) -> SOCKET {
        let mut l = unsafe { *len } as libc::socklen_t;
        let r = unsafe { libc::accept4(s, addr, &mut l, libc::SOCK_CLOEXEC) };
        unsafe { *len = l as i32 };
        r
    }
    pub unsafe fn ioctlsocket(s: SOCKET, cmd: i32, arg: *mut u32) -> i32 {
        if cmd != FIONBIO {
            return SOCKET_ERROR;
        }
        let flags = unsafe { libc::fcntl(s, libc::F_GETFL, 0) };
        if flags < 0 {
            return SOCKET_ERROR;
        }
        let flags = if unsafe { *arg } != 0 { flags | libc::O_NONBLOCK } else { flags & !libc::O_NONBLOCK };
        if unsafe { libc::fcntl(s, libc::F_SETFL, flags) } == 0 { 0 } else { SOCKET_ERROR }
    }
    pub unsafe fn recv(s: SOCKET, buf: *mut u8, len: i32, flags: i32) -> i32 {
        unsafe { libc::recv(s, buf.cast(), len as usize, flags) as i32 }
    }
    /// MSG_NOSIGNAL: a peer that went away is an error here, not SIGPIPE ending the launcher.
    pub unsafe fn send(s: SOCKET, buf: *const u8, len: i32, flags: i32) -> i32 {
        unsafe { libc::send(s, buf.cast(), len as usize, flags | libc::MSG_NOSIGNAL) as i32 }
    }
    /// shutdown() first wakes a thread still blocked on the socket, as closesocket does on Windows.
    pub unsafe fn closesocket(s: SOCKET) -> i32 {
        unsafe {
            libc::shutdown(s, libc::SHUT_RDWR);
            libc::close(s)
        }
    }

    /// select() as Winsock has it: the sets list sockets, and on return hold only the ready ones.
    pub unsafe fn select(_nfds: i32, rd: *mut FD_SET, wr: *mut FD_SET, ex: *mut FD_SET, tv: *const TIMEVAL) -> i32 {
        let sets = [(rd, libc::POLLIN), (wr, libc::POLLOUT), (ex, libc::POLLPRI)];
        let mut fds: Vec<libc::pollfd> = Vec::new();
        let mut owner: Vec<usize> = Vec::new();
        for (k, (set, events)) in sets.iter().enumerate() {
            if let Some(set) = unsafe { set.as_ref() } {
                for &s in &set.fd_array[..set.fd_count as usize] {
                    fds.push(libc::pollfd { fd: s, events: *events, revents: 0 });
                    owner.push(k);
                }
            }
        }
        let ms = match unsafe { tv.as_ref() } {
            Some(t) => t.tv_sec.saturating_mul(1000).saturating_add(t.tv_usec / 1000),
            None => -1,
        };
        let n = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, ms) };
        if n < 0 {
            return SOCKET_ERROR;
        }
        let mut ready = 0;
        for (k, (set, events)) in sets.iter().enumerate() {
            if let Some(set) = unsafe { set.as_mut() } {
                set.fd_count = 0;
                for (f, _) in fds.iter().zip(&owner).filter(|(_, o)| **o == k) {
                    // A closed or failed socket reads as readable, as Winsock reports it: recv() then says why.
                    let hit = f.revents & (*events | libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) != 0;
                    if hit {
                        set.fd_array[set.fd_count as usize] = f.fd;
                        set.fd_count += 1;
                        ready += 1;
                    }
                }
            }
        }
        ready
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wildcards_match_as_find_first_file_does() {
        let m = |p: &str, n: &str| wild_match(&p.chars().collect::<Vec<_>>(), &n.chars().collect::<Vec<_>>());
        assert!(m("*.swf", "Game.SWF") && m("g?me*", "game1.swf") && m("*", "") && !m("*.swf", "game.swf.bak"));
    }

    #[test]
    fn paths_are_found_whatever_their_case() {
        let dir = std::env::temp_dir().join(format!("fl-ospath-{}", std::process::id()));
        std::fs::create_dir_all(dir.join("Data")).unwrap();
        std::fs::write(dir.join("Data/Original.dat"), b"x").unwrap();
        let d = dir.to_string_lossy();
        assert_eq!(os_path(&format!("{d}\\data\\ORIGINAL.dat")), format!("{d}/Data/Original.dat"));
        assert_eq!(os_path(&format!("{d}/DATA/new/file")), format!("{d}/Data/new/file"));
        assert_eq!(os_path("nul"), "/dev/null");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
