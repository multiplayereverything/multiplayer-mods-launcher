//! The C++ tests for part 3 (old/tests/test_relay.cpp: the relay against real loopback sockets; test_core.cpp: command
//! line quoting, the environment block, the web server, Discord's messages), ported case for case, and the launch
//! helpers the window's command uses. A whole launch is compared with the C++ launcher by tests/parity/launch.mjs.

use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

use super::config::{ActionSpec, PlayerSpec};
use super::discord::{DiscordPresence, Presence, activity, frame, handshake};
use super::launch::{add_flash_vars, fill_placeholders, game_values, normalize_code, stable_port, with_default_port};
use super::library::ResolvedAction;
use super::process::{build_command_line, quote_arg};
#[cfg(windows)]
use super::process::environment_block;
use super::relay::Relay;
use super::tests::TempDir;
use super::webserver::{WebServer, mime_type};

fn free_port() -> i32 {
    static NEXT: std::sync::atomic::AtomicI32 = std::sync::atomic::AtomicI32::new(0);
    47700 + (std::process::id() % 400) as i32 + NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
}

struct Peer {
    s: Option<TcpStream>,
    buf: Vec<u8>,
}

impl Peer {
    fn new(port: i32) -> Peer {
        let s = TcpStream::connect(("127.0.0.1", port as u16)).ok();
        if let Some(s) = &s {
            s.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
        }
        Peer { s, buf: Vec::new() }
    }
    fn ok(&self) -> bool {
        self.s.is_some()
    }
    fn send_raw(&mut self, data: &[u8]) {
        let _ = self.s.as_mut().unwrap().write_all(data);
    }
    fn send_msg(&mut self, msg: &str) {
        let mut d = msg.as_bytes().to_vec();
        d.push(0);
        self.send_raw(&d);
    }
    /// The next NUL-terminated message; "<closed>" or "<timeout>" otherwise.
    fn recv_msg(&mut self) -> String {
        loop {
            if let Some(nul) = self.buf.iter().position(|&b| b == 0) {
                let m: Vec<u8> = self.buf.drain(..=nul).collect();
                return String::from_utf8_lossy(&m[..m.len() - 1]).into_owned();
            }
            let mut tmp = [0u8; 4096];
            match self.s.as_mut().unwrap().read(&mut tmp) {
                Ok(0) => return "<closed>".into(),
                Ok(n) => self.buf.extend_from_slice(&tmp[..n]),
                Err(e) if matches!(e.kind(), std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock) => return "<timeout>".into(),
                Err(_) => return "<closed>".into(),
            }
        }
    }
    fn close(&mut self) {
        self.s = None;
    }
}

fn wait_for(r: &Relay, peers: usize) -> bool {
    for _ in 0..100 {
        if r.snapshot().peers.len() == peers {
            return true;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    false
}

#[test]
fn relay_answers_flash_policy_request() {
    let mut relay = Relay::new();
    let port = free_port();
    relay.start(port).unwrap();
    let mut p = Peer::new(port);
    assert!(p.ok());
    p.send_raw(b"<policy-file-request/>\0");
    let policy = p.recv_msg();
    assert!(policy.contains("<cross-domain-policy>"));
    assert!(policy.contains("to-ports=\"*\""));
    assert_eq!(p.recv_msg(), "<closed>");
}

#[test]
fn relay_policy_request_split_across_packets() {
    let mut relay = Relay::new();
    let port = free_port();
    relay.start(port).unwrap();
    let mut p = Peer::new(port);
    assert!(p.ok());
    p.send_raw(b"<policy-fi");
    std::thread::sleep(Duration::from_millis(150));
    p.send_raw(b"le-request/>\0");
    assert!(p.recv_msg().contains("<cross-domain-policy>"));
    assert_eq!(p.recv_msg(), "<closed>");
}

#[test]
fn relay_routes_host_and_guests() {
    let mut relay = Relay::new();
    let port = free_port();
    relay.start(port).unwrap();

    let mut guest_early = Peer::new(port); // a friend may connect before the host
    assert!(guest_early.ok());
    guest_early.send_msg("J|Early");
    let w1 = guest_early.recv_msg();
    assert!(w1.starts_with("W|"));
    let early_id = w1[2..].to_string();

    let mut host = Peer::new(port);
    assert!(host.ok());
    host.send_msg("H|Alice");
    assert_eq!(host.recv_msg(), "W|0");
    assert_eq!(host.recv_msg(), format!("J|{early_id}|Early")); // told about guests already there

    let mut guest = Peer::new(port);
    assert!(guest.ok());
    guest.send_msg("\r\nJ|Bob"); // newlines around a message are ignored
    let w2 = guest.recv_msg();
    assert!(w2.starts_with("W|"));
    let bob_id = w2[2..].to_string();
    assert_eq!(host.recv_msg(), format!("J|{bob_id}|Bob"));
    assert!(wait_for(&relay, 3));
    let snap = relay.snapshot();
    assert_eq!(snap.peers.len(), 3);
    assert!(snap.peers[0].host && snap.peers[0].name == "Alice");

    host.send_msg("F|12|keys"); // host -> every guest, unchanged
    assert_eq!(guest.recv_msg(), "F|12|keys");
    assert_eq!(guest_early.recv_msg(), "F|12|keys");

    guest.send_msg("I|3|left"); // guest -> host, tagged with the guest id
    assert_eq!(host.recv_msg(), format!("G|{bob_id}|I|3|left"));

    guest.send_msg("D|debug line"); // debug lines go to the log only
    guest.send_msg("I|4");
    assert_eq!(host.recv_msg(), format!("G|{bob_id}|I|4"));

    let mut second = Peer::new(port);
    assert!(second.ok());
    second.send_msg("H|Mallory");
    assert_eq!(second.recv_msg(), "E|A host is already connected to this server");

    guest.close();
    assert_eq!(host.recv_msg(), format!("L|{bob_id}"));
    host.close();
    assert_eq!(guest_early.recv_msg(), "X");
    assert!(wait_for(&relay, 1));
    assert!(relay.snapshot().messages >= 3);
}

#[test]
fn relay_keeps_message_order_under_load() {
    let mut relay = Relay::new();
    let port = free_port();
    relay.start(port).unwrap();
    let (mut host, mut guest) = (Peer::new(port), Peer::new(port));
    assert!(host.ok() && guest.ok());
    host.send_msg("H|h");
    assert_eq!(host.recv_msg(), "W|0");
    guest.send_msg("J|g");
    assert!(guest.recv_msg().starts_with("W|"));
    host.recv_msg(); // J|id|g
    let mut batch = Vec::new();
    for i in 0..2000 {
        batch.extend_from_slice(format!("F|{i}\0").as_bytes());
    }
    host.send_raw(&batch); // many messages in few TCP segments
    for i in 0..2000 {
        assert_eq!(guest.recv_msg(), format!("F|{i}"));
    }
}

#[test]
fn relay_reports_port_in_use() {
    let (mut a, mut b) = (Relay::new(), Relay::new());
    let port = free_port();
    a.start(port).unwrap();
    let (_, in_use) = b.start(port).unwrap_err();
    assert!(in_use);
    a.stop();
    assert!(!a.running());
    assert!(b.start(port).is_ok()); // free again after stop
    assert!(b.snapshot().running && b.snapshot().port == port);
}

#[test]
fn command_line_quoting() {
    assert_eq!(quote_arg("plain"), "plain");
    assert_eq!(quote_arg("has space"), "\"has space\"");
    assert_eq!(quote_arg(""), "\"\"");
    assert_eq!(quote_arg("C:\\dir with space\\"), "\"C:\\dir with space\\\\\"");
    assert_eq!(quote_arg("say \"hi\""), "\"say \\\"hi\\\"\"");
    assert_eq!(quote_arg("a\\\\\"b c"), "\"a\\\\\\\\\\\"b c\"");
    assert_eq!(
        build_command_line("C:\\Program Files\\r.exe", &["--a".into(), "C:\\My Games\\g.swf".into()]),
        "\"C:\\Program Files\\r.exe\" --a \"C:\\My Games\\g.swf\""
    );
}

#[cfg(windows)]
#[test]
fn environment_block_sets_and_replaces() {
    // "A=1", "=C:=C:\d", "Path=x" as Windows hands them over, NUL-separated and NUL-ended.
    let w = |s: &str| s.encode_utf16().collect::<Vec<u16>>();
    let current = w("A=1\0=C:=C:\\d\0Path=x\0\0");
    let block = environment_block(&current, &[("PATH".into(), "y".into()), ("TV".into(), "1".into())]);
    assert_eq!(block, w("A=1\0=C:=C:\\d\0PATH=y\0TV=1\0\0"));
}

fn http_get(port: i32, path: &str, extra: &str) -> String {
    let Ok(mut s) = TcpStream::connect(("127.0.0.1", port as u16)) else { return String::new() };
    let _ = s.write_all(format!("GET {path} HTTP/1.1\r\nHost: x\r\n{extra}\r\n").as_bytes());
    let mut out = Vec::new();
    let _ = s.read_to_end(&mut out);
    String::from_utf8_lossy(&out).into_owned()
}

#[test]
fn web_server_serves_only_its_folder() {
    let dir = TempDir::new("web");
    let write = |rel: &str, text: &str| {
        let p = dir.join(rel);
        std::fs::create_dir_all(super::paths::parent(&p)).unwrap();
        std::fs::write(p, text).unwrap();
    };
    write("game\\index.html", "<h1>game</h1>");
    write("game\\data\\level 1.json", "0123456789");
    write("secret.txt", "no");
    let mut web = WebServer::new();
    web.start(&dir.join("game"), 0).unwrap();
    let page = http_get(web.port(), "/", "");
    assert!(page.contains("200 OK") && page.contains("text/html") && page.contains("<h1>game</h1>"));
    assert!(http_get(web.port(), "/data/level%201.json", "").contains("0123456789"));
    let range = http_get(web.port(), "/data/level%201.json", "Range: bytes=2-4\r\n");
    assert!(range.contains("206 Partial") && range.contains("\r\n\r\n234"), "{range}");
    let suffix = http_get(web.port(), "/data/level%201.json", "range: bytes=-3\r\n");
    assert!(suffix.contains("Content-Range: bytes 7-9/10") && suffix.ends_with("789"), "{suffix}");
    assert!(http_get(web.port(), "/data/level%201.json", "Range: bytes=20-\r\n").contains("416"));
    assert!(http_get(web.port(), "/../secret.txt", "").contains("404"));
    assert!(http_get(web.port(), "/%2e%2e/secret.txt", "").contains("404"));
    assert!(http_get(web.port(), "/..%5csecret.txt", "").contains("404"));
    let port = web.port();
    web.stop();
    assert!(TcpStream::connect(("127.0.0.1", port as u16)).is_err());
    assert_eq!(mime_type("x.wasm"), "application/wasm");
    assert!(mime_type("x.JS").starts_with("text/javascript"));
    assert_eq!(mime_type("noext"), "application/octet-stream");
}

#[test]
fn discord_presence_messages() {
    let hello = handshake("42");
    assert!(hello.contains("\"v\":1") && hello.contains("\"client_id\":\"42\""));
    let p = Presence { details: "Playing Gun Mayhem 2".into(), state: "Room VK7Q2".into(), start: 1700000000 };
    let set = activity(Some(&p), 99, 3);
    assert!(set.contains("\"cmd\":\"SET_ACTIVITY\""));
    assert!(set.contains("\"pid\":99") && set.contains("\"nonce\":\"3\""));
    assert!(set.contains("\"details\":\"Playing Gun Mayhem 2\""));
    assert!(set.contains("\"start\":1700000000"));
    assert!(activity(None, 99, 4).contains("\"activity\":null"));
    // Discord rejects fields shorter than 2 characters and longer than 128.
    let short = activity(Some(&Presence { details: "x".into(), state: "y".repeat(300), start: 0 }), 1, 1);
    assert!(!short.contains("details") && !short.contains("timestamps") && short.contains(&format!("\"state\":\"{}\"", "y".repeat(120))));
    let f = frame(1, "{}");
    assert_eq!(f, [1, 0, 0, 0, 2, 0, 0, 0, b'{', b'}']);
}

/// The presence talks to a stand-in for the Discord app on a pipe of its own: the handshake, the activity, and the
/// clearing message when it goes.
#[cfg(windows)]
#[test]
fn discord_presence_over_a_pipe() {
    use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{PIPE_ACCESS_DUPLEX, ReadFile, WriteFile};
    use windows_sys::Win32::System::Pipes::{ConnectNamedPipe, CreateNamedPipeW, PIPE_TYPE_BYTE, PIPE_WAIT};
    let prefix = format!(r"\\.\pipe\fl-test-discord-{}-", std::process::id());
    let name = super::paths::wide(&format!("{prefix}0"));
    let pipe = unsafe { CreateNamedPipeW(name.as_ptr(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, std::ptr::null()) };
    assert_ne!(pipe, INVALID_HANDLE_VALUE);
    let pipe = pipe as usize;
    let server = std::thread::spawn(move || {
        let h = pipe as *mut core::ffi::c_void;
        unsafe { ConnectNamedPipe(h, std::ptr::null_mut()) };
        let read = || -> Option<(u32, String)> {
            let mut head = [0u8; 8];
            let mut got = 0u32;
            let mut off = 0;
            while off < 8 {
                if unsafe { ReadFile(h, head[off..].as_mut_ptr(), (8 - off) as u32, &mut got, std::ptr::null_mut()) } == 0 || got == 0 {
                    return None;
                }
                off += got as usize;
            }
            let len = u32::from_le_bytes(head[4..].try_into().unwrap()) as usize;
            let mut body = vec![0u8; len];
            let mut off = 0;
            while off < len {
                if unsafe { ReadFile(h, body[off..].as_mut_ptr(), (len - off) as u32, &mut got, std::ptr::null_mut()) } == 0 {
                    return None;
                }
                off += got as usize;
            }
            Some((u32::from_le_bytes(head[..4].try_into().unwrap()), String::from_utf8(body).unwrap()))
        };
        let reply = |json: &str| {
            let f = frame(1, json);
            let mut n = 0u32;
            unsafe { WriteFile(h, f.as_ptr(), f.len() as u32, &mut n, std::ptr::null_mut()) };
        };
        let mut got = Vec::new();
        while let Some(m) = read() {
            got.push(m);
            reply(r#"{"evt":"READY"}"#);
        }
        unsafe { CloseHandle(h) };
        got
    });
    {
        let d = DiscordPresence::with_pipe("1234", &prefix);
        d.set(&Presence { details: "Playing Test".into(), state: "Room ABCDE".into(), start: 1700000000 });
        std::thread::sleep(Duration::from_millis(800));
    }
    let got = server.join().unwrap();
    assert!(got.len() >= 3, "{got:?}");
    assert_eq!(got[0], (0, handshake("1234")));
    assert_eq!(got[1].0, 1);
    assert!(got[1].1.contains("\"details\":\"Playing Test\"") && got[1].1.contains("\"state\":\"Room ABCDE\"") && got[1].1.contains("\"nonce\":\"1\""));
    assert!(got.last().unwrap().1.contains("\"activity\":null"));
}

/// The same over the Unix socket the Discord app listens on in Linux.
#[cfg(unix)]
#[test]
fn discord_presence_over_a_socket() {
    let dir = TempDir::new("discord");
    let prefix = format!("{}/discord-ipc-", dir.path());
    let listener = std::os::unix::net::UnixListener::bind(format!("{prefix}0")).unwrap();
    let server = std::thread::spawn(move || {
        let (mut s, _) = listener.accept().unwrap();
        let mut got = Vec::new();
        loop {
            let mut head = [0u8; 8];
            if s.read_exact(&mut head).is_err() {
                break;
            }
            let mut body = vec![0u8; u32::from_le_bytes(head[4..].try_into().unwrap()) as usize];
            if s.read_exact(&mut body).is_err() {
                break;
            }
            got.push((u32::from_le_bytes(head[..4].try_into().unwrap()), String::from_utf8(body).unwrap()));
            let _ = s.write_all(&frame(1, r#"{"evt":"READY"}"#));
        }
        got
    });
    {
        let d = DiscordPresence::with_pipe("1234", &prefix);
        d.set(&Presence { details: "Playing Test".into(), state: "Room ABCDE".into(), start: 1700000000 });
        std::thread::sleep(Duration::from_millis(800));
    }
    let got = server.join().unwrap();
    assert!(got.len() >= 3, "{got:?}");
    assert_eq!(got[0], (0, handshake("1234")));
    assert_eq!(got[1].0, 1);
    assert!(got[1].1.contains("\"details\":\"Playing Test\"") && got[1].1.contains("\"nonce\":\"1\""));
    assert!(got.last().unwrap().1.contains("\"activity\":null"));
}

fn player(flashvar_arg: &str) -> PlayerSpec {
    let text = format!(r#"{{ "players": [{{ "id": "p", "paths": ["p.exe"], "flashvarArg": "{flashvar_arg}" }}], "games": [] }}"#);
    super::config::load_config(text.as_bytes(), r"C:\x").config.players.remove(0)
}

#[test]
fn flashvars_go_before_the_game_file() {
    let p = player("-P${KEY}=${VALUE}");
    let mut r = ResolvedAction { player: Some(&p), swf: "g.swf".into(), args: vec!["--a".into(), "g.swf".into(), "--after".into()], ..Default::default() };
    let action = ActionSpec { flashvars: vec![("online".into(), "1".into()), ("name".into(), "${PLAYER_NAME}".into()), ("e".into(), "".into())], ..Default::default() };
    let values = game_values(&action, "Ann", &[("join".into(), "ABCDE".into())]);
    assert_eq!(values, vec![("online".into(), "1".into()), ("name".into(), "Ann".into()), ("join".into(), "ABCDE".into())]);
    add_flash_vars(&mut r, &values);
    assert_eq!(r.args, ["--a", "-Ponline=1", "-Pname=Ann", "-Pjoin=ABCDE", "g.swf", "--after"]);
    // No player name: the value is left out; joinArgs: the room code isn't a value.
    let joining = ActionSpec { join_args: vec!["--j".into()], ..action };
    assert_eq!(game_values(&joining, "", &[("join".into(), "X".into())]), vec![("online".to_string(), "1".to_string())]);
    // A player without flashvarArg takes none; the file not among the arguments: at the end.
    let none = player("");
    let mut r2 = ResolvedAction { player: Some(&none), swf: "g.swf".into(), args: vec!["g.swf".into()], ..Default::default() };
    add_flash_vars(&mut r2, &values);
    assert_eq!(r2.args, ["g.swf"]);
    let mut r3 = ResolvedAction { player: Some(&p), swf: "http://x/g.swf".into(), args: vec!["--a".into()], ..Default::default() };
    add_flash_vars(&mut r3, &values[..1]);
    assert_eq!(r3.args, ["--a", "-Ponline=1"]);
}

#[test]
fn placeholders_are_filled_or_named() {
    let mut args: Vec<String> = vec!["--s".into(), "${SERVER}".into(), "--n=${PLAYER_NAME}".into(), "${CODE}${CODE}".into()];
    assert_eq!(fill_placeholders(&mut args, "", "AB", "h:1", ""), None);
    assert_eq!(args, ["--s", "h:1", "--n=", "ABAB"]);
    let mut need = vec!["--j".to_string(), "${ADDRESS}".into()];
    assert_eq!(fill_placeholders(&mut need, "n", "c", "s", ""), Some("ADDRESS"));
    let mut code = vec!["${CODE}".to_string()];
    assert_eq!(fill_placeholders(&mut code, "n", "", "s", "a"), Some("CODE"));
}

#[test]
fn an_address_without_a_port_gets_the_games_port() {
    assert_eq!(with_default_port("26.1.2.3", 7790), "26.1.2.3:7790");
    assert_eq!(with_default_port(" 26.1.2.3 ", 7800), "26.1.2.3:7800");
    assert_eq!(with_default_port("26.1.2.3:", 7790), "26.1.2.3:7790");
    assert_eq!(with_default_port("host.example.com", 7790), "host.example.com:7790");
    assert_eq!(with_default_port("[fe80::1]", 7790), "[fe80::1]:7790");
    assert_eq!(with_default_port("[fe80::1]:", 7790), "[fe80::1]:7790");
    // What the person typed wins, and no address stays no address so the launch still reports it missing.
    assert_eq!(with_default_port("26.1.2.3:9000", 7790), "26.1.2.3:9000");
    assert_eq!(with_default_port("[fe80::1]:9000", 7790), "[fe80::1]:9000");
    assert_eq!(with_default_port("fe80::1", 7790), "fe80::1");
    assert_eq!(with_default_port("  ", 7790), "");
}

#[test]
fn room_codes_and_stable_ports() {
    assert_eq!(normalize_code(" ab c-de "), "ABCDE");
    // As the C++ launcher: letters outside ASCII are kept, only ASCII ones raised (towupper in the C locale).
    assert_eq!(normalize_code("\u{e9}a1\u{df}_."), "\u{e9}A1\u{df}");
    // FNV-1a of the id, 47000..48999: the same port every time for the game's saves.
    assert_eq!(stable_port(""), 47000 + (2166136261u32 % 2000) as i32);
    let p = stable_port("web");
    assert!((47000..49000).contains(&p) && p == stable_port("web") && p != stable_port("web2"));
}

/// Linux: a game run through Wine with BepInEx's winhttp.dll beside it gets Wine told to load that one.
#[cfg(unix)]
#[test]
fn wine_loads_a_winhttp_loader_beside_the_game() {
    use super::process::needs_winhttp_override;
    let dir = TempDir::new("wine");
    let exe = dir.join("Game.exe");
    std::fs::write(&exe, b"MZ").unwrap();
    let args = vec!["-x".to_string()];
    let via_wine = vec!["/usr/bin/wine".to_string(), exe.clone(), "-x".to_string()];
    let direct = vec![exe.clone(), "-x".to_string()];
    let none: Vec<(String, String)> = Vec::new();
    let set = vec![("WINEDLLOVERRIDES".to_string(), "d3d9=n".to_string())];
    let outside = std::env::var_os("WINEDLLOVERRIDES").is_some();
    assert!(!needs_winhttp_override(&exe, &args, &via_wine, &none), "no loader there yet");
    std::fs::write(dir.join("WinHttp.dll"), b"MZ").unwrap();
    assert_eq!(needs_winhttp_override(&exe, &args, &via_wine, &none), !outside);
    assert!(!needs_winhttp_override(&exe, &args, &direct, &none));
    assert!(!needs_winhttp_override(&exe, &args, &via_wine, &set), "the mod's own setting wins");
}
