//! The built-in netplay relay for the online mods (old/src/core/relay.cpp). Same wire protocol as the mods'
//! gm2_relay.py / sh2_relay.py, so a game can't tell the difference:
//!
//!   messages are NUL-terminated UTF-8 text (Flash XMLSocket)
//!   first message "H|name" makes a client the host, "J|name" a guest
//!   host  -> relay : anything, forwarded to every guest unchanged
//!   guest -> relay : "<msg>", forwarded to the host as "G|<guest id>|<msg>"
//!   relay -> host  : "W|0" welcome, "J|<id>|<name>" join, "L|<id>" leave
//!   relay -> guest : "W|<id>" welcome, "X" when the host leaves
//!   "D|text" from anyone is a debug line for the log
//!
//! It also answers Flash's <policy-file-request/> on the game port (and on 843 when that port is free). One thread,
//! non-blocking sockets and select(), TCP_NODELAY: per-frame input messages are tiny and must not wait for Nagle.
//! Winsock directly rather than std::net, for SO_EXCLUSIVEADDRUSE and select() as the C++ relay has them.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

#[cfg(windows)]
use windows_sys::Win32::Networking::WinSock::{
    AF_INET, FD_SET, FIONBIO, INVALID_SOCKET, IPPROTO_TCP, SO_EXCLUSIVEADDRUSE, SOCK_STREAM, SOCKADDR, SOCKADDR_IN, SOCKET,
    SOCKET_ERROR, SOL_SOCKET, TCP_NODELAY, TIMEVAL, WSACleanup, WSADATA, WSAEACCES, WSAEADDRINUSE, WSAEWOULDBLOCK,
    WSAGetLastError, WSAStartup, accept, bind, closesocket, ioctlsocket, listen, recv, select, send, setsockopt, socket,
};
#[cfg(unix)]
use super::posix::winsock::{
    AF_INET, FD_SET, FIONBIO, INVALID_SOCKET, IPPROTO_TCP, SO_EXCLUSIVEADDRUSE, SOCK_STREAM, SOCKADDR, SOCKADDR_IN, SOCKET,
    SOCKET_ERROR, SOL_SOCKET, TCP_NODELAY, TIMEVAL, WSACleanup, WSADATA, WSAEACCES, WSAEADDRINUSE, WSAEWOULDBLOCK,
    WSAGetLastError, WSAStartup, accept, bind, closesocket, ioctlsocket, listen, recv, select, send, setsockopt, socket,
};

use super::http::win32_error_message;
use super::log;

const POLICY_REQUEST: &[u8] = b"<policy-file-request/>";
const POLICY: &[u8] = b"<?xml version=\"1.0\"?><cross-domain-policy><allow-access-from domain=\"*\" to-ports=\"*\"/></cross-domain-policy>";
const MAX_BUFFERED: usize = 4 * 1024 * 1024; // a peer that sends this much without a NUL is broken
const MAX_CLIENTS: usize = 48; // select() handles 64 sockets
const POLICY_TIMEOUT: Duration = Duration::from_secs(5);
/// winsock2.h's SOMAXCONN (windows-sys carries winsock 1's 5).
const SOMAXCONN: i32 = 0x7fff_ffff;

#[derive(Clone, Debug, PartialEq)]
pub struct RelayPeer {
    pub id: i32,
    pub name: String,
    pub address: String,
    pub host: bool,
}

#[derive(Clone, Debug, Default)]
pub struct RelaySnapshot {
    pub running: bool,
    pub port: i32,
    pub peers: Vec<RelayPeer>, // host first
    pub messages: u64,         // relayed so far
}

/// A listening TCP socket on `addr` (host order), or the Winsock error. Exclusive: two relays silently sharing a
/// port would split the players.
pub(crate) fn listen_on(addr: u32, port: i32, non_blocking: bool) -> Result<SOCKET, i32> {
    unsafe {
        let s = socket(AF_INET as i32, SOCK_STREAM, IPPROTO_TCP);
        if s == INVALID_SOCKET {
            return Err(WSAGetLastError());
        }
        let yes: i32 = 1;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (&yes as *const i32).cast(), 4);
        let mut a: SOCKADDR_IN = std::mem::zeroed();
        a.sin_family = AF_INET;
        a.sin_addr.S_un.S_addr = addr.to_be();
        a.sin_port = (port as u16).to_be();
        let mut nb: u32 = 1;
        if bind(s, (&a as *const SOCKADDR_IN).cast(), std::mem::size_of::<SOCKADDR_IN>() as i32) == SOCKET_ERROR
            || listen(s, SOMAXCONN) == SOCKET_ERROR
            || (non_blocking && ioctlsocket(s, FIONBIO, &mut nb) == SOCKET_ERROR)
        {
            let e = WSAGetLastError();
            closesocket(s);
            return Err(e);
        }
        Ok(s)
    }
}

pub(crate) fn wsa_startup() -> i32 {
    let mut wsa: WSADATA = unsafe { std::mem::zeroed() };
    unsafe { WSAStartup(0x0202, &mut wsa) }
}

fn strip_newlines(s: &[u8]) -> &[u8] {
    let (mut a, mut b) = (0, s.len());
    while a < b && (s[a] == b'\r' || s[a] == b'\n') {
        a += 1;
    }
    while b > a && (s[b - 1] == b'\r' || s[b - 1] == b'\n') {
        b -= 1;
    }
    &s[a..b]
}

fn find(hay: &[u8], needle: u8, from: usize) -> Option<usize> {
    hay[from..].iter().position(|&b| b == needle).map(|p| p + from)
}

#[derive(PartialEq, Clone, Copy)]
enum Role {
    None,
    Host,
    Guest,
}

struct Client {
    s: SOCKET,
    id: i32,
    input: Vec<u8>,
    out: Vec<u8>,
    name: Vec<u8>,
    address: String,
    role: Role,
    policy_only: bool,    // a connection on port 843
    policy_checked: bool, // the first bytes were not a policy request
    close_after_flush: bool,
    dead: bool,
    since: Instant,
}

struct Shared {
    stop: AtomicBool,
    messages: AtomicU64,
    snap: Mutex<RelaySnapshot>,
}

/// The relay thread's own state.
struct Core {
    shared: Arc<Shared>,
    port: i32,
    listener: SOCKET,
    policy_listener: SOCKET,
    clients: Vec<Client>,
    next_id: i32,
    host_id: i32,
}

#[derive(Default)]
pub struct Relay {
    shared: Option<Arc<Shared>>,
    thread: Option<JoinHandle<Core>>,
    port: i32,
    running: bool,
}

fn publish(shared: &Shared, running: bool, port: i32, clients: &[Client]) {
    let mut snap = RelaySnapshot { running, port, ..Default::default() };
    for c in clients {
        if c.dead || c.role == Role::None {
            continue;
        }
        let p = RelayPeer { id: c.id, name: String::from_utf8_lossy(&c.name).into_owned(), address: c.address.clone(), host: c.role == Role::Host };
        if p.host { snap.peers.insert(0, p) } else { snap.peers.push(p) }
    }
    *shared.snap.lock().unwrap() = snap;
}

fn fd_set(set: &mut FD_SET, s: SOCKET) {
    if (set.fd_count as usize) < set.fd_array.len() {
        set.fd_array[set.fd_count as usize] = s;
        set.fd_count += 1;
    }
}

fn fd_isset(set: &FD_SET, s: SOCKET) -> bool {
    set.fd_array[..set.fd_count as usize].contains(&s)
}

impl Relay {
    pub fn new() -> Self {
        Self::default()
    }

    /// Binds at once so the caller learns about "port in use" (likely another relay) right away: the error's second
    /// value says so.
    pub fn start(&mut self, port: i32) -> Result<(), (String, bool)> {
        self.stop();
        let rc = wsa_startup();
        if rc != 0 {
            return Err((win32_error_message(rc as u32), false));
        }
        let listener = match listen_on(0, port, true) {
            Ok(s) => s,
            Err(code) => {
                let error = win32_error_message(code as u32);
                log::warn(&format!("Relay: cannot listen on port {port}: {error}"));
                unsafe { WSACleanup() };
                return Err((error, code == WSAEADDRINUSE || code == WSAEACCES));
            }
        };
        // Flash asks port 843 for the policy first; when it can't connect it falls back to the game port, so failing
        // here only costs the game a few seconds.
        let policy_listener = if port == 843 { INVALID_SOCKET } else { listen_on(0, 843, true).unwrap_or(INVALID_SOCKET) };
        self.port = port;
        self.running = true;
        let shared = Arc::new(Shared { stop: AtomicBool::new(false), messages: AtomicU64::new(0), snap: Mutex::new(RelaySnapshot::default()) });
        log::info(&format!(
            "Relay listening on port {port}{}",
            if policy_listener != INVALID_SOCKET { " (policy server on 843)" } else { " (policy served on the game port)" }
        ));
        publish(&shared, true, port, &[]);
        let mut core = Core { shared: shared.clone(), port, listener, policy_listener, clients: Vec::new(), next_id: 1, host_id: 0 };
        self.shared = Some(shared);
        self.thread = Some(std::thread::spawn(move || {
            core.run();
            core
        }));
        Ok(())
    }

    pub fn stop(&mut self) {
        if !self.running && self.thread.is_none() {
            return;
        }
        if let Some(s) = &self.shared {
            s.stop.store(true, Ordering::SeqCst);
        }
        if let Some(t) = self.thread.take() {
            if let Ok(core) = t.join() {
                unsafe {
                    for c in &core.clients {
                        closesocket(c.s);
                    }
                    closesocket(core.listener);
                    if core.policy_listener != INVALID_SOCKET {
                        closesocket(core.policy_listener);
                    }
                }
            }
        }
        self.running = false;
        unsafe { WSACleanup() };
        log::info("Relay stopped");
        if let Some(s) = &self.shared {
            publish(s, false, self.port, &[]);
        }
    }

    pub fn running(&self) -> bool {
        self.running
    }
    pub fn port(&self) -> i32 {
        self.port
    }
    pub fn snapshot(&self) -> RelaySnapshot {
        match &self.shared {
            Some(s) => {
                let mut snap = s.snap.lock().unwrap().clone();
                snap.messages = s.messages.load(Ordering::Relaxed);
                snap
            }
            None => RelaySnapshot::default(),
        }
    }
}

impl Drop for Relay {
    fn drop(&mut self) {
        self.stop();
    }
}

impl Core {
    fn run(&mut self) {
        while !self.shared.stop.load(Ordering::SeqCst) {
            let mut rd = FD_SET::default();
            let mut wr = FD_SET::default();
            fd_set(&mut rd, self.listener);
            if self.policy_listener != INVALID_SOCKET {
                fd_set(&mut rd, self.policy_listener);
            }
            for c in &self.clients {
                fd_set(&mut rd, c.s);
                if !c.out.is_empty() {
                    fd_set(&mut wr, c.s);
                }
            }
            // The timeout only bounds how fast stop() is noticed; data wakes select at once.
            let tv = TIMEVAL { tv_sec: 0, tv_usec: 100 * 1000 };
            let n = unsafe { select(0, &mut rd, &mut wr, std::ptr::null_mut(), &tv) };
            if n == SOCKET_ERROR {
                log::error(&format!("Relay: select failed: {}", win32_error_message(unsafe { WSAGetLastError() } as u32)));
                std::thread::sleep(Duration::from_millis(50));
                continue;
            }
            if fd_isset(&rd, self.listener) {
                self.accept(self.listener, false);
            }
            if self.policy_listener != INVALID_SOCKET && fd_isset(&rd, self.policy_listener) {
                self.accept(self.policy_listener, true);
            }
            // Index loop: handlers may add output to other clients but never add clients.
            for i in 0..self.clients.len() {
                if self.clients[i].dead {
                    continue;
                }
                let s = self.clients[i].s;
                if fd_isset(&rd, s) {
                    self.on_data(i);
                }
                if !self.clients[i].dead && fd_isset(&wr, s) {
                    self.flush(i);
                }
                let c = &mut self.clients[i];
                if !c.dead && c.policy_only && c.since.elapsed() > POLICY_TIMEOUT {
                    c.dead = true;
                }
            }
            let mut changed = false;
            self.clients.retain(|c| {
                if c.dead {
                    changed = changed || c.role != Role::None;
                    unsafe { closesocket(c.s) };
                }
                !c.dead
            });
            if changed {
                publish(&self.shared, true, self.port, &self.clients);
            }
        }
    }

    fn accept(&mut self, listener: SOCKET, policy_only: bool) {
        loop {
            let mut addr: SOCKADDR_IN = unsafe { std::mem::zeroed() };
            let mut len = std::mem::size_of::<SOCKADDR_IN>() as i32;
            let s = unsafe { accept(listener, (&mut addr as *mut SOCKADDR_IN).cast::<SOCKADDR>(), &mut len) };
            if s == INVALID_SOCKET {
                return; // WSAEWOULDBLOCK: no more pending
            }
            if self.clients.len() >= MAX_CLIENTS {
                unsafe { closesocket(s) };
                continue;
            }
            let mut nb: u32 = 1;
            let yes: i32 = 1;
            unsafe {
                ioctlsocket(s, FIONBIO, &mut nb);
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (&yes as *const i32).cast(), 4);
            }
            let b = unsafe { addr.sin_addr.S_un.S_addr }.to_ne_bytes();
            let id = self.next_id;
            self.next_id += 1;
            self.clients.push(Client {
                s,
                id,
                input: Vec::new(),
                out: Vec::new(),
                name: b"?".to_vec(),
                address: format!("{}.{}.{}.{}", b[0], b[1], b[2], b[3]),
                role: Role::None,
                policy_only,
                policy_checked: false,
                close_after_flush: false,
                dead: false,
                since: Instant::now(),
            });
        }
    }

    fn find(&self, id: i32) -> Option<usize> {
        self.clients.iter().position(|c| c.id == id && !c.dead)
    }

    fn on_data(&mut self, i: usize) {
        let mut buf = vec![0u8; 64 * 1024];
        loop {
            let n = unsafe { recv(self.clients[i].s, buf.as_mut_ptr(), buf.len() as i32, 0) };
            if n > 0 {
                self.clients[i].input.extend_from_slice(&buf[..n as usize]);
                continue;
            }
            if n == 0 || unsafe { WSAGetLastError() } != WSAEWOULDBLOCK {
                // What came with the goodbye is not read, as in the C++ relay.
                self.drop_client(i);
                return;
            }
            break;
        }
        if self.clients[i].policy_only {
            self.clients[i].close_after_flush = true; // before send(): it may flush everything right away
            self.send(i, POLICY);
            self.clients[i].input.clear();
            return;
        }
        if !self.clients[i].policy_checked && self.clients[i].role == Role::None {
            let c = &mut self.clients[i];
            let k = c.input.len().min(POLICY_REQUEST.len());
            if c.input[..k] == POLICY_REQUEST[..k] {
                if c.input.len() < POLICY_REQUEST.len() {
                    return; // could still become a policy request
                }
                c.close_after_flush = true;
                self.send(i, POLICY);
                self.clients[i].input.clear();
                return;
            }
            c.policy_checked = true;
        }
        let mut start = 0;
        while !self.clients[i].dead {
            let Some(nul) = find(&self.clients[i].input, 0, start) else { break };
            let msg = strip_newlines(&self.clients[i].input[start..nul]).to_vec();
            start = nul + 1;
            if !msg.is_empty() {
                self.on_message(i, &msg);
            }
        }
        let c = &mut self.clients[i];
        let start = start.min(c.input.len());
        c.input.drain(..start);
        if c.input.len() > MAX_BUFFERED {
            log::warn(&format!("Relay: dropping {} (message too long)", c.address));
            self.drop_client(i);
        }
    }

    fn on_message(&mut self, i: usize, msg: &[u8]) {
        if msg.starts_with(b"D|") {
            return; // a debug line: the C++ relay logs it at debug level, which its log leaves out
        }
        if self.clients[i].role == Role::None {
            let bar = msg.iter().position(|&b| b == b'|');
            let kind = &msg[..bar.unwrap_or(msg.len())];
            let name = bar.map(|b| &msg[b + 1..]).unwrap_or(&[]);
            self.clients[i].name = if name.is_empty() { b"Player".to_vec() } else { name.to_vec() };
            let id = self.clients[i].id;
            let name = String::from_utf8_lossy(&self.clients[i].name).into_owned();
            let address = self.clients[i].address.clone();
            if kind == b"H" {
                if self.host_id != 0 && self.find(self.host_id).is_some() {
                    self.send(i, b"E|A host is already connected to this server");
                    return;
                }
                self.clients[i].role = Role::Host;
                self.host_id = id;
                log::info(&format!("Relay: host '{name}' connected from {address}"));
                self.send(i, b"W|0");
                for g in 0..self.clients.len() {
                    if !self.clients[g].dead && self.clients[g].role == Role::Guest {
                        let mut m = format!("J|{}|", self.clients[g].id).into_bytes();
                        m.extend_from_slice(&self.clients[g].name);
                        self.send(i, &m);
                    }
                }
                publish(&self.shared, true, self.port, &self.clients);
            } else if kind == b"J" {
                self.clients[i].role = Role::Guest;
                log::info(&format!("Relay: friend '{name}' (id {id}) connected from {address}"));
                self.send(i, format!("W|{id}").as_bytes());
                if let Some(h) = self.find(self.host_id) {
                    let mut m = format!("J|{id}|").into_bytes();
                    m.extend_from_slice(&self.clients[i].name);
                    self.send(h, &m);
                }
                publish(&self.shared, true, self.port, &self.clients);
            }
            return;
        }
        self.shared.messages.fetch_add(1, Ordering::Relaxed);
        if self.clients[i].role == Role::Host {
            for g in 0..self.clients.len() {
                if !self.clients[g].dead && self.clients[g].role == Role::Guest {
                    self.send(g, msg);
                }
            }
        } else if let Some(h) = self.find(self.host_id) {
            let mut m = format!("G|{}|", self.clients[i].id).into_bytes();
            m.extend_from_slice(msg);
            self.send(h, &m);
        }
    }

    fn send(&mut self, i: usize, msg: &[u8]) {
        let c = &mut self.clients[i];
        if c.dead {
            return;
        }
        c.out.extend_from_slice(msg);
        c.out.push(0);
        if c.out.len() > MAX_BUFFERED {
            // A peer that stopped reading entirely.
            self.drop_client(i);
            return;
        }
        self.flush(i);
    }

    fn flush(&mut self, i: usize) {
        while !self.clients[i].out.is_empty() {
            let c = &self.clients[i];
            let len = c.out.len().min(1 << 20) as i32;
            let n = unsafe { send(c.s, c.out.as_ptr(), len, 0) };
            if n > 0 {
                self.clients[i].out.drain(..n as usize);
                continue;
            }
            if unsafe { WSAGetLastError() } == WSAEWOULDBLOCK {
                return; // select() says when it drains
            }
            self.drop_client(i);
            return;
        }
        if self.clients[i].close_after_flush {
            self.clients[i].dead = true;
        }
    }

    fn drop_client(&mut self, i: usize) {
        if self.clients[i].dead {
            return;
        }
        self.clients[i].dead = true;
        let (role, id) = (self.clients[i].role, self.clients[i].id);
        let name = String::from_utf8_lossy(&self.clients[i].name).into_owned();
        if role == Role::Host && self.host_id == id {
            self.host_id = 0;
            log::info(&format!("Relay: host '{name}' left"));
            for g in 0..self.clients.len() {
                if !self.clients[g].dead && self.clients[g].role == Role::Guest {
                    self.send(g, b"X");
                }
            }
        } else if role == Role::Guest {
            log::info(&format!("Relay: friend '{name}' left"));
            if let Some(h) = self.find(self.host_id) {
                self.send(h, format!("L|{id}").as_bytes());
            }
        }
    }
}
