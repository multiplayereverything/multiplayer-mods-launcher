//! Discord Rich Presence through the Discord app's local pipe, discord-ipc-N (old/src/core/discord.cpp): "Playing
//! Gun Mayhem 2, Room VK7Q2". Nothing leaves the PC except through the Discord app itself. A worker thread owns the
//! pipe; Discord not running is normal, and it is tried again now and then.

use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

#[cfg(windows)]
use windows_sys::Win32::Foundation::{CloseHandle, GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE};
#[cfg(windows)]
use windows_sys::Win32::Storage::FileSystem::{CreateFileW, OPEN_EXISTING, ReadFile, WriteFile};
#[cfg(windows)]
use windows_sys::Win32::System::Pipes::PeekNamedPipe;
#[cfg(windows)]
use windows_sys::Win32::System::Threading::GetCurrentProcessId;

use super::json::{Value, serialize};
#[cfg(windows)]
use super::paths::wide;

#[cfg(windows)]
type Pipe = HANDLE;
#[cfg(windows)]
const NO_PIPE: Pipe = INVALID_HANDLE_VALUE;
#[cfg(windows)]
fn close_pipe(p: Pipe) {
    unsafe { CloseHandle(p) };
}
#[cfg(windows)]
fn current_pid() -> u32 {
    unsafe { GetCurrentProcessId() }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Presence {
    pub details: String, // first line, e.g. "Playing Gun Mayhem 2"
    pub state: String,   // second line, e.g. "Room VK7Q2"
    pub start: i64,      // unix time the game started; 0 = no timer
}

pub fn handshake(client_id: &str) -> String {
    let mut v = Value::object();
    v.set("v", 1);
    v.set("client_id", client_id);
    serialize(&v, false)
}

/// Discord counts UTF-16 units, as the C++ side did with its wide strings.
fn utf16_len(s: &str) -> usize {
    s.encode_utf16().count()
}
fn utf16_prefix(s: &str, n: usize) -> String {
    String::from_utf16_lossy(&s.encode_utf16().take(n).collect::<Vec<u16>>())
}

/// SET_ACTIVITY; `None` clears it.
pub fn activity(p: Option<&Presence>, pid: u32, nonce: i32) -> String {
    let mut args = Value::object();
    args.set("pid", pid as i64);
    match p {
        Some(p) => {
            let mut a = Value::object();
            // Discord rejects fields shorter than 2 characters.
            if utf16_len(&p.details) >= 2 {
                a.set("details", utf16_prefix(&p.details, 120));
            }
            if utf16_len(&p.state) >= 2 {
                a.set("state", utf16_prefix(&p.state, 120));
            }
            if p.start > 0 {
                let mut ts = Value::object();
                ts.set("start", p.start);
                a.set("timestamps", ts);
            }
            args.set("activity", a);
        }
        None => args.set("activity", Value::Null),
    }
    let mut v = Value::object();
    v.set("cmd", "SET_ACTIVITY");
    v.set("args", args);
    v.set("nonce", nonce.to_string());
    serialize(&v, false)
}

/// Frames are: opcode (0 handshake, 1 frame), payload length, JSON. Little-endian.
pub fn frame(op: u32, json: &str) -> Vec<u8> {
    let mut buf = Vec::with_capacity(8 + json.len());
    buf.extend_from_slice(&op.to_le_bytes());
    buf.extend_from_slice(&(json.len() as u32).to_le_bytes());
    buf.extend_from_slice(json.as_bytes());
    buf
}

#[cfg(windows)]
fn write_frame(pipe: HANDLE, op: u32, json: &str) -> bool {
    let buf = frame(op, json);
    let mut done = 0u32;
    unsafe { WriteFile(pipe, buf.as_ptr(), buf.len() as u32, &mut done, std::ptr::null_mut()) != 0 && done as usize == buf.len() }
}

/// Reads one reply if one arrives within `ms`, without ever blocking longer.
#[cfg(windows)]
fn read_frame(pipe: HANDLE, ms: u64) -> Option<(u32, String)> {
    let until = Instant::now() + Duration::from_millis(ms);
    loop {
        let mut avail = 0u32;
        if unsafe { PeekNamedPipe(pipe, std::ptr::null_mut(), 0, std::ptr::null_mut(), &mut avail, std::ptr::null_mut()) } == 0 {
            return None;
        }
        if avail >= 8 {
            break;
        }
        if Instant::now() > until {
            return None;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    let mut head = [0u8; 8];
    let mut got = 0u32;
    if unsafe { ReadFile(pipe, head.as_mut_ptr(), 8, &mut got, std::ptr::null_mut()) } == 0 || got != 8 {
        return None;
    }
    let op = u32::from_le_bytes(head[..4].try_into().unwrap());
    let len = u32::from_le_bytes(head[4..].try_into().unwrap()) as usize;
    if len > 1 << 20 {
        return None;
    }
    let mut json = vec![0u8; len];
    let mut off = 0;
    while off < len {
        if unsafe { ReadFile(pipe, json[off..].as_mut_ptr(), (len - off) as u32, &mut got, std::ptr::null_mut()) } == 0 || got == 0 {
            return None;
        }
        off += got as usize;
    }
    Some((op, String::from_utf8_lossy(&json).into_owned()))
}

#[cfg(windows)]
fn connect_pipe(prefix: &str) -> HANDLE {
    for i in 0..10 {
        let name = wide(&format!("{prefix}{i}"));
        let h = unsafe { CreateFileW(name.as_ptr(), GENERIC_READ | GENERIC_WRITE, 0, std::ptr::null(), OPEN_EXISTING, 0, std::ptr::null_mut()) };
        if h != INVALID_HANDLE_VALUE {
            return h;
        }
    }
    INVALID_HANDLE_VALUE
}

#[derive(Default)]
struct Wanted {
    presence: Presence,
    has: bool,
    dirty: bool,
    stop: bool,
}

pub struct DiscordPresence {
    state: Arc<(Mutex<Wanted>, Condvar)>,
    thread: Option<JoinHandle<()>>,
}

impl DiscordPresence {
    pub fn new(client_id: &str) -> Self {
        // Linux: an empty prefix looks where the Discord app (native, Flatpak or Snap) puts its socket.
        Self::with_pipe(client_id, if cfg!(windows) { r"\\.\pipe\discord-ipc-" } else { "" })
    }

    /// `pipe`: the pipe names without their number (tests stand in for Discord under another name).
    pub fn with_pipe(client_id: &str, pipe: &str) -> Self {
        let state = Arc::new((Mutex::new(Wanted::default()), Condvar::new()));
        let (s, id, prefix) = (state.clone(), client_id.to_string(), pipe.to_string());
        let thread = std::thread::spawn(move || run(&s, &id, &prefix));
        DiscordPresence { state, thread: Some(thread) }
    }

    pub fn set(&self, p: &Presence) {
        let mut w = self.state.0.lock().unwrap();
        if w.has && w.presence == *p {
            return;
        }
        w.presence = p.clone();
        w.has = true;
        w.dirty = true;
        self.state.1.notify_all();
    }

    pub fn clear(&self) {
        let mut w = self.state.0.lock().unwrap();
        if !w.has {
            return;
        }
        w.has = false;
        w.dirty = true;
        self.state.1.notify_all();
    }
}

impl Drop for DiscordPresence {
    fn drop(&mut self) {
        self.state.0.lock().unwrap().stop = true;
        self.state.1.notify_all();
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

fn run(state: &(Mutex<Wanted>, Condvar), client_id: &str, prefix: &str) {
    let mut pipe = NO_PIPE;
    let mut nonce = 0;
    let pid = current_pid();
    let drop_pipe = |pipe: &mut Pipe| {
        if *pipe != NO_PIPE {
            close_pipe(*pipe);
        }
        *pipe = NO_PIPE;
    };
    let mut w = state.0.lock().unwrap();
    while !w.stop {
        // Wake on a change; retry a lost connection every 20 seconds while there is something to show.
        w = state.1.wait_timeout_while(w, Duration::from_secs(20), |w| !(w.stop || w.dirty)).unwrap().0;
        if w.stop {
            break;
        }
        if !w.dirty && (pipe != NO_PIPE || !w.has) {
            continue;
        }
        let (p, has) = (w.presence.clone(), w.has);
        w.dirty = false;
        drop(w);
        if pipe == NO_PIPE && has {
            pipe = connect_pipe(prefix);
            if pipe != NO_PIPE && !(write_frame(pipe, 0, &handshake(client_id)) && read_frame(pipe, 3000).is_some_and(|(op, _)| op == 1)) {
                drop_pipe(&mut pipe);
            }
        }
        if pipe != NO_PIPE {
            nonce += 1;
            if !write_frame(pipe, 1, &activity(has.then_some(&p), pid, nonce)) {
                drop_pipe(&mut pipe);
                w = state.0.lock().unwrap();
                w.dirty = has; // Discord restarted: send it again on the next try
                continue;
            }
            let _ = read_frame(pipe, 2000); // drain the answer; an error is only worth a debug line
            if !has {
                drop_pipe(&mut pipe); // nothing to show: let go of the pipe
            }
        }
        w = state.0.lock().unwrap();
    }
    drop(w);
    if pipe != NO_PIPE {
        nonce += 1;
        write_frame(pipe, 1, &activity(None, pid, nonce));
        drop_pipe(&mut pipe);
    }
}

// ---- Linux (old/src/core/discord.cpp's #else half) -------------------------------------------------------------
// The Discord app listens on a Unix socket, discord-ipc-N, in $XDG_RUNTIME_DIR (or the temp folder); the Flatpak and
// Snap builds put theirs one folder down. The frames are the same.

#[cfg(unix)]
type Pipe = i32;
#[cfg(unix)]
const NO_PIPE: Pipe = -1;
#[cfg(unix)]
fn close_pipe(p: Pipe) {
    unsafe { libc::close(p) };
}
#[cfg(unix)]
fn current_pid() -> u32 {
    std::process::id()
}

#[cfg(unix)]
fn write_frame(pipe: Pipe, op: u32, json: &str) -> bool {
    let buf = frame(op, json);
    let mut off = 0;
    while off < buf.len() {
        let n = unsafe { libc::send(pipe, buf[off..].as_ptr().cast(), buf.len() - off, libc::MSG_NOSIGNAL) };
        if n <= 0 {
            return false;
        }
        off += n as usize;
    }
    true
}

/// `len` bytes, waiting at most until `until` for each part.
#[cfg(unix)]
fn read_exact(pipe: Pipe, buf: &mut [u8], until: Instant) -> bool {
    let mut off = 0;
    while off < buf.len() {
        let left = until.saturating_duration_since(Instant::now()).as_millis().min(i32::MAX as u128) as i32;
        let mut pfd = libc::pollfd { fd: pipe, events: libc::POLLIN, revents: 0 };
        if unsafe { libc::poll(&mut pfd, 1, left) } <= 0 {
            return false;
        }
        let n = unsafe { libc::recv(pipe, buf[off..].as_mut_ptr().cast(), buf.len() - off, 0) };
        if n <= 0 {
            return false;
        }
        off += n as usize;
    }
    true
}

/// Reads one reply if one arrives within `ms`, without ever blocking longer.
#[cfg(unix)]
fn read_frame(pipe: Pipe, ms: u64) -> Option<(u32, String)> {
    let until = Instant::now() + Duration::from_millis(ms);
    let mut head = [0u8; 8];
    if !read_exact(pipe, &mut head, until) {
        return None;
    }
    let op = u32::from_le_bytes(head[..4].try_into().unwrap());
    let len = u32::from_le_bytes(head[4..].try_into().unwrap()) as usize;
    if len > 1 << 20 {
        return None;
    }
    let mut json = vec![0u8; len];
    // The body follows the header at once; a second's grace covers a slow app.
    if !read_exact(pipe, &mut json, until.max(Instant::now() + Duration::from_secs(1))) {
        return None;
    }
    Some((op, String::from_utf8_lossy(&json).into_owned()))
}

/// The socket prefixes to try: `prefix` itself, or with none, every place a Discord build puts its socket.
#[cfg(unix)]
fn socket_prefixes(prefix: &str) -> Vec<String> {
    if !prefix.is_empty() {
        return vec![prefix.to_string()];
    }
    let mut dirs: Vec<String> = ["XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"].iter().filter_map(|v| std::env::var(v).ok()).filter(|d| d.starts_with('/')).collect();
    dirs.push("/tmp".into());
    let mut out = Vec::new();
    for d in dirs {
        let d = d.trim_end_matches('/').to_string();
        for sub in ["", "app/com.discordapp.Discord/", "app/com.discordapp.DiscordCanary/", "snap.discord/", "snap.discord-canary/"] {
            let p = format!("{d}/{sub}discord-ipc-");
            if !out.contains(&p) {
                out.push(p);
            }
        }
    }
    out
}

#[cfg(unix)]
fn connect_pipe(prefix: &str) -> Pipe {
    use std::os::fd::IntoRawFd;
    for p in socket_prefixes(prefix) {
        for i in 0..10 {
            if let Ok(s) = std::os::unix::net::UnixStream::connect(format!("{p}{i}")) {
                return s.into_raw_fd();
            }
        }
    }
    NO_PIPE
}
