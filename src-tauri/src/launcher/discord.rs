//! Discord Rich Presence through the Discord app's local pipe, discord-ipc-N (old/src/core/discord.cpp): "Playing
//! Gun Mayhem 2, Room VK7Q2". Nothing leaves the PC except through the Discord app itself. A worker thread owns the
//! pipe; Discord not running is normal, and it is tried again now and then.

use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use windows_sys::Win32::Foundation::{CloseHandle, GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE};
use windows_sys::Win32::Storage::FileSystem::{CreateFileW, OPEN_EXISTING, ReadFile, WriteFile};
use windows_sys::Win32::System::Pipes::PeekNamedPipe;
use windows_sys::Win32::System::Threading::GetCurrentProcessId;

use super::json::{Value, serialize};
use super::paths::wide;

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

fn write_frame(pipe: HANDLE, op: u32, json: &str) -> bool {
    let buf = frame(op, json);
    let mut done = 0u32;
    unsafe { WriteFile(pipe, buf.as_ptr(), buf.len() as u32, &mut done, std::ptr::null_mut()) != 0 && done as usize == buf.len() }
}

/// Reads one reply if one arrives within `ms`, without ever blocking longer.
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
        Self::with_pipe(client_id, r"\\.\pipe\discord-ipc-")
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
    let mut pipe = INVALID_HANDLE_VALUE;
    let mut nonce = 0;
    let pid = unsafe { GetCurrentProcessId() };
    let drop_pipe = |pipe: &mut HANDLE| {
        if *pipe != INVALID_HANDLE_VALUE {
            unsafe { CloseHandle(*pipe) };
        }
        *pipe = INVALID_HANDLE_VALUE;
    };
    let mut w = state.0.lock().unwrap();
    while !w.stop {
        // Wake on a change; retry a lost connection every 20 seconds while there is something to show.
        w = state.1.wait_timeout_while(w, Duration::from_secs(20), |w| !(w.stop || w.dirty)).unwrap().0;
        if w.stop {
            break;
        }
        if !w.dirty && (pipe != INVALID_HANDLE_VALUE || !w.has) {
            continue;
        }
        let (p, has) = (w.presence.clone(), w.has);
        w.dirty = false;
        drop(w);
        if pipe == INVALID_HANDLE_VALUE && has {
            pipe = connect_pipe(prefix);
            if pipe != INVALID_HANDLE_VALUE && !(write_frame(pipe, 0, &handshake(client_id)) && read_frame(pipe, 3000).is_some_and(|(op, _)| op == 1)) {
                drop_pipe(&mut pipe);
            }
        }
        if pipe != INVALID_HANDLE_VALUE {
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
    if pipe != INVALID_HANDLE_VALUE {
        nonce += 1;
        write_frame(pipe, 1, &activity(None, pid, nonce));
        drop_pipe(&mut pipe);
    }
}
