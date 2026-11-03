//! Open rooms on the Multiplayer Everything central server (old/src/core/rooms.cpp, and the `--rooms` branches of
//! old/src/app/main.cpp with cli_json.cpp's `roomsJson`). "RELAY|2|ROOMS|<game>" answers
//! "ROOMS|<code>,<players>,<max>,<locked>,<state>,<region>,<ping>,<version>,<name>;..."; an older server only knows
//! "RELAY|1|LIST|<game>" -> "LIST|<code>,<players>,<max>,<name>;...". Both are NUL-terminated XMLSocket text. The
//! server to ask comes from mods-info's servers.json, as the mods find it, else launcher.json's `rooms.server`.
//! The JSON must stay field for field what the C++ launcher printed (tests/parity/rooms.mjs checks it).

use std::collections::{BTreeMap, HashSet};
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::sync::Mutex;
use std::time::Duration;

use serde_json::{Map, Value, json};

use super::config::Config;
use super::http;
use super::json;
use super::library_json::{load_launcher_config, open_library};

#[derive(Clone, Debug, Default, PartialEq)]
pub struct RoomInfo {
    pub code: String,
    pub name: String,
    pub players: i32,
    pub max: i32,
    // From RELAY|2|ROOMS (rooms the launcher's lobby hosts); empty / -1 on an older server.
    pub locked: bool,
    pub state: String,   // "lobby" | "ingame" | "" (unknown)
    pub region: String,  // the host's country, "NL"
    pub host_ping: i32,  // the host's round trip to the server, ms
    pub version: String, // "1.2#0123456789abcdef"
}

impl RoomInfo {
    pub fn in_game(&self) -> bool {
        self.state == "ingame"
    }
}

/// UTF-8 decoded by Windows (MultiByteToWideChar), as the C++ launcher widens what the server sends: a bad byte
/// becomes U+FFFD exactly where Windows puts one.
fn widen(bytes: &[u8]) -> String {
    use windows_sys::Win32::Globalization::{CP_UTF8, MultiByteToWideChar};
    if bytes.is_empty() {
        return String::new();
    }
    let len = bytes.len() as i32;
    let n = unsafe { MultiByteToWideChar(CP_UTF8, 0, bytes.as_ptr(), len, std::ptr::null_mut(), 0) };
    let mut w = vec![0u16; n.max(0) as usize];
    unsafe { MultiByteToWideChar(CP_UTF8, 0, bytes.as_ptr(), len, w.as_mut_ptr(), n) };
    String::from_utf16_lossy(&w)
}

/// `atoi` on the bytes of a field.
fn atoi(bytes: &[u8]) -> i32 {
    super::text::atoi(&String::from_utf8_lossy(bytes))
}

fn find(hay: &[u8], needle: &[u8]) -> bool {
    hay.windows(needle.len()).any(|w| w == needle)
}

/// A LIST or ROOMS reply; None for anything else. Room names may contain commas (they're the last field), a row
/// with too few fields or no code is skipped.
pub fn parse_room_list(reply: &[u8]) -> Option<Vec<RoomInfo>> {
    let v2 = reply.starts_with(b"ROOMS|");
    if !v2 && !reply.starts_with(b"LIST|") {
        return None;
    }
    let body = &reply[if v2 { 6 } else { 5 }..];
    let fields = if v2 { 8 } else { 3 }; // before the name
    let mut out = Vec::new();
    // A trailing ';' ends the list without an empty row after it, as the C++ loop stops at the end of the text.
    let mut rows: Vec<&[u8]> = body.split(|&b| b == b';').collect();
    if body.is_empty() || body.ends_with(b";") {
        rows.pop();
    }
    for row in rows {
        let mut f: Vec<&[u8]> = Vec::new();
        let mut at = 0;
        while f.len() < fields {
            let Some(comma) = row[at..].iter().position(|&b| b == b',') else { break };
            f.push(&row[at..at + comma]);
            at += comma + 1;
        }
        if f.len() < fields {
            continue;
        }
        let mut r = RoomInfo { code: widen(f[0]), players: atoi(f[1]), max: atoi(f[2]), host_ping: -1, ..Default::default() };
        if v2 {
            r.locked = f[3] == b"1";
            r.state = widen(f[4]);
            r.region = widen(f[5]);
            r.host_ping = if f[6].is_empty() { -1 } else { atoi(f[6]) };
            r.version = widen(f[7]);
        }
        r.name = widen(&row[at..]);
        if !r.code.is_empty() {
            out.push(r);
        }
    }
    Some(out)
}

/// The first IPv4 address `getaddrinfo` gives, as the C++ side asks for it: a port may also be a service name
/// ("127.0.0.1:http"), which Rust's own lookup refuses. None when nothing is found; Err when Winsock won't start.
fn resolve_v4(host: &str, port: &str) -> Result<Option<SocketAddr>, ()> {
    use windows_sys::Win32::Networking::WinSock::{ADDRINFOA, AF_INET, SOCK_STREAM, SOCKADDR_IN, WSACleanup, WSADATA, WSAStartup, freeaddrinfo, getaddrinfo};
    // A C string ends at the first NUL, as c_str() did.
    let c = |s: &str| std::ffi::CString::new(s.split('\0').next().unwrap_or("")).unwrap_or_default();
    let (h, p) = (c(host), c(port));
    let mut wsa: WSADATA = unsafe { std::mem::zeroed() };
    if unsafe { WSAStartup(0x0202, &mut wsa) } != 0 {
        return Err(());
    }
    let hints = ADDRINFOA { ai_family: AF_INET as i32, ai_socktype: SOCK_STREAM, ..Default::default() };
    let mut res: *mut ADDRINFOA = std::ptr::null_mut();
    let mut found = None;
    if unsafe { getaddrinfo(h.as_ptr().cast(), p.as_ptr().cast(), &hints, &mut res) } == 0 && !res.is_null() {
        let ai = unsafe { &*res };
        if !ai.ai_addr.is_null() && ai.ai_addrlen >= std::mem::size_of::<SOCKADDR_IN>() {
            let sin = unsafe { &*(ai.ai_addr as *const SOCKADDR_IN) };
            let ip = std::net::Ipv4Addr::from(u32::from_be(unsafe { sin.sin_addr.S_un.S_addr }));
            found = Some(SocketAddr::from((ip, u16::from_be(sin.sin_port))));
        }
        unsafe { freeaddrinfo(res) };
    }
    unsafe { WSACleanup() };
    Ok(found)
}

/// One request/answer exchange on its own connection: the first NUL-terminated answer. An error when the server
/// can't be reached or says nothing. `server` is "host:port".
pub fn ask_server(server: &str, request: &str, timeout_ms: u64) -> Result<Vec<u8>, String> {
    let Some(colon) = server.rfind(':') else { return Err(format!("Server address needs a port: {server}")) };
    let Ok(addr) = resolve_v4(&server[..colon], &server[colon + 1..]) else { return Err("Networking is not available".into()) };
    let Some(addr) = addr else { return Err(format!("Cannot find the server {server}")) };
    let timeout = Duration::from_millis(timeout_ms);
    let Ok(mut s) = TcpStream::connect_timeout(&addr, timeout) else { return Err(format!("Cannot reach {server}")) };
    let _ = s.set_read_timeout(Some(timeout));
    let _ = s.set_write_timeout(Some(timeout));
    let mut req = request.as_bytes().to_vec();
    req.push(0);
    let _ = s.write_all(&req);
    let mut reply = Vec::new();
    let mut buf = [0u8; 4096];
    // The C++ side waits up to the timeout for each piece, not for the whole answer, and keeps what came before a
    // stall: a server that goes quiet halfway still "answered".
    loop {
        let n = match s.read(&mut buf) {
            Ok(n) if n > 0 => n,
            _ => break,
        };
        reply.extend_from_slice(&buf[..n]);
        if let Some(nul) = reply.iter().position(|&b| b == 0) {
            reply.truncate(nul);
            break;
        }
        if reply.len() > 256 * 1024 {
            break;
        }
    }
    if reply.is_empty() { Err(format!("No answer from {server}")) } else { Ok(reply) }
}

/// Servers that answered RELAY|2 with "unsupported": asked with RELAY|1 from then on, for as long as the app runs
/// (as the C++ window kept them).
static OLD_SERVERS: Mutex<Option<HashSet<String>>> = Mutex::new(None);

fn older_server_reply(reply: &[u8]) -> bool {
    reply.starts_with(b"E|") && (find(reply, b"RELAY|2") || find(reply, b"handshake version") || find(reply, b"Bad handshake"))
}

#[derive(Debug, PartialEq)]
pub struct ListError {
    pub message: String,
    /// The server answered but doesn't run this game yet ("E|Unknown game ..."): its mods-info file isn't merged.
    pub rejected: bool,
}

/// One game's rooms on `server`.
pub fn list_rooms(server: &str, game_id: &str, timeout_ms: u64) -> Result<Vec<RoomInfo>, ListError> {
    let fail = |message: String| ListError { message, rejected: false };
    let old = OLD_SERVERS.lock().unwrap().as_ref().is_some_and(|s| s.contains(server));
    let ask = if old { "RELAY|1|LIST|" } else { "RELAY|2|ROOMS|" };
    let mut reply = ask_server(server, &format!("{ask}{game_id}"), timeout_ms).map_err(fail)?;
    if !old && older_server_reply(&reply) {
        OLD_SERVERS.lock().unwrap().get_or_insert_with(HashSet::new).insert(server.to_string());
        reply = ask_server(server, &format!("RELAY|1|LIST|{game_id}"), timeout_ms).map_err(fail)?;
    }
    if let Some(rooms) = parse_room_list(&reply) {
        return Ok(rooms);
    }
    if reply.starts_with(b"E|") {
        return Err(ListError { message: widen(&reply[2..]), rejected: true });
    }
    Err(fail(format!("Unexpected answer from {server}")))
}

/// Can the internet reach `port` on this PC? The server connects back to the address it sees us from. "open",
/// "closed", "other" (something else answers there); an error when the server can't test it (an older server,
/// unreachable). For the lobby (part 5).
pub fn probe_port(server: &str, game_id: &str, port: i32) -> Result<String, String> {
    let reply = ask_server(server, &format!("RELAY|2|PROBE|{game_id}|{port}"), 12000)?;
    let Some(rest) = reply.strip_prefix(b"PROBE|") else {
        return Err(if reply.starts_with(b"E|") { widen(&reply[2..]) } else { "The server can't test ports".into() });
    };
    let bar = rest.iter().position(|&b| b == b'|');
    let result = widen(&rest[..bar.unwrap_or(rest.len())]);
    if result == "error" {
        return Err(bar.map(|b| widen(&rest[b + 1..])).unwrap_or_default());
    }
    Ok(result)
}

/// The first "online" server in mods-info's servers.json at `servers_url`, else `fallback`; "host:port" or empty.
/// The second value is why servers.json couldn't be read (empty when it could, even if it listed nothing usable).
pub fn resolve_room_server(servers_url: &str, fallback: &str) -> (String, String) {
    if servers_url.is_empty() {
        return (fallback.to_string(), String::new());
    }
    match http::get_text(servers_url) {
        Ok(text) => {
            let root = json::parse(&text).ok();
            let list = root.as_ref().filter(|r| r.is_object()).and_then(|r| r.find("servers"));
            if let Some(list) = list.filter(|l| l.is_array()) {
                for sv in list.items() {
                    let addr = sv.find("address").and_then(|a| a.as_str());
                    let status = sv.find("status").and_then(|s| s.as_str());
                    if let Some(addr) = addr {
                        if status.is_none_or(|s| s == "online") {
                            return (addr.to_string(), String::new());
                        }
                    }
                }
            }
            (fallback.to_string(), String::new())
        }
        Err(e) => (fallback.to_string(), e),
    }
}

/// What the window reads for one game: the server asked, whether it runs the game, why the list is missing, rooms.
pub fn rooms_json(server: &str, on_server: bool, rooms: &[RoomInfo], error: &str) -> Value {
    let list: Vec<Value> = rooms
        .iter()
        .map(|r| {
            json!({
                "code": r.code, "name": r.name, "players": r.players, "max": r.max, "locked": r.locked,
                "state": r.state, "region": r.region, "hostPing": r.host_ping,
            })
        })
        .collect();
    json!({ "server": server, "onServer": on_server, "error": error, "rooms": list })
}

const TIMEOUT_MS: u64 = 4000;

/// `--rooms <id> --json`: one game's rooms, and whether they were listed (the C++ exit code was 1 when not, which
/// the window ignored: it read the JSON either way). An error, as the C++ launcher's exit 3, for a launcher.json
/// with problems or a game that isn't online.
pub fn game_rooms_from(cfg: &Config, game: &str) -> Result<(Value, bool), String> {
    let Some(g) = cfg.find_game(game).filter(|g| !g.online_id.is_empty()) else {
        return Err(format!("No online game with id \"{game}\""));
    };
    let (server, err) = resolve_room_server(&cfg.rooms.servers, &cfg.rooms.server);
    let listed = if server.is_empty() { Err(ListError { message: err, rejected: false }) } else { list_rooms(&server, &g.online_id, TIMEOUT_MS) };
    Ok(match listed {
        Ok(rooms) => (rooms_json(&server, true, &rooms, ""), true),
        Err(e) => (rooms_json(&server, !e.rejected, &[], &e.message), e.rejected),
    })
}

/// `--rooms all --json`: every online game's rooms in one call, as the window keeps its cards' room counts. Each
/// distinct online id is asked once.
pub fn all_rooms_from(cfg: &Config) -> Value {
    let (server, err) = resolve_room_server(&cfg.rooms.servers, &cfg.rooms.server);
    let mut asked: BTreeMap<&str, Value> = BTreeMap::new();
    let mut games = Map::new();
    for g in &cfg.games {
        if g.online_id.is_empty() || g.coming_soon {
            continue;
        }
        let answer = asked.entry(&g.online_id).or_insert_with(|| {
            let listed = if server.is_empty() { Err(ListError { message: String::new(), rejected: false }) } else { list_rooms(&server, &g.online_id, TIMEOUT_MS) };
            match listed {
                Ok(rooms) => rooms_json(&server, true, &rooms, ""),
                // An empty reason (no server, or a bare "E|") falls back to why servers.json couldn't be read.
                Err(e) => rooms_json(&server, !e.rejected, &[], if e.message.is_empty() { &err } else { &e.message }),
            }
        });
        games.insert(g.id.clone(), answer.clone());
    }
    json!({ "server": server, "games": games })
}

/// The C++ launcher's set-up before `--rooms` (launcher.json, the data, games and players folders), then one game.
pub fn game_rooms(game: &str) -> Result<(Value, bool), String> {
    let cfg = load_launcher_config()?;
    open_library(&cfg);
    game_rooms_from(&cfg, game)
}

pub fn all_rooms() -> Result<Value, String> {
    let cfg = load_launcher_config()?;
    open_library(&cfg);
    Ok(all_rooms_from(&cfg))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    #[test]
    fn room_list_parsing() {
        let rooms = parse_room_list(b"LIST|VK7Q2,2,4,Liam's room;M2NN5,4,4,Friday, late;bad;QX3TB,1,4,").unwrap();
        assert_eq!(rooms.len(), 3); // the malformed "bad" row is skipped
        assert!(rooms[0].code == "VK7Q2" && rooms[0].players == 2 && rooms[0].max == 4 && rooms[0].name == "Liam's room");
        assert_eq!(rooms[1].name, "Friday, late"); // the name is the last field and may contain commas
        assert!(rooms[2].code == "QX3TB" && rooms[2].name.is_empty());
        assert!(parse_room_list(b"LIST|").unwrap().is_empty()); // no rooms open
        assert!(parse_room_list(b"E|unknown game").is_none());
    }

    #[test]
    fn room_list_v2_carries_lobby_details() {
        let rooms = parse_room_list(b"ROOMS|SKT75,2,3,1,ingame,NL,42,1.0#abc,Friday, late;OLD11,1,4,0,,,-1,v1,Old room").unwrap();
        assert_eq!(rooms.len(), 2);
        assert!(rooms[0].code == "SKT75" && rooms[0].players == 2 && rooms[0].max == 3 && rooms[0].locked);
        assert!(rooms[0].in_game() && rooms[0].region == "NL" && rooms[0].host_ping == 42 && rooms[0].version == "1.0#abc");
        assert_eq!(rooms[0].name, "Friday, late");
        assert!(!rooms[1].locked && !rooms[1].in_game() && rooms[1].host_ping == -1 && rooms[1].name == "Old room");
    }

    #[test]
    fn room_list_edges() {
        // An empty row between two, a row without a code, an empty ping, a trailing ';'.
        let rooms = parse_room_list(b"ROOMS|A1,1,2,0,lobby,EU,,v,One;;,1,2,0,,,,,No code;B2,x,3,1,,,7,,Two;").unwrap();
        assert_eq!(rooms.len(), 2);
        assert_eq!(rooms[0].host_ping, -1);
        assert!(rooms[1].players == 0 && rooms[1].locked && rooms[1].host_ping == 7);
        assert!(parse_room_list(b"LIST|;").unwrap().is_empty());
        assert!(parse_room_list(b"list|A,1,2,x").is_none());
    }

    /// A server on loopback that answers each connection with the next reply (and its NUL), recording what it was
    /// asked.
    fn serve(replies: Vec<&'static str>) -> (String, std::thread::JoinHandle<Vec<String>>) {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = format!("127.0.0.1:{}", l.local_addr().unwrap().port());
        let t = std::thread::spawn(move || {
            let mut asked = Vec::new();
            for reply in replies {
                let (mut c, _) = l.accept().unwrap();
                let mut buf = [0u8; 256];
                let n = c.read(&mut buf).unwrap();
                asked.push(String::from_utf8_lossy(&buf[..n]).trim_end_matches('\0').to_string());
                let mut r = reply.as_bytes().to_vec();
                r.push(0);
                c.write_all(&r).unwrap();
            }
            asked
        });
        (addr, t)
    }

    #[test]
    fn rooms_a_server_without_the_game_is_told_apart() {
        // The relay answers "E|Unknown game ..." until the mod's mods-info file is merged.
        let (addr, t) = serve(vec!["E|Unknown game 'tw' on this server", "LIST|ABCDE,1,4,Room"]);
        let e = list_rooms(&addr, "tw", 3000).unwrap_err();
        assert!(e.rejected && e.message.contains("Unknown game"));
        let rooms = list_rooms(&addr, "gm2", 3000).unwrap();
        assert_eq!(rooms.len(), 1);
        t.join().unwrap();
    }

    #[test]
    fn an_older_server_is_asked_with_relay_1_from_then_on() {
        let (addr, t) = serve(vec!["E|Bad handshake: RELAY|2", "LIST|ABCDE,1,4,Room", "LIST|"]);
        assert_eq!(list_rooms(&addr, "gm2", 3000).unwrap().len(), 1);
        assert!(list_rooms(&addr, "gm2", 3000).unwrap().is_empty());
        assert_eq!(t.join().unwrap(), ["RELAY|2|ROOMS|gm2", "RELAY|1|LIST|gm2", "RELAY|1|LIST|gm2"]);
    }

    #[test]
    fn ask_server_errors() {
        assert_eq!(ask_server("127.0.0.1", "x", 500).unwrap_err(), "Server address needs a port: 127.0.0.1");
        assert_eq!(ask_server("127.0.0.1:notaport", "x", 500).unwrap_err(), "Cannot find the server 127.0.0.1:notaport");
        // A port nobody listens on.
        let port = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port();
        let addr = format!("127.0.0.1:{port}");
        assert_eq!(ask_server(&addr, "x", 500).unwrap_err(), format!("Cannot reach {addr}"));
        let (addr, t) = serve(vec![""]);
        assert_eq!(ask_server(&addr, "x", 3000).unwrap_err(), format!("No answer from {addr}"));
        t.join().unwrap();
        let (addr, t) = serve(vec!["garbage"]);
        assert_eq!(list_rooms(&addr, "g", 3000).unwrap_err(), ListError { message: format!("Unexpected answer from {addr}"), rejected: false });
        t.join().unwrap();
    }

    #[test]
    fn probe_port_answers() {
        let (addr, t) = serve(vec!["PROBE|open|1.2.3.4", "PROBE|error|no route", "E|old", "LIST|"]);
        assert_eq!(probe_port(&addr, "g", 7790).unwrap(), "open");
        assert_eq!(probe_port(&addr, "g", 7790).unwrap_err(), "no route");
        assert_eq!(probe_port(&addr, "g", 7790).unwrap_err(), "old");
        assert_eq!(probe_port(&addr, "g", 7790).unwrap_err(), "The server can't test ports");
        assert_eq!(t.join().unwrap()[0], "RELAY|2|PROBE|g|7790");
    }

    #[test]
    fn bad_utf8_becomes_replacement_characters() {
        let rooms = parse_room_list(b"LIST|AB,1,2,\xff\xfeok\xe2\x82").unwrap();
        assert!(rooms[0].name.contains('\u{FFFD}') && rooms[0].name.contains("ok"));
    }
}
