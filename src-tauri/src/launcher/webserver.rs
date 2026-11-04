//! A tiny static web server for HTML5 games that are just a folder of files (old/src/core/webserver.cpp). Browsers
//! block much of what games do from file:// pages (loading their own data, WebAssembly, audio), so the folder is
//! served on 127.0.0.1 while the game runs. GET and HEAD only, byte ranges for media, nothing outside the folder.

use std::io::{Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
#[cfg(windows)]
use std::os::windows::io::FromRawSocket;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

#[cfg(windows)]
use windows_sys::Win32::Networking::WinSock::{INADDR_LOOPBACK, SOCKADDR, SOCKADDR_IN, WSACleanup, getsockname};
#[cfg(unix)]
use super::posix::winsock::{INADDR_LOOPBACK, SOCKADDR, SOCKADDR_IN, WSACleanup, getsockname};

use super::http::win32_error_message;
use super::log;
use super::paths;
use super::relay::{listen_on, wsa_startup};

/// Percent-decoding of a URL path; '+' stays '+' (it only means space in queries).
fn url_decode(s: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < s.len() {
        if s[i] == b'%' && i + 2 < s.len() && s[i + 1].is_ascii_hexdigit() && s[i + 2].is_ascii_hexdigit() {
            out.push(u8::from_str_radix(std::str::from_utf8(&s[i + 1..i + 3]).unwrap(), 16).unwrap());
            i += 3;
        } else {
            out.push(s[i]);
            i += 1;
        }
    }
    out
}

/// MIME type from a file name ("game.wasm" -> "application/wasm").
pub fn mime_type(file_name: &str) -> &'static str {
    let ext = file_name.rfind('.').map(|i| super::text::to_lower(&file_name[i + 1..])).unwrap_or_default();
    const TYPES: &[(&str, &str)] = &[
        ("html", "text/html; charset=utf-8"), ("htm", "text/html; charset=utf-8"),
        ("js", "text/javascript; charset=utf-8"), ("mjs", "text/javascript; charset=utf-8"),
        ("css", "text/css; charset=utf-8"), ("json", "application/json"), ("wasm", "application/wasm"),
        ("png", "image/png"), ("jpg", "image/jpeg"), ("jpeg", "image/jpeg"), ("gif", "image/gif"),
        ("webp", "image/webp"), ("svg", "image/svg+xml"), ("ico", "image/x-icon"),
        ("mp3", "audio/mpeg"), ("ogg", "audio/ogg"), ("wav", "audio/wav"), ("m4a", "audio/mp4"),
        ("mp4", "video/mp4"), ("webm", "video/webm"), ("woff", "font/woff"), ("woff2", "font/woff2"),
        ("ttf", "font/ttf"), ("otf", "font/otf"), ("xml", "application/xml"), ("txt", "text/plain; charset=utf-8"),
        ("swf", "application/x-shockwave-flash"),
    ];
    TYPES.iter().find(|(e, _)| *e == ext).map(|(_, t)| *t).unwrap_or("application/octet-stream")
}

/// strtoull on what follows: leading white space, digits; nothing readable is 0.
fn strtoull(s: &[u8]) -> usize {
    let t: &[u8] = &s[s.iter().position(|b| !b.is_ascii_whitespace()).unwrap_or(s.len())..];
    let t = t.strip_prefix(b"+").unwrap_or(t);
    t.iter().take_while(|b| b.is_ascii_digit()).fold(0usize, |v, b| v.saturating_mul(10).saturating_add((b - b'0') as usize))
}

fn find_bytes(hay: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if from > hay.len() {
        return None;
    }
    hay[from..].windows(needle.len()).position(|w| w == needle).map(|p| p + from)
}

#[derive(Default)]
pub struct WebServer {
    port: i32,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
    workers: Arc<Mutex<Vec<JoinHandle<()>>>>,
}

impl WebServer {
    pub fn new() -> Self {
        Self::default()
    }

    /// Serves `root` on 127.0.0.1:`port` (0 = any free port). A fixed port per game keeps its saves: browsers file
    /// local storage by address.
    pub fn start(&mut self, root: &str, port: i32) -> Result<(), String> {
        self.stop();
        if wsa_startup() != 0 {
            return Err("Networking is not available".into());
        }
        let s = match listen_on(INADDR_LOOPBACK, port, false) {
            Ok(s) => s,
            Err(code) => {
                unsafe { WSACleanup() };
                return Err(win32_error_message(code as u32));
            }
        };
        let mut addr: SOCKADDR_IN = unsafe { std::mem::zeroed() };
        let mut len = std::mem::size_of::<SOCKADDR_IN>() as i32;
        unsafe { getsockname(s, (&mut addr as *mut SOCKADDR_IN).cast::<SOCKADDR>(), &mut len) };
        self.port = u16::from_be(addr.sin_port) as i32;
        let root = paths::normalize(root);
        #[cfg(windows)]
        let listener = unsafe { TcpListener::from_raw_socket(s as u64) };
        #[cfg(unix)]
        let listener = unsafe { <TcpListener as std::os::fd::FromRawFd>::from_raw_fd(s) };
        self.stop = Arc::new(AtomicBool::new(false));
        let (stop, workers, served) = (self.stop.clone(), self.workers.clone(), root.clone());
        self.thread = Some(std::thread::spawn(move || {
            for c in listener.incoming() {
                if stop.load(Ordering::SeqCst) {
                    break;
                }
                let Ok(c) = c else { break };
                // Browsers open several connections at once; each gets a short-lived thread.
                let root = served.clone();
                workers.lock().unwrap().push(std::thread::spawn(move || serve(c, &root)));
            }
        }));
        log::info(&format!("Serving {root} at http://127.0.0.1:{}/", self.port));
        Ok(())
    }

    pub fn stop(&mut self) {
        let Some(t) = self.thread.take() else { return };
        self.stop.store(true, Ordering::SeqCst);
        // Wakes accept(): the listener closes when its thread ends.
        let _ = TcpStream::connect_timeout(&([127, 0, 0, 1], self.port as u16).into(), Duration::from_secs(2));
        let _ = t.join();
        let workers = std::mem::take(&mut *self.workers.lock().unwrap());
        for w in workers {
            let _ = w.join();
        }
        unsafe { WSACleanup() };
    }

    pub fn port(&self) -> i32 {
        self.port
    }
}

impl Drop for WebServer {
    fn drop(&mut self) {
        self.stop();
    }
}

fn send_status(c: &mut TcpStream, status: &str) {
    let _ = c.write_all(format!("HTTP/1.1 {status}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").as_bytes());
}

fn serve(mut c: TcpStream, root: &str) {
    let _ = c.set_read_timeout(Some(Duration::from_secs(10)));
    let mut req: Vec<u8> = Vec::new();
    let mut buf = [0u8; 8192];
    while find_bytes(&req, b"\r\n\r\n", 0).is_none() && req.len() < 64 * 1024 {
        match c.read(&mut buf) {
            Ok(n) if n > 0 => req.extend_from_slice(&buf[..n]),
            _ => break,
        }
    }
    let line = &req[..find_bytes(&req, b"\r\n", 0).unwrap_or(req.len())];
    let sp1 = line.iter().position(|&b| b == b' ');
    let method = &line[..sp1.unwrap_or(line.len())];
    let target: &[u8] = match sp1 {
        None => b"",
        Some(a) => {
            let rest = &line[a + 1..];
            &rest[..rest.iter().position(|&b| b == b' ').unwrap_or(rest.len())]
        }
    };
    if method != b"GET" && method != b"HEAD" {
        send_status(&mut c, "405 Method Not Allowed");
        return;
    }
    let mut path = url_decode(&target[..target.iter().position(|&b| b == b'?' || b == b'#').unwrap_or(target.len())]);
    if path.is_empty() || path.last() == Some(&b'/') {
        path.extend_from_slice(b"index.html");
    }
    // The first character is dropped as the leading '/', whatever it is, as in the C++ server.
    let rel = String::from_utf8_lossy(path.get(1..).unwrap_or(&[])).replace('/', "\\");
    let file = paths::normalize(&paths::join(root, &rel));
    // "..", drive letters or encoded slashes can't leave the game's folder.
    if !paths::is_under(&file, root) || !paths::is_file(&file) {
        send_status(&mut c, "404 Not Found");
        return;
    }
    let body = paths::read_file(&file).unwrap_or_default();
    let (mut from, mut to) = (0usize, body.len().saturating_sub(1));
    let mut partial = false;
    // Audio and video elements ask for byte ranges ("Range: bytes=100-").
    let lower = req.to_ascii_lowercase();
    if let Some(r) = find_bytes(&lower, b"\r\nrange: bytes=", 0).filter(|_| !body.is_empty()) {
        let end = find_bytes(&req, b"\r\n", r + 2).unwrap_or(req.len());
        let spec = &req[(r + 15).min(end)..end];
        if let Some(dash) = spec.iter().position(|&b| b == b'-') {
            let (a, b) = (&spec[..dash], &spec[dash + 1..]);
            if a.is_empty() && !b.is_empty() {
                let n = strtoull(b).min(body.len());
                from = body.len() - n;
            } else {
                from = strtoull(a);
                if !b.is_empty() {
                    to = strtoull(b).min(body.len() - 1);
                }
            }
            if from > to || from >= body.len() {
                send_status(&mut c, "416 Range Not Satisfiable");
                return;
            }
            partial = true;
        }
    }
    let length = if body.is_empty() { 0 } else { to - from + 1 };
    let mut head = format!(
        "HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {length}\r\nAccept-Ranges: bytes\r\nCache-Control: no-cache\r\nConnection: close\r\n",
        if partial { "206 Partial Content" } else { "200 OK" },
        mime_type(&file)
    );
    if partial {
        head += &format!("Content-Range: bytes {from}-{to}/{}\r\n", body.len());
    }
    head += "\r\n";
    let _ = c.write_all(head.as_bytes());
    if method == b"GET" && length > 0 {
        let _ = c.write_all(&body[from..from + length]);
    }
    let _ = c.shutdown(Shutdown::Write);
}
