//! The C++ tests for part 4 (old/tests/test_core.cpp: the rate-limit wording; test_shipped_mods.cpp: a player the
//! launcher installed is updated only when a newer release exists; the install and catalog checks of e2e.ps1), ported
//! against a loopback stand-in for GitHub and the download host and real temporary folders. Every case of the
//! commands is compared with the C++ launcher by tests/parity/downloads.mjs.

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::{Arc, Mutex};

use super::catalog::{outdated_game_files, parse_catalog};
use super::config::{PlayerSpec, load_config, load_config_file};
use super::http::{is_rate_limit_message, rate_limit_message};
use super::install::*;
use super::paths;
use super::tests::TempDir;
use super::text::{format_bytes, glob_match};
use super::vars::Vars;

/// A stand-in web server: each path answers a fixed status, headers and body (404 otherwise); it counts the requests
/// for each path. `drop` sends half the body after a full Content-Length and hangs up.
#[derive(Clone, Default)]
struct Route {
    status: u16,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
    drop: bool,
}

#[derive(Clone)]
struct Server {
    port: u16,
    routes: Arc<Mutex<HashMap<String, Route>>>,
    hits: Arc<Mutex<Vec<String>>>,
}

impl Server {
    fn new() -> Server {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        let s = Server { port, routes: Arc::default(), hits: Arc::default() };
        let t = s.clone();
        std::thread::spawn(move || {
            for c in l.incoming() {
                let Ok(mut c) = c else { continue };
                let mut buf = vec![0u8; 8192];
                let mut req = Vec::new();
                while !req.windows(4).any(|w| w == b"\r\n\r\n") {
                    match c.read(&mut buf) {
                        Ok(0) | Err(_) => break,
                        Ok(n) => req.extend_from_slice(&buf[..n]),
                    }
                }
                let text = String::from_utf8_lossy(&req);
                let path = text.split(' ').nth(1).unwrap_or("").to_string();
                t.hits.lock().unwrap().push(path.clone());
                let r = t.routes.lock().unwrap().get(&path).cloned().unwrap_or(Route { status: 404, ..Default::default() });
                let mut head = format!("HTTP/1.1 {} X\r\nContent-Length: {}\r\nConnection: close\r\n", r.status, r.body.len());
                for (k, v) in &r.headers {
                    head.push_str(&format!("{k}: {v}\r\n"));
                }
                head.push_str("\r\n");
                let _ = c.write_all(head.as_bytes());
                let body = if r.drop { &r.body[..r.body.len() / 2] } else { &r.body[..] };
                let _ = c.write_all(body);
                let _ = c.flush();
            }
        });
        s
    }
    fn url(&self, path: &str) -> String {
        format!("http://127.0.0.1:{}{path}", self.port)
    }
    fn set(&self, path: &str, body: &[u8]) {
        self.routes.lock().unwrap().insert(path.into(), Route { status: 200, body: body.to_vec(), ..Default::default() });
    }
    fn set_route(&self, path: &str, r: Route) {
        self.routes.lock().unwrap().insert(path.into(), r);
    }
    fn hits(&self, path: &str) -> usize {
        self.hits.lock().unwrap().iter().filter(|p| *p == path).count()
    }
}

fn sha(data: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    Sha256::digest(data).iter().map(|b| format!("{b:02x}")).collect()
}

fn crc32(data: &[u8]) -> u32 {
    let mut c = 0xffff_ffffu32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xedb8_8320 ^ (c >> 1) } else { c >> 1 };
        }
    }
    !c
}

/// A stored (uncompressed) zip, as tar.exe reads it.
fn zip(entries: &[(&str, &[u8])]) -> Vec<u8> {
    let (mut out, mut central) = (Vec::new(), Vec::new());
    for (name, data) in entries {
        let crc = crc32(data);
        let offset = out.len() as u32;
        let le16 = |v: &mut Vec<u8>, x: u16| v.extend_from_slice(&x.to_le_bytes());
        let le32 = |v: &mut Vec<u8>, x: u32| v.extend_from_slice(&x.to_le_bytes());
        le32(&mut out, 0x0403_4b50);
        for x in [20u16, 0x0800, 0, 0, 0] {
            le16(&mut out, x);
        }
        le32(&mut out, crc);
        le32(&mut out, data.len() as u32);
        le32(&mut out, data.len() as u32);
        le16(&mut out, name.len() as u16);
        le16(&mut out, 0);
        out.extend_from_slice(name.as_bytes());
        out.extend_from_slice(data);
        le32(&mut central, 0x0201_4b50);
        for x in [20u16, 20, 0x0800, 0, 0, 0] {
            le16(&mut central, x);
        }
        le32(&mut central, crc);
        le32(&mut central, data.len() as u32);
        le32(&mut central, data.len() as u32);
        for x in [name.len() as u16, 0, 0, 0, 0] {
            le16(&mut central, x);
        }
        le32(&mut central, 0);
        le32(&mut central, offset);
        central.extend_from_slice(name.as_bytes());
    }
    let (cd_offset, cd_len, n) = (out.len() as u32, central.len() as u32, entries.len() as u16);
    out.extend_from_slice(&central);
    out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
    out.extend_from_slice(&[0, 0, 0, 0]);
    out.extend_from_slice(&n.to_le_bytes());
    out.extend_from_slice(&n.to_le_bytes());
    out.extend_from_slice(&cd_len.to_le_bytes());
    out.extend_from_slice(&cd_offset.to_le_bytes());
    out.extend_from_slice(&[0, 0]);
    out
}

fn player(dir: &TempDir, install: &str) -> (PlayerSpec, Vars) {
    let text = format!(
        r#"{{ "paths": {{"games": "${{LAUNCHER_DIR}}\\games", "players": "${{LAUNCHER_DIR}}\\players", "data": "${{LAUNCHER_DIR}}\\data", "flashpoint": []}},
             "players": [{{ "id": "rt", "name": "Release Player", "paths": ["rt.exe"], "install": {install} }}],
             "games": [{{ "id": "g", "name": "G", "actions": [{{ "id": "p", "swf": "a.swf" }}] }}] }}"#
    );
    let r = load_config(text.as_bytes(), dir.path());
    assert!(r.ok(), "{:?}", r.issues.iter().map(|i| &i.message).collect::<Vec<_>>());
    let mut vars = Vars::default();
    vars.set("PLAYERS_DIR", &dir.join("players"));
    (r.config.players[0].clone(), vars)
}

fn read(path: &str) -> String {
    String::from_utf8_lossy(&std::fs::read(path).unwrap_or_default()).into_owned()
}

fn release(tag: &str, name: &str, url: &str, digest: &str) -> Vec<u8> {
    format!(r#"{{"tag_name":"{tag}","draft":false,"assets":[{{"name":"{name}","browser_download_url":"{url}","digest":"sha256:{digest}"}}]}}"#).into_bytes()
}

#[test]
fn rate_limit_message_says_how_long_is_left() {
    assert!(rate_limit_message(20).contains("about a minute"));
    assert!(rate_limit_message(61).contains("2 minutes"));
    assert!(rate_limit_message(23 * 60).contains("23 minutes"));
    assert!(rate_limit_message(3600).contains("1 h 0 min"));
    assert!(rate_limit_message(-5).contains("about a minute"));
    assert!(is_rate_limit_message(&rate_limit_message(90)) && !is_rate_limit_message("api.github.com answered HTTP 403"));
}

#[test]
fn asset_patterns_and_sizes_read_as_the_cpp_helpers() {
    assert!(glob_match("*windows-x86_64.zip", "ruffle-nightly-2024_windows-X86_64.zip"));
    assert!(glob_match("Mod-*.zip", "mod-1.0.0.zip") && !glob_match("Mod-*.zip", "Mod-1.0.0.7z"));
    assert!(!glob_match("*.zip", "dir/a.zip") && glob_match("**/a.zip", "a.zip") && glob_match("**/a.zip", "x/y/a.zip"));
    assert!(glob_match("a?c", "abc") && !glob_match("a?c", "a/c"));
    assert_eq!(format_bytes(1), "1 byte");
    assert_eq!(format_bytes(1000), "1000 bytes");
    assert_eq!(format_bytes(600_214), "586 KB");
    assert_eq!(format_bytes(3 * 1024 * 1024 + 400_000), "3.4 MB");
    assert!(paths::is_dangerous_root("C:\\") && paths::is_dangerous_root(&paths::known_folder("Documents")));
    assert!(paths::is_dangerous_root(&paths::join(&paths::known_folder("Windows"), "System32")));
    assert!(!paths::is_dangerous_root(&paths::join(&paths::known_folder("Documents"), "MultiplayerEverythingLauncher\\players\\x")));
}

#[test]
fn a_player_the_launcher_installed_is_updated_only_when_a_newer_release_exists() {
    let dir = TempDir::new("player-update");
    let api = Server::new();
    let (p, vars) = player(&dir, &format!(r#"{{ "github": "o/r", "asset": "Mod-*.zip", "api": "{}", "dir": "${{PLAYERS_DIR}}\\rt", "exe": "rt.exe" }}"#, api.url("")));
    let exe = dir.join("players\\rt\\rt.exe");
    let marker = dir.join("players\\rt\\.version");
    let latest = "/repos/o/r/releases/latest";
    // The download address refuses connections, so an install attempt fails here.
    let serve = |v: &str| api.set(latest, &release(&format!("v{v}"), &format!("Mod-{v}.zip"), "http://127.0.0.1:1/Mod.zip", &"a".repeat(64)));

    // Not installed by the launcher (no file in its folder): left alone, GitHub isn't even asked.
    let none = update_installed_player(&p, &vars, None, true, &mut |_| {});
    assert!(!none.checked && !none.updated && none.error.is_empty() && api.hits(latest) == 0);

    // The newest release is the one installed: nothing to do.
    std::fs::create_dir_all(dir.join("players\\rt")).unwrap();
    std::fs::write(&exe, "old").unwrap();
    std::fs::write(&marker, "v1.0.1").unwrap();
    serve("1.0.1");
    let same = update_installed_player(&p, &vars, None, true, &mut |_| {});
    assert!(same.checked && !same.available && !same.updated && same.error.is_empty() && same.installed == "v1.0.1");
    assert_eq!(read(&exe), "old");

    // Only asked (the window asks the player first): the newer release is reported, nothing is downloaded.
    serve("1.0.2");
    let asked = update_installed_player(&p, &vars, None, false, &mut |_| {});
    assert!(asked.checked && asked.available && !asked.updated && asked.error.is_empty() && asked.version == "v1.0.2");
    assert_eq!(read(&exe), "old");
    assert_eq!(read(&marker), "v1.0.1");

    // A newer release exists but can't be downloaded here: the installed copy stays exactly as it was.
    let newer = update_installed_player(&p, &vars, None, true, &mut |_| {});
    assert!(newer.checked && !newer.updated && !newer.error.is_empty());
    assert_eq!(read(&exe), "old");
    assert_eq!(read(&marker), "v1.0.1");
    assert!(!paths::exists(&dir.join("players\\rt.installing")) && !paths::exists(&dir.join("players\\rt.old")));

    // A copy from before the version was recorded counts as older, so even the same release is installed over it.
    serve("1.0.1");
    std::fs::remove_file(&marker).unwrap();
    let unmarked = update_installed_player(&p, &vars, None, true, &mut |_| {});
    assert!(unmarked.checked && !unmarked.updated && !unmarked.error.is_empty()); // it tried (the download fails here)
    assert_eq!(read(&exe), "old");
}

#[test]
fn a_newer_release_is_installed_with_its_tag_and_progress() {
    let dir = TempDir::new("player-release");
    let api = Server::new();
    let (p, vars) = player(&dir, &format!(r#"{{ "github": "o/r", "asset": "rt-*.zip", "api": "{}", "dir": "${{PLAYERS_DIR}}\\rt", "exe": "rt.exe" }}"#, api.url("")));
    let pkg = zip(&[("rt.exe", b"new build"), ("lib/data.bin", &[7u8; 300_000])]);
    api.set("/repos/o/r/releases/latest", &release("v2.0.0", "rt-2.0.0.zip", &api.url("/dl/rt.zip"), &sha(&pkg).to_uppercase()));
    api.set("/dl/rt.zip", &pkg);
    std::fs::create_dir_all(dir.join("players\\rt")).unwrap();
    std::fs::write(dir.join("players\\rt\\rt.exe"), "old").unwrap();
    std::fs::write(dir.join("players\\rt\\stale.txt"), "from the old release").unwrap();
    std::fs::write(dir.join("players\\rt\\.version"), "v1.9").unwrap();
    let mut seen = Vec::new();
    let u = update_installed_player(&p, &vars, None, true, &mut |s| {
        if s.stage == Stage::Downloading {
            seen.push((s.done, s.total));
        }
    });
    assert!(u.checked && u.available && u.updated && u.error.is_empty(), "{u:?}");
    assert_eq!(read(&dir.join("players\\rt\\rt.exe")), "new build");
    assert_eq!(read(&dir.join("players\\rt\\.version")), "v2.0.0");
    assert!(!paths::exists(&dir.join("players\\rt\\stale.txt")), "the folder is replaced as a whole");
    assert_eq!(seen.last().copied(), Some((pkg.len() as u64, pkg.len() as u64)));
    assert!(!paths::exists(&dir.join("players\\rt.installing")) && !paths::exists(&dir.join("players\\rt.old")));
}

#[test]
fn install_player_checks_the_download_and_keeps_the_working_copy() {
    let dir = TempDir::new("player-install");
    let web = Server::new();
    let pkg = zip(&[("fakeplay.exe", b"fake player")]);
    web.set("/fake.zip", &pkg);
    let spec = |hash: &str| format!(r#"{{ "url": "{}", "sha256": "{hash}", "dir": "${{PLAYERS_DIR}}\\fake", "exe": "fakeplay.exe" }}"#, web.url("/fake.zip"));
    let (p, vars) = player(&dir, &spec(&sha(&pkg)));
    let exe = dir.join("players\\fake\\fakeplay.exe");

    // --install-player downloads, checks and unpacks; no leftovers.
    let r = install_player(&p, &vars, &mut |_| {}, None);
    assert!(r.ok && r.exe == exe && read(&exe) == "fake player", "{r:?}");
    assert!(!paths::exists(&dir.join("players\\fake.installing")) && !paths::exists(&dir.join("players\\fake.old")));
    assert!(!paths::exists(&dir.join("players\\fake\\.version")), "a fixed URL has no release tag");

    // A download with the wrong checksum is refused, and the working player stays.
    let (bad, _) = player(&dir, &spec(&"0".repeat(64)));
    let r = install_player(&bad, &vars, &mut |_| {}, None);
    assert!(!r.ok && r.error.contains("checksum"), "{r:?}");
    assert_eq!(read(&exe), "fake player");
    assert!(!paths::exists(&dir.join("players\\fake.installing")));

    // A connection that drops halfway: the same.
    web.set_route("/fake.zip", Route { status: 200, body: pkg.clone(), drop: true, ..Default::default() });
    let r = install_player(&p, &vars, &mut |_| {}, None);
    assert!(!r.ok);
    assert_eq!(read(&exe), "fake player");
    assert!(!paths::exists(&dir.join("players\\fake.installing")));

    // An archive without the player, and one with an entry that climbs out of the folder.
    let empty = zip(&[("other.exe", b"x")]);
    web.set("/fake.zip", &empty);
    let (p2, _) = player(&dir, &spec(&sha(&empty)));
    let r = install_player(&p2, &vars, &mut |_| {}, None);
    assert_eq!(r.error, "The download doesn't contain fakeplay.exe");
    let evil = zip(&[("fakeplay.exe", b"evil"), ("../escaped.txt", b"outside")]);
    web.set("/fake.zip", &evil);
    let (p3, _) = player(&dir, &spec(&sha(&evil)));
    let r = install_player(&p3, &vars, &mut |_| {}, None);
    assert!(!r.ok && !paths::exists(&dir.join("players\\escaped.txt")) && !paths::exists(&dir.join("escaped.txt")));
    assert_eq!(read(&exe), "fake player");

    // A drive root is never an install folder.
    let (root, _) = player(&dir, &format!(r#"{{ "url": "{}", "dir": "C:\\", "exe": "x.exe" }}"#, web.url("/fake.zip")));
    assert_eq!(install_player(&root, &vars, &mut |_| {}, None).error, "Unsafe install folder: C:\\");

    // GitHub's rate limit is worded for the player.
    let (gh, _) = player(&dir, &format!(r#"{{ "github": "o/r", "asset": "*.zip", "api": "{}", "dir": "${{PLAYERS_DIR}}\\gh" }}"#, web.url("")));
    web.set_route("/repos/o/r/releases/latest", Route { status: 429, headers: vec![("Retry-After".into(), "120".into())], ..Default::default() });
    let r = install_player(&gh, &vars, &mut |_| {}, None);
    assert!(is_rate_limit_message(&r.error) && r.error.contains("2 minutes"), "{}", r.error);
}

#[test]
fn game_files_come_from_the_catalog_checked_and_renamed_into_place() {
    let dir = TempDir::new("catalog-download");
    let web = Server::new();
    let file = b"pretend game file";
    web.set("/games/demo2/dl.swf", file);
    let catalog = |hash: &str| format!(r#"{{ "schemaVersion": 1, "files": [ {{ "name": "dl.swf", "game": "demo2", "path": "games/demo2/dl.swf", "size": {}, "sha256": "{hash}" }} ] }}"#, file.len());
    web.set("/catalog.json", catalog(&sha(file)).as_bytes());
    let games = dir.join("dlgames");

    let (files, raw) = fetch_catalog(&web.url("/catalog.json"), None).unwrap();
    assert_eq!(raw, catalog(&sha(file)).as_bytes());
    assert_eq!(files[0].url, web.url("/games/demo2/dl.swf"));
    let r = download_game_files(&files, &games, &mut |_| {}, None);
    assert!(r.ok, "{r:?}");
    assert_eq!(paths::sha256_file(&paths::join(&games, "dl.swf")), sha(file));

    // Already there with the right checksum: kept, not downloaded again.
    let before = web.hits("/games/demo2/dl.swf");
    assert!(download_game_files(&files, &games, &mut |_| {}, None).ok);
    assert_eq!(web.hits("/games/demo2/dl.swf"), before);
    assert!(outdated_game_files(&files, &games).is_empty());

    // The catalog now names another build: the copy counts as outdated.
    let newer = parse_catalog(catalog(&"1".repeat(64)).as_bytes(), &web.url("/catalog.json")).unwrap();
    assert_eq!(outdated_game_files(&newer, &games).len(), 1);

    // A file with the wrong checksum is not saved, and no .part is left.
    std::fs::remove_file(paths::join(&games, "dl.swf")).unwrap();
    let wrong = parse_catalog(catalog(&"0".repeat(64)).as_bytes(), &web.url("/catalog.json")).unwrap();
    let r = download_game_files(&wrong, &games, &mut |_| {}, None);
    assert!(!r.ok && r.error.starts_with("dl.swf: The download doesn't match"), "{}", r.error);
    assert!(!paths::exists(&paths::join(&games, "dl.swf")) && !paths::exists(&paths::join(&games, "dl.swf.part")));

    // A cancel before the first read stops it the same way.
    let stop = std::sync::atomic::AtomicBool::new(true);
    let r = download_game_files(&files, &games, &mut |_| {}, Some(&stop));
    assert_eq!(r.error, "dl.swf: Cancelled");
    assert!(!paths::exists(&paths::join(&games, "dl.swf.part")));
}

/// The commands read launcher.json from FL_LAUNCHER_CONFIG, which is the whole process's: one test at a time.
static CONFIG_ENV: Mutex<()> = Mutex::new(());

#[test]
fn unknown_ids_exit_3_as_the_cpp_launcher_did() {
    let _one = CONFIG_ENV.lock().unwrap();
    let dir = TempDir::new("commands");
    let web = Server::new();
    web.set("/catalog.json", br#"{ "files": [ { "name": "a.swf", "game": "ga", "path": "a.swf", "sha256": "00" } ] }"#);
    let cfg = dir.join("launcher.json");
    std::fs::write(
        &cfg,
        format!(
            r#"{{ "paths": {{"games": "${{LAUNCHER_DIR}}\\games", "players": "${{LAUNCHER_DIR}}\\players", "data": "${{LAUNCHER_DIR}}\\data", "flashpoint": []}},
                 "downloads": {{ "catalog": "{}" }},
                 "players": [{{ "id": "plain", "name": "Plain", "paths": ["x.exe"] }}],
                 "games": [{{ "id": "ga", "name": "A", "actions": [{{ "id": "p", "swf": "a.swf" }}] }}] }}"#,
            web.url("/catalog.json")
        ),
    )
    .unwrap();
    unsafe { std::env::set_var("FL_LAUNCHER_CONFIG", &cfg) };
    let mut out = Vec::new();
    let (code, msg) = install_player_command("nope", &mut |l| out.push(l.to_string()));
    assert_eq!((code, msg.as_str()), (3, "No installable player with id \"nope\" (it needs an \"install\" entry)"));
    assert_eq!(install_player_command("plain", &mut |_| {}).0, 3);
    let (code, msg) = download_game_command("nosuchgame", &mut |_| {});
    assert_eq!((code, msg.as_str()), (3, "The catalog has no files for \"nosuchgame\""));
    let mut lines = Vec::new();
    let (code, _) = update_players_command(true, true, &mut |l| lines.push(l.to_string()));
    assert_eq!(code, 0);
    assert_eq!(
        lines,
        [r#"{"players":[{"id":"plain","checked":false,"available":false,"updated":false,"installed":"","version":"","error":""}]}"#]
    );
    assert!(out.is_empty());
    assert!(paths::is_dir(&dir.join("games")) && paths::is_dir(&dir.join("players")), "the folders exist from the first command");
    unsafe { std::env::remove_var("FL_LAUNCHER_CONFIG") };
}

#[test]
fn the_websites_games_json_is_fetched_at_most_once_an_hour() {
    use super::site::{SiteCache, load_site_cache, refresh_site_cache, save_site_cache};
    let dir = TempDir::new("site-fetch");
    let cache = dir.join("site.json");
    let s = Server::new();
    let first = br#"{"games":[{"id":"gunmayhem2","name":"Gun Mayhem 2"}]}"#;
    s.set("/games.json", first);
    // No copy yet: it is fetched and kept.
    assert_eq!(refresh_site_cache(&s.url("/games.json"), &cache), Ok(true));
    assert_eq!(load_site_cache(&cache).unwrap().raw.as_bytes(), first);
    // Under an hour old: the website isn't asked again, even when it changed.
    s.set("/games.json", br#"{"games":[{"id":"newgame","name":"New Game"}]}"#);
    assert_eq!(refresh_site_cache(&s.url("/games.json"), &cache), Ok(false));
    assert_eq!(s.hits("/games.json"), 1);
    // An hour old: fetched again, and the change is reported.
    let old = load_site_cache(&cache).unwrap();
    save_site_cache(&cache, &SiteCache { fetched_at: old.fetched_at - 3600, raw: old.raw });
    assert_eq!(refresh_site_cache(&s.url("/games.json"), &cache), Ok(true));
    assert!(load_site_cache(&cache).unwrap().raw.contains("newgame"));
    // The same answer again is not a change.
    let old = load_site_cache(&cache).unwrap();
    save_site_cache(&cache, &SiteCache { fetched_at: old.fetched_at - 3600, raw: old.raw });
    assert_eq!(refresh_site_cache(&s.url("/games.json"), &cache), Ok(false));
    // A broken answer or a dead website keeps the copy it had.
    let old = load_site_cache(&cache).unwrap();
    save_site_cache(&cache, &SiteCache { fetched_at: old.fetched_at - 3600, raw: old.raw.clone() });
    s.set("/games.json", b"<html>maintenance</html>");
    assert_eq!(refresh_site_cache(&s.url("/games.json"), &cache).unwrap_err(), "The website's data isn't in the expected shape");
    assert!(refresh_site_cache(&s.url("/gone.json"), &cache).unwrap_err().contains("404"));
    assert_eq!(load_site_cache(&cache).unwrap().raw, old.raw);
    // Only https (or loopback, for tests).
    assert_eq!(refresh_site_cache("http://example.com/games.json", &dir.join("other.json")).unwrap_err(), "No https address for the website's data");
}

#[test]
fn website_covers_are_pictures_or_nothing() {
    use super::site::download_image;
    let dir = TempDir::new("site-covers");
    let s = Server::new();
    let png = [&[0x89u8, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A][..], &[0u8; 32][..]].concat();
    s.set("/covers/a.png", &png);
    s.set("/covers/page.png", b"<html>not a picture</html>");
    s.set("/covers/big.png", &vec![0u8; 3 * 1024 * 1024 + 10]);
    let dest = dir.join("covers/site-a.png");
    download_image(&s.url("/covers/a.png"), &dest).unwrap();
    assert_eq!(std::fs::read(&dest).unwrap(), png);
    assert_eq!(download_image(&s.url("/covers/page.png"), &dir.join("covers/x.png")).unwrap_err(), "That isn't a PNG, JPEG or WebP picture");
    assert_eq!(download_image(&s.url("/covers/big.png"), &dir.join("covers/y.png")).unwrap_err(), "The picture is too large");
    assert!(!paths::exists(&dir.join("covers/x.png")) && !paths::exists(&dir.join("covers/y.png")));
    assert!(!paths::exists(&dir.join("covers/site-a.png.part")));
    assert_eq!(download_image("http://example.com/a.png", &dir.join("z.png")).unwrap_err(), "Pictures are only fetched over https");
}

// ---- community mods from mods-info (src/launcher/community.rs) ---------------------------------------------------

fn shipped_config() -> super::config::Config {
    let path = format!("{}\\..\\launcher\\launcher.json", env!("CARGO_MANIFEST_DIR"));
    let mut r = load_config_file(&paths::normalize(&path));
    assert!(r.ok());
    super::tests::add_team_fixtures(&mut r.config);
    r.config
}

const SHA: &str = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/// An approved record as the website writes it into mods-info's community/<id>.json.
fn record() -> serde_json::Value {
    serde_json::json!({
        "id": "lab-coop",
        "repo": "someone/lab-coop",
        "tag": "v1.2.0",
        "version": "1.2.0",
        "asset": "LabCoop-1.2.0.zip",
        "url": "https://github.com/someone/lab-coop/releases/download/v1.2.0/LabCoop-1.2.0.zip",
        "sha256": SHA,
        "manifest_sha256": SHA,
        "author": "someone",
        "approved_at": "2026-11-03",
        "approved_by": "JessniX",
        "mod": {
            "id": "lab-coop",
            "game": {
                "name": "Lab Game",
                "executable": "LabGame.exe",
                "steamAppId": "123456",
                "validationFiles": ["${GAME_EXE}"],
                "processNames": ["LabGame.exe"],
                "detection": [{ "type": "steam", "appId": "123456", "installDir": "Lab Game" }]
            },
            "install": { "targetDir": "${GAME_DIR}/BepInEx/plugins/LabCoop" },
            "components": [{ "id": "mod", "name": "LabCoop", "source": "LabCoop.dll", "target": "${INSTALL_DIR}", "required": true }],
            "launch": { "uri": "steam://rungameid/${STEAM_APP_ID}", "hint": "Host from the pause menu." }
        }
    })
}

fn read_rec(v: &serde_json::Value) -> Result<super::config::GameSpec, String> {
    super::community::read_record(&v.to_string(), &shipped_config(), false)
}

#[test]
fn an_approved_community_record_becomes_an_installable_card() {
    let g = read_rec(&record()).unwrap();
    assert_eq!(g.id, "lab-coop");
    assert_eq!(g.name, "Lab Game", "a community card is named after the game");
    assert_eq!(g.categories, ["community"]);
    let m = g.mod_spec.as_ref().unwrap();
    assert_eq!(m.package.version, "1.2.0");
    assert_eq!(m.package.sha256, SHA);
    assert!(m.package.url.starts_with("https://github.com/someone/lab-coop/releases/download/v1.2.0/"));
    assert!(m.package.github.is_empty() && m.package.mods_info.is_empty(), "only the pinned file, never the newest release");
    assert_eq!(m.info.publisher, "someone", "credited to its author");
    assert_eq!(m.info.website, "https://github.com/someone/lab-coop");
    assert_eq!(m.launch.uri, "steam://rungameid/${STEAM_APP_ID}");
    let ops: Vec<&str> = g.actions.iter().map(|a| a.mod_op.as_str()).collect();
    assert_eq!(ops, ["update", "install", "launch", "repair", "uninstall"]);
}

/// What the website's add-a-mod form writes when the release has no multiplayereverything.json (buildManifest in the
/// website's community-validate.js): no manifest_sha256, backslash paths, a Steam rule and what the mod needs first.
#[test]
fn a_record_built_from_the_websites_form_is_installable_too() {
    let mut r = record();
    r.as_object_mut().unwrap().remove("manifest_sha256");
    r["mod"] = serde_json::json!({
        "id": "lab-coop",
        "publisher": "someone",
        "website": "https://github.com/someone/lab-coop",
        "game": {
            "name": "Lab Game", "executable": "LabGame.exe", "steamAppId": "123456",
            "validationFiles": ["${GAME_EXE}"], "processNames": ["LabGame.exe"],
            "detection": [{ "type": "steam", "appId": "123456", "installDir": "Lab Game" }]
        },
        "install": {
            "targetDir": "${GAME_DIR}\\BepInEx\\plugins\\LabCoop",
            "requires": [{ "path": "BepInEx\\core", "message": "Install BepInEx 5 first" }]
        },
        "components": [{ "id": "mod", "name": "Lab Game", "source": "release\\LabCoop", "target": "${INSTALL_DIR}", "required": true }],
        "launch": { "uri": "steam://rungameid/${STEAM_APP_ID}" }
    });
    let g = read_rec(&r).unwrap();
    let m = g.mod_spec.as_ref().unwrap();
    assert_eq!(m.package.sha256, SHA);
    assert_eq!(m.install.target_dir, "${GAME_DIR}\\BepInEx\\plugins\\LabCoop");
    assert_eq!(m.components[0].source, "release\\LabCoop", "a folder inside the zip");

    // A mod released as one file: the asset is the DLL and the source is its own name.
    r["asset"] = "LabCoop.dll".into();
    r["url"] = "https://github.com/someone/lab-coop/releases/download/v1.2.0/LabCoop.dll".into();
    r["mod"]["components"][0]["source"] = "LabCoop.dll".into();
    let g = read_rec(&r).unwrap();
    assert!(g.mod_spec.as_ref().unwrap().package.url.ends_with("/LabCoop.dll"));

    // The form leaves a Steam app id out only by mistake; the launch line then has nothing to start.
    r["mod"]["game"]["steamAppId"] = "".into();
    r["mod"]["game"].as_object_mut().unwrap().remove("detection");
    assert!(read_rec(&r).is_err(), "no app id and steam://rungameid/${{STEAM_APP_ID}} can't be launched");
}

/// A mod moved from launcher.json keeps what its card showed (guide, colours, players) and where its install record
/// lives, so an install made before the move is still found.
#[test]
fn a_record_written_by_the_team_keeps_its_card_and_its_install_record() {
    let mut r = record();
    r["description"] = "Four players in one lab.".into();
    r["players"] = "2-4 players (online co-op)".into();
    r["color"] = "#6BA83A".into();
    r["colorEnd"] = "#17250C".into();
    r["guide"] = serde_json::json!({
        "needs": ["Your own copy of Lab Game."],
        "steps": ["Press Install.", "Press Launch."],
        "tips": ["It is a beta."],
        "keys": [{ "key": "F2", "does": "Invites friends." }],
        "links": [{ "label": "Discord", "url": "https://discord.com/invite/x" }]
    });
    r["mod"]["install"]["stateDir"] = "${INSTALL_DIR}\\.installer".into();
    r["mod"]["game"]["detection"][0]["subPath"] = "Atlas\\Binaries\\Win64".into();
    let g = read_rec(&r).unwrap();
    assert_eq!(g.description, "Four players in one lab.");
    assert_eq!((g.player_count.as_str(), g.max_players), ("2-4 players (online co-op)", 4));
    assert_eq!(g.guide.steps, ["Press Install.", "Press Launch."]);
    assert_eq!(g.guide.keys, [("F2".to_string(), "Invites friends.".to_string())]);
    assert!(g.guide.tips[0].starts_with("Made by someone, not by Multiplayer Everything"), "the credit comes first");
    assert_eq!(g.guide.tips[1], "It is a beta.");
    let links: Vec<&str> = g.guide.links.iter().map(|l| l.url.as_str()).collect();
    assert_eq!(links, ["https://discord.com/invite/x", "https://github.com/someone/lab-coop"], "the repository comes last");
    let m = g.mod_spec.as_ref().unwrap();
    assert_eq!(m.install.state_dir, "${INSTALL_DIR}\\.installer");
}

/// A mod with an installer of its own, or none yet: the card links to its page and installs nothing.
#[test]
fn a_link_record_is_a_card_with_one_button_to_the_mods_page() {
    let link = || {
        serde_json::json!({
            "id": "lab-link",
            "kind": "link",
            "repo": "someone/lab-link",
            "author": "someone",
            "approved_at": "2026-11-04",
            "approved_by": "Multiplayer Everything Team",
            "link": { "game": "Lab Link", "url": "https://github.com/someone/lab-link/releases" },
            "players": "2-16 players (alpha)",
            "maxPlayers": 4,
            "guide": { "steps": ["Open the mod's page."] }
        })
    };
    let g = read_rec(&link()).unwrap();
    assert_eq!((g.name.as_str(), g.categories.as_slice()), ("Lab Link", ["community".to_string()].as_slice()));
    assert!(g.mod_spec.is_none(), "nothing to install");
    assert_eq!(g.max_players, 4);
    assert_eq!(g.actions.len(), 1);
    assert_eq!((g.actions[0].label.as_str(), g.actions[0].link.as_str()), ("Open the mod's page", "https://github.com/someone/lab-link/releases"));
    assert!(g.guide.tips[0].contains("The launcher doesn't install, update or support it"));
    assert_eq!(g.guide.links.last().unwrap().url, "https://github.com/someone/lab-link");

    let mut cases: Vec<(serde_json::Value, &str)> = Vec::new();
    let mut add = |f: &dyn Fn(&mut serde_json::Value), want: &'static str| {
        let mut r = link();
        f(&mut r);
        cases.push((r, want));
    };
    add(&|r| r["link"]["url"] = "https://github.com/other/repo".into(), "link.url must be on https://github.com/someone/lab-link");
    add(&|r| r["link"]["url"] = "https://github.com/someone/lab-link-evil".into(), "link.url must be on");
    add(&|r| r["link"]["url"] = "https://evil.example/someone/lab-link".into(), "link.url must be on");
    add(&|r| r["link"]["game"] = "".into(), "link.game is missing");
    add(&|r| r["mod"] = serde_json::json!({}), "record.mod: not allowed");
    add(&|r| r["sha256"] = SHA.into(), "record.sha256: not allowed");
    add(&|r| r["link"]["exe"] = "x.exe".into(), "link.exe: not allowed");
    add(&|r| r["kind"] = "ruffle".into(), "kind \"ruffle\" isn't known to this launcher");
    for (r, want) in cases {
        let e = read_rec(&r).map(|g| g.id).unwrap_err();
        assert!(e.contains(want), "wanted {want:?}, got {e:?}");
    }
}

/// FL_TEST_COMMUNITY_DIR=<a mods-info checkout>: every record in its community/ passes. One whose id launcher.json
/// still has makes the same card as that entry (how the community entries moved to mods-info was checked).
#[test]
fn the_records_in_a_mods_info_checkout_pass() {
    let Ok(dir) = std::env::var("FL_TEST_COMMUNITY_DIR") else { return };
    let index = std::fs::read(format!("{dir}\\community\\index.json")).unwrap();
    let ids = super::community::parse_index(&index).unwrap();
    let shipped = shipped_config();
    for id in &ids {
        let text = std::fs::read_to_string(format!("{dir}\\community\\{id}.json")).unwrap();
        let mut cfg = shipped.clone();
        let before = cfg.games.iter().position(|g| &g.id == id).map(|i| cfg.games.remove(i));
        let g = super::community::read_record(&text, &cfg, false).unwrap_or_else(|e| panic!("{id}: {e}"));
        let Some(b) = before else { continue };
        assert_eq!((&g.name, &g.description, &g.player_count, g.max_players), (&b.name, &b.description, &b.player_count, b.max_players), "{id}");
        assert_eq!((g.color, g.color_end), (b.color, b.color_end), "{id}");
        assert_eq!((&g.guide.needs, &g.guide.steps, &g.guide.keys), (&b.guide.needs, &b.guide.steps, &b.guide.keys), "{id}");
        // The credit comes first now; the other tips keep their order.
        let others = |x: &super::config::GameSpec| x.guide.tips.iter().filter(|t| !t.starts_with("Made by ")).cloned().collect::<Vec<_>>();
        assert_eq!(others(&g), others(&b), "{id}: the tips besides the credit");
        assert_eq!(g.guide.tips.iter().filter(|t| t.starts_with("Made by ")).count(), 1, "{id}: one credit");
        assert!(g.guide.tips[0].starts_with("Made by "), "{id}");
        let urls = |x: &super::config::GameSpec| x.guide.links.iter().map(|l| l.url.clone()).collect::<std::collections::BTreeSet<_>>();
        let mut want = urls(&b);
        want.insert(format!("https://github.com/{}", text.split("\"repo\": \"").nth(1).unwrap().split('"').next().unwrap()));
        assert_eq!(urls(&g), want, "{id}: links");
        match (&g.mod_spec, &b.mod_spec) {
            (Some(m), Some(n)) => {
                assert_eq!(format!("{:?}", m.game), format!("{:?}", n.game), "{id}: the game");
                assert_eq!(format!("{:?}", m.install), format!("{:?}", n.install), "{id}: where it installs");
                assert_eq!(format!("{:?}", m.components), format!("{:?}", n.components), "{id}: what it installs");
                assert_eq!(format!("{:?}", m.launch), format!("{:?}", n.launch), "{id}: how it starts");
                assert_eq!((&m.package.version, &m.package.url, &m.package.sha256), (&n.package.version, &n.package.url, &n.package.sha256), "{id}");
                assert_eq!((&m.info.publisher, &m.info.website), (&n.info.publisher, &n.info.website), "{id}");
                let ops = |x: &super::config::GameSpec| x.actions.iter().filter(|a| !a.mod_op.is_empty()).map(|a| a.mod_op.clone()).collect::<Vec<_>>();
                assert_eq!(ops(&g), ops(&b), "{id}: the mod's buttons");
            }
            (None, None) => {
                let links = |x: &super::config::GameSpec| x.actions.iter().map(|a| (a.label.clone(), a.link.clone())).collect::<Vec<_>>();
                assert_eq!(links(&g), links(&b), "{id}: the page button");
            }
            _ => panic!("{id}: one installs and the other doesn't"),
        }
    }
    assert!(!ids.is_empty());
}

/// The team records in a mods-info checkout (FL_TEST_COMMUNITY_DIR) are exactly the published copy the tests use
/// (tests/fixtures/team), so what the tests prove holds for what players get. After a record changes on mods-info,
/// copy team/ into tests/fixtures/team.
#[test]
fn the_team_records_in_a_mods_info_checkout_are_the_published_copy() {
    let Ok(dir) = std::env::var("FL_TEST_COMMUNITY_DIR") else { return };
    let Ok(index) = std::fs::read(format!("{dir}\\team\\index.json")) else { return };
    let ids = super::community::parse_index(&index).unwrap();
    let copy = super::tests::team_fixtures();
    assert_eq!(ids.len(), copy.raw.len(), "the number of records");
    let norm = |t: String| t.replace("\r\n", "\n");
    for (id, mine) in ids.iter().zip(copy.raw) {
        let theirs = std::fs::read_to_string(format!("{dir}\\team\\{id}.json")).unwrap();
        assert_eq!(norm(theirs), norm(mine), "{id}: mods-info and tests/fixtures/team differ");
    }
}

#[test]
fn a_community_record_that_could_do_more_than_it_shows_is_refused_whole() {
    let mut cases: Vec<(serde_json::Value, &str)> = Vec::new();
    let mut add = |f: &dyn Fn(&mut serde_json::Value), want: &'static str| {
        let mut r = record();
        f(&mut r);
        cases.push((r, want));
    };
    add(&|r| r["extra"] = "x".into(), "record.extra: not allowed");
    // A developer's record never picks its own download: only a loader from its own releases, pinned.
    add(&|r| r["mod"]["package"] = serde_json::json!({ "github": "x/y" }), "mod.package.github: not allowed");
    add(&|r| r["mod"]["package"] = serde_json::json!({ "extras": [{ "dir": "loader", "tag": "b1", "asset": "L.zip", "sha256": "a".repeat(64),
        "url": "https://github.com/other/repo/releases/download/b1/L.zip" }] }), "extras[0].url must be https://github.com/someone/lab-coop/");
    add(&|r| r["mod"]["package"] = serde_json::json!({ "extras": [{ "dir": "loader", "tag": "b1", "asset": "L.zip",
        "url": "https://github.com/someone/lab-coop/releases/download/b1/L.zip" }] }), "extras[0].sha256 must be");
    add(&|r| r["mod"]["package"] = serde_json::json!({ "extras": [{ "dir": "../up", "tag": "b1", "asset": "L.zip", "sha256": "a".repeat(64),
        "url": "https://github.com/someone/lab-coop/releases/download/b1/L.zip" }] }), "extras[0].dir: a folder name");
    add(&|r| r["mod"]["package"] = serde_json::json!({ "extras": [{ "dir": "l", "github": "x/y", "tag": "b1", "asset": "L.zip", "sha256": "a".repeat(64),
        "url": "https://github.com/someone/lab-coop/releases/download/b1/L.zip" }] }), "extras[0].github: not allowed");
    // A program to start: in the game's folder, and instead of Steam, not as well.
    add(&|r| r["mod"]["launch"]["executable"] = "${GAME_DIR}\\Lab.exe".into(), "either uri");
    add(&|r| r["mod"]["launch"] = serde_json::json!({ "executable": "cmd.exe" }), "launch.executable must be ${GAME_DIR}");
    add(&|r| r["mod"]["launch"] = serde_json::json!({ "executable": "${GAME_DIR}\\..\\x.exe" }), "launch.executable must be ${GAME_DIR}");
    add(&|r| r["mod"]["launch"] = serde_json::json!({ "executable": "${GAME_DIR}\\run.bat" }), "launch.executable must be ${GAME_DIR}");
    add(&|r| r["mod"]["launch"] = serde_json::json!({ "executable": "${GAME_DIR}\\${env:COMSPEC}.exe" }), "launch.executable must be ${GAME_DIR}");
    add(&|r| r["mod"]["components"][0]["skipIfPresent"] = serde_json::json!(["..\\Windows"]), "skipIfPresent: one to ten paths");
    add(&|r| r["mod"]["components"][0]["skipIfPresent"] = serde_json::json!([]), "skipIfPresent: one to ten paths");
    add(&|r| r["mod"]["install"]["uninstallCleanupWith"] = "nope".into(), "uninstallCleanupWith: the id of one");
    add(&|r| r["minLauncher"] = "soon".into(), "minLauncher: a version");
    add(&|r| r["mod"]["launch"]["uri"] = "https://example.com".into(), "steam://rungameid");
    add(&|r| r["mod"]["install"]["allowedRoots"] = serde_json::json!(["C:/"]), "allowedRoots: not allowed");
    add(&|r| r["mod"]["install"]["targetDir"] = "C:/Windows".into(), "targetDir must be inside");
    add(&|r| r["mod"]["install"]["targetDir"] = "${GAME_DIR}/../..".into(), "targetDir must be inside");
    add(&|r| r["mod"]["components"][0]["target"] = "%APPDATA%".into(), "target must be inside");
    add(&|r| r["mod"]["install"]["stateDir"] = "%APPDATA%\\x".into(), "stateDir must be inside");
    add(&|r| r["mod"]["install"]["stateDir"] = "${GAME_DIR}\\..\\x".into(), "stateDir must be inside");
    add(&|r| r["mod"]["game"]["detection"][0]["subPath"] = "..\\Other".into(), "subPath: a folder inside");
    add(&|r| r["mod"]["game"]["detection"][0]["subPath"] = "C:\\Windows".into(), "subPath: a folder inside");
    add(&|r| r["mod"]["game"]["detection"][0]["subPath"] = "${env:TEMP}".into(), "subPath: a folder inside");
    add(&|r| r["guide"] = serde_json::json!({ "script": ["x"] }), "guide.script: not allowed");
    add(&|r| r["guide"] = serde_json::json!({ "links": [{ "label": "a", "url": "http://plain.example" }] }), "https://");
    add(&|r| r["guide"] = serde_json::json!({ "links": [{ "label": "a", "url": "https://a" }, { "label": "b", "url": "https://b" }, { "label": "c", "url": "https://c" }, { "label": "d", "url": "https://d" }] }), "up to 3");
    add(&|r| r["description"] = "x".repeat(601).into(), "description: text, at most 600");
    add(&|r| r["color"] = "red".into(), "color");
    // The engine looks for a requirement under the game folder: with ${GAME_DIR} in front, Install would never turn on.
    add(&|r| r["mod"]["install"]["requires"] = serde_json::json!([{ "path": "${GAME_DIR}\\BepInEx\\core", "message": "x" }]), "without ${GAME_DIR}");
    add(&|r| r["mod"]["components"][0]["source"] = "../secret".into(), "source: a path inside the package");
    add(&|r| r["mod"]["components"][0]["sourceGame"] = serde_json::json!(["x"]), "sourceGame: not allowed");
    add(&|r| r["mod"]["components"] = serde_json::json!([]), "at least one");
    add(&|r| r["mod"]["game"]["detection"][0] = serde_json::json!({ "type": "registry", "key": "HKLM" }), "only \"steam\" and \"path\"");
    add(&|r| r["mod"]["game"]["executable"] = "../evil.exe".into(), "must be a file name");
    add(&|r| r["mod"]["website"] = "https://evil.example".into(), "mod.website must be");
    add(&|r| r["mod"]["id"] = "other".into(), "mod.id must be the record's id");
    add(&|r| r["url"] = "https://evil.example/LabCoop-1.2.0.zip".into(), "url must be https://github.com/someone/lab-coop/");
    add(&|r| r["url"] = "https://github.com/other/repo/releases/download/v1.2.0/LabCoop-1.2.0.zip".into(), "url must be");
    add(&|r| r["sha256"] = "ABC".into(), "sha256 must be");
    add(&|r| r["id"] = "Lab Coop".into(), "id: lowercase");
    add(&|r| r["repo"] = "no-slash".into(), "repo must be");
    add(&|r| r["tag"] = "../x".into(), "plain names");
    add(&|r| r["id"] = "spooky-together".into(), "launcher.json already has");
    for (r, want) in cases {
        let e = read_rec(&r).map(|g| g.id).unwrap_err();
        assert!(e.contains(want), "wanted {want:?}, got {e:?}");
    }
    assert!(super::community::read_record("[1]", &shipped_config(), false).is_err());
    assert!(super::community::read_record("not json", &shipped_config(), false).is_err());
}

/// What the developer SDK gained: a loader from the mod's own releases (pinned like the mod), a component left out when
/// the game has a loader already, the loader's leftovers removed with it, and a program in the game folder to start.
#[test]
fn a_community_record_may_bring_its_loader_skip_it_and_start_a_program() {
    let mut r = record();
    let sha = "b".repeat(64);
    r["mod"]["package"] = serde_json::json!({ "extras": [{ "dir": "loader", "tag": "bepinex-5.4.23.5", "asset": "BepInEx_win_x64_5.4.23.5.zip",
        "sha256": sha, "url": "https://github.com/someone/lab-coop/releases/download/bepinex-5.4.23.5/BepInEx_win_x64_5.4.23.5.zip" }] });
    let mut loader = r["mod"]["components"][0].clone();
    loader["id"] = "loader".into();
    loader["source"] = "loader/BepInEx/core".into();
    loader["target"] = "${GAME_DIR}\\BepInEx\\core".into();
    loader["skipIfPresent"] = serde_json::json!(["BepInEx/core"]);
    r["mod"]["components"].as_array_mut().unwrap().insert(0, loader);
    r["mod"]["install"]["uninstallCleanupWith"] = "loader".into();
    r["mod"]["install"]["uninstallCleanup"] = serde_json::json!(["BepInEx/cache/**"]);
    r["mod"]["launch"] = serde_json::json!({ "executable": "${GAME_DIR}\\LabCoopLauncher.exe", "hint": "Pick Host or Join." });
    let g = read_rec(&r).unwrap_or_else(|e| panic!("{e}"));
    let m = g.mod_spec.as_ref().unwrap();
    let e = &m.package.extras[0];
    assert_eq!((e.dir.as_str(), e.sha256.as_str()), ("loader", sha.as_str()));
    assert!(e.url.ends_with("/someone/lab-coop/releases/download/bepinex-5.4.23.5/BepInEx_win_x64_5.4.23.5.zip"));
    // The mod's own package is still the one the approval pinned.
    assert_eq!(m.package.sha256, record()["sha256"].as_str().unwrap());
    assert_eq!(m.components[0].skip_if_present, vec!["BepInEx/core".to_string()]);
    assert_eq!(m.install.uninstall_cleanup_with, "loader");
    assert_eq!(m.launch.executable, "${GAME_DIR}\\LabCoopLauncher.exe");
    assert!(m.launch.uri.is_empty());
}

/// "minLauncher": a launcher too old for a record says so on a card with nothing to press, and reads nothing else of
/// it (a newer record may use keys it doesn't know); one new enough reads it as usual.
#[test]
fn a_record_for_a_newer_launcher_asks_for_an_update() {
    let mut r = record();
    r["minLauncher"] = "999.0".into();
    r["mod"]["somethingNew"] = serde_json::json!({ "x": 1 });
    let g = read_rec(&r).unwrap_or_else(|e| panic!("{e}"));
    assert!(g.coming_soon && g.actions.is_empty() && g.mod_spec.is_none());
    assert_eq!(g.name, r["mod"]["game"]["name"].as_str().unwrap());
    assert!(g.description.contains("999.0 or newer"), "{}", g.description);
    assert_eq!(g.categories, vec!["community".to_string()]);
    let mut ok = record();
    ok["minLauncher"] = "1.0".into();
    assert!(read_rec(&ok).unwrap_or_else(|e| panic!("{e}")).mod_spec.is_some());
}

// ---- team records (team/<id>.json): our own mods, as launcher.json holds them today ------------------------------

/// The published team record `id` (tests/fixtures/team), and the shipped config without that game: where reading the
/// record must make the game again.
fn team_record(id: &str) -> (serde_json::Value, super::config::Config) {
    let path = format!("{}\\..\\tests\\fixtures\\team\\{id}.json", env!("CARGO_MANIFEST_DIR"));
    let r: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
    let mut cfg = shipped_config();
    cfg.games.retain(|g| g.id != id);
    (r, cfg)
}

fn read_team(r: &serde_json::Value, cfg: &super::config::Config) -> Result<super::config::GameSpec, String> {
    super::community::read_team_record(&r.to_string(), cfg, false)
}

/// A published team record read on its own makes the very game the shipped config has (card, guide, categories, the
/// online rooms, every building block of the mod). That these are the games launcher.json had was proved, record by
/// record, before the entries left it (commits 24f88f4 and 87aa2e7).
#[test]
fn a_team_record_is_the_same_game_as_its_launcher_json_entry() {
    for id in ["spooky-together", "gta-coop"] {
        let (r, cfg) = team_record(id);
        let from_record = read_team(&r, &cfg).unwrap_or_else(|e| panic!("{id}: {e}"));
        let shipped = shipped_config();
        let from_json = shipped.games.iter().find(|g| g.id == id).unwrap();
        assert_eq!(format!("{from_record:?}"), format!("{from_json:?}"), "{id}");
    }
}

/// Our Flash games as team records: the same card, the same buttons with the same arguments, the same host options.
#[test]
fn a_flash_team_record_is_the_same_game_as_its_launcher_json_entry() {
    for id in ["gunmayhem2", "zombotron", "fireboywatergirlforesttemple"] {
        let (r, cfg) = team_record(id);
        let from_record = read_team(&r, &cfg).unwrap_or_else(|e| panic!("{id}: {e}"));
        let shipped = shipped_config();
        let from_json = shipped.games.iter().find(|g| g.id == id).unwrap();
        assert!(!from_json.actions.is_empty() && from_json.mod_spec.is_none());
        assert_eq!(format!("{from_record:?}"), format!("{from_json:?}"), "{id}");
    }
}

/// A Flash record's buttons only ever start one of launcher.json's players with a game file: no program, page, server
/// or link of their own, and no files outside the games folder (or a copy Flashpoint or the Desktop has).
#[test]
fn a_flash_team_record_only_starts_our_player_with_a_game_file() {
    let (base, cfg) = team_record("zombotron");
    let mut cases: Vec<(serde_json::Value, &str)> = Vec::new();
    let mut add = |f: &dyn Fn(&mut serde_json::Value), want: &'static str| {
        let mut r = base.clone();
        f(&mut r);
        cases.push((r, want));
    };
    add(&|r| r["actions"][0]["exe"] = serde_json::json!(["cmd.exe"]), "actions[0].exe: not allowed");
    add(&|r| r["actions"][0]["server"] = serde_json::json!({ "exe": ["x.exe"] }), "actions[0].server: not allowed");
    add(&|r| r["actions"][0]["url"] = "https://example.com".into(), "actions[0].url: not allowed");
    add(&|r| r["actions"][0]["link"] = "https://example.com".into(), "actions[0].link: not allowed");
    add(&|r| r["actions"][0]["cwd"] = "C:\\".into(), "actions[0].cwd: not allowed");
    add(&|r| { r["actions"][0].as_object_mut().unwrap().remove("swf"); }, "actions[0].swf: game files");
    add(&|r| r["actions"][0]["swf"] = "C:\\Windows\\x.swf".into(), "actions[0].swf: game files");
    add(&|r| r["actions"][0]["swf"] = "${GAMES_DIR}\\..\\..\\x.swf".into(), "actions[0].swf: game files");
    add(&|r| r["actions"][0]["swf"] = "${GAMES_DIR}\\${env:TEMP}\\x.swf".into(), "actions[0].swf: game files");
    add(&|r| r["actions"][0]["players"] = serde_json::json!(["ghost"]), "players");
    add(&|r| r["actions"] = serde_json::json!([]), "actions: one to twenty");
    add(&|r| r["mod"] = serde_json::json!({ "id": "zombotron" }), "one of them");
    add(&|r| r["flash"] = serde_json::json!({}), "one of them");
    add(&|r| { r.as_object_mut().unwrap().remove("actions"); }, "one of them");
    for (r, want) in cases {
        let e = read_team(&r, &cfg).map(|g| g.id).unwrap_err();
        assert!(e.contains(want), "wanted {want:?}, got {e:?}");
    }
}

#[test]
fn a_team_record_comes_only_from_the_teams_repositories() {
    let (base, cfg) = team_record("spooky-together");
    let mut cases: Vec<(serde_json::Value, &str)> = Vec::new();
    let mut add = |f: &dyn Fn(&mut serde_json::Value), want: &'static str| {
        let mut r = base.clone();
        f(&mut r);
        cases.push((r, want));
    };
    add(&|r| r["repo"] = "someone/spooky-together".into(), "team records are the team's own repositories");
    add(&|r| r["mod"]["package"]["github"] = "multiplayereverything/other".into(), "mod.package.github must be multiplayereverything/spooky-together");
    add(&|r| r["mod"]["package"]["extras"][0]["github"] = "someone/loader".into(), "extras[0].github must be a multiplayereverything repository");
    add(&|r| r["mod"]["package"]["url"] = "https://evil.example/x.zip".into(), "mod.package.url must be a release file");
    add(&|r| r["mod"]["package"]["url"] = "https://github.com/multiplayereverything/other/releases/download/v1/x.zip".into(), "mod.package.url must be a release file");
    add(&|r| r["categories"] = serde_json::json!(["community"]), "\"community\" is for developers' records");
    add(&|r| r["actions"] = serde_json::json!([]), "one of them");
    add(&|r| r["kind"] = "link".into(), "team record.kind: not allowed");
    add(&|r| r["mod"]["id"] = "other".into(), "mod.id must be the record's id");
    add(&|r| r["mod"] = serde_json::Value::Null, "no \"mod\" object");
    add(&|r| { r["mod"].as_object_mut().unwrap().remove("package"); }, "mod.package is missing");
    add(&|r| r["id"] = "gta-coop".into(), "launcher.json already has");
    add(&|r| r["minLauncher"] = "next".into(), "minLauncher: a version");
    // Then every check a launcher.json game gets.
    add(&|r| r["categories"] = serde_json::json!(["nope"]), "categories");
    for (r, want) in cases {
        let e = read_team(&r, &cfg).map(|g| g.id).unwrap_err();
        assert!(e.contains(want), "wanted {want:?}, got {e:?}");
    }
}

#[test]
fn a_team_record_for_a_newer_launcher_keeps_its_place_and_asks_for_an_update() {
    let (mut r, cfg) = team_record("spooky-together");
    r["minLauncher"] = "999.0".into();
    r["mod"]["somethingNew"] = serde_json::json!(true);
    let g = read_team(&r, &cfg).unwrap_or_else(|e| panic!("{e}"));
    assert!(g.coming_soon && g.mod_spec.is_none() && g.actions.is_empty());
    assert_eq!(g.name, "Spooky Together");
    assert_eq!(g.categories, vec!["pc", "online", "coop"]);
}

/// The window lists team records before community ones, so a developer's record can't take a team id.
#[test]
fn team_records_merge_first_and_win_their_ids() {
    let (r, mut cfg) = team_record("spooky-together");
    let team = super::community::Cache { fetched_at: 0, raw: vec![r.to_string()] };
    assert!(super::community::merge_team(&mut cfg, &team).is_empty());
    let mut dev = record();
    dev["id"] = "spooky-together".into();
    dev["mod"]["id"] = "spooky-together".into();
    let left = super::community::merge(&mut cfg, &super::community::Cache { fetched_at: 0, raw: vec![dev.to_string()] });
    assert_eq!(left.len(), 1, "{left:?}");
    assert_eq!(cfg.games.iter().filter(|g| g.id == "spooky-together").count(), 1);
    assert!(cfg.games.iter().find(|g| g.id == "spooky-together").unwrap().categories.contains(&"pc".to_string()));
}

// ---- Flash records: the launcher makes the buttons (1.0.43) --------------------------------------------------------

fn short_team_record(id: &str) -> serde_json::Value {
    let path = format!("{}\\..\\tests\\fixtures\\team-short\\{id}.json", env!("CARGO_MANIFEST_DIR"));
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}

/// What a button starts: everything but its texts.
fn command_of(a: &super::config::ActionSpec) -> String {
    format!(
        "{} swf={:?} args={:?} join={:?} offline={} more={} original={} players={:?} icon={}",
        a.id, a.swf, a.args, a.join_args, a.offline, a.more, a.original, a.players, a.icon
    )
}

/// Our Flash games in the short form (tests/fixtures/team-short, made by scripts/flash-records-short.py from the
/// published records) make the same buttons with the same command lines, the same card and the same host options as
/// the hand-written records. Only a few button texts may read differently, as launcher.json words them now.
#[test]
fn flash_team_records_in_the_short_form_make_the_same_buttons() {
    let dir = format!("{}\\..\\tests\\fixtures\\team-short", env!("CARGO_MANIFEST_DIR"));
    let mut seen = 0;
    for e in std::fs::read_dir(dir).unwrap() {
        let name = e.unwrap().file_name().to_string_lossy().to_string();
        let id = name.trim_end_matches(".json");
        if id == "index" {
            continue;
        }
        let short = short_team_record(id);
        let (published, cfg) = team_record(id);
        if short.get("flash").is_none() {
            assert_eq!(short, published, "{id}: only Flash records change");
            continue;
        }
        assert!(published.get("actions").is_some() && short.get("actions").is_none(), "{id}");
        let a = read_team(&short, &cfg).unwrap_or_else(|e| panic!("{id}: {e}"));
        let b = read_team(&published, &cfg).unwrap_or_else(|e| panic!("{id}: {e}"));
        assert!(!a.coming_soon, "{id}");
        let ca: Vec<String> = a.actions.iter().map(command_of).collect();
        let cb: Vec<String> = b.actions.iter().map(command_of).collect();
        assert_eq!(ca, cb, "{id}");
        for (x, y) in a.actions.iter().zip(&b.actions) {
            assert!(!x.label.is_empty() && !x.description.is_empty(), "{id}.{}", x.id);
            // The texts the records' own wording kept: the offline button's and the original's.
            if ["offline", "local", "play"].contains(&x.id.as_str()) {
                assert_eq!((&x.label, &x.description), (&y.label, &y.description), "{id}.{}", x.id);
            }
        }
        let card = |g: &super::config::GameSpec| {
            format!(
                "{} {} {:?} {} {} {} {:?} {:?} {:?}",
                g.name, g.description, g.categories, g.online_id, g.join_action, g.max_players, g.guide, g.settings, g.color
            )
        };
        assert_eq!(card(&a), card(&b), "{id}");
        seen += 1;
    }
    assert_eq!(seen, 16); // Bad Ice-Cream keeps its own buttons: its online ones pass no key map
}

/// The generated buttons for 2 players and for 6: rooms and hosts up to maxPlayers, the texts from launcher.json.
#[test]
fn the_launcher_makes_a_flash_games_buttons_from_its_player_count() {
    let (_, cfg) = team_record("chaosfaction2");
    let g = read_team(&short_team_record("chaosfaction2"), &cfg).unwrap();
    let ids: Vec<&str> = g.actions.iter().map(|a| a.id.as_str()).collect();
    assert_eq!(ids, ["online", "room3", "room4", "room5", "room6", "host", "host3", "host4", "host5", "host6", "join", "offline"]);
    let room5 = &g.actions[3];
    assert_eq!((room5.label.as_str(), room5.description.as_str()), ("Host a room for 5 players", "The game starts when 4 friends have joined"));
    assert_eq!(room5.args, ["--coop-host-relay", "${SERVER}", "--coop-name", "${PLAYER_NAME}", "--coop-keys", "chaos-faction-2", "--coop-players", "5"]);
    assert!(g.actions[0].hint.ends_with("For up to 6 players use Other ways to connect."));
    let (_, cfg) = team_record("fireboywatergirlforesttemple");
    let g = read_team(&short_team_record("fireboywatergirlforesttemple"), &cfg).unwrap();
    let ids: Vec<&str> = g.actions.iter().map(|a| a.id.as_str()).collect();
    assert_eq!(ids, ["online", "host", "join", "offline"]);
    assert_eq!(g.actions[0].hint, "Send the room code the message shows to your friend. The game opens once they join.");
    assert_eq!(g.actions[1].label, "Host with your IP (2 players)");
}

#[test]
fn a_short_flash_team_record_says_only_what_the_launcher_can_make() {
    let (_, cfg) = team_record("zombotron");
    let base = short_team_record("zombotron");
    let mut cases: Vec<(serde_json::Value, &str)> = Vec::new();
    let mut add = |f: &dyn Fn(&mut serde_json::Value), want: &'static str| {
        let mut r = base.clone();
        f(&mut r);
        cases.push((r, want));
    };
    add(&|r| r["flash"]["args"] = serde_json::json!(["--evil"]), "flash.args: not allowed");
    add(&|r| r["flash"]["file"] = "C:\\Windows\\x.swf".into(), "flash.file: a game file");
    add(&|r| r["flash"]["file"] = "${GAMES_DIR}\\..\\x.swf".into(), "flash.file: a game file");
    add(&|r| r["flash"]["keys"] = "..\\..\\x.keys".into(), "flash.keys: a key map");
    add(&|r| r["flash"]["keys"] = "--coop-host".into(), "flash.keys: a key map");
    add(&|r| r["flash"]["maxPlayers"] = 7.into(), "flash.maxPlayers");
    add(&|r| r["flash"]["maxPlayers"] = 1.into(), "flash.maxPlayers");
    add(&|r| r["flash"]["maxPlayers"] = 2.5.into(), "flash.maxPlayers");
    add(&|r| r["flash"]["offline"] = true.into(), "flash.offline: an object");
    add(&|r| r["flash"]["offline"] = serde_json::json!({ "args": ["x"] }), "flash.offline.args: not allowed");
    add(&|r| r["flash"]["offline"] = serde_json::json!({ "keys": "a b" }), "flash.offline.keys");
    add(&|r| r["flash"]["offline"] = serde_json::json!({ "label": "x".repeat(41) }), "flash.offline.label");
    add(&|r| r["flash"]["original"] = serde_json::json!({ "files": [] }), "flash.original.files");
    add(&|r| r["flash"]["original"] = serde_json::json!({ "files": ["C:\\x.swf"] }), "flash.original.files");
    add(&|r| r["flash"]["ruffle"] = "latest".into(), "flash.ruffle");
    add(&|r| r["flash"] = "x".into(), "flash must be an object");
    for (r, want) in cases {
        let e = read_team(&r, &cfg).map(|g| g.id).err().unwrap_or_else(|| panic!("{want}: read"));
        assert!(e.contains(want), "wanted {want:?}, got {e:?}");
    }
    let mut r = base.clone();
    r["flash"]["ruffle"] = "1.0.5".into();
    assert_eq!(read_team(&r, &cfg).unwrap().min_player, "1.0.5");
}

/// A developer's Flash game as the website writes it into community/<id>.json.
fn flash_record() -> serde_json::Value {
    let rel = "https://github.com/someone/lab-flash/releases/download/v2.0";
    serde_json::json!({
        "id": "lab-flash-coop",
        "repo": "someone/lab-flash",
        "kind": "flash",
        "minLauncher": "1.0.43",
        "tag": "v2.0",
        "version": "2.0",
        "manifest_sha256": SHA,
        "author": "someone",
        "approved_at": "2026-11-04",
        "approved_by": "JessniX",
        "description": "A lab game for two.",
        "flash": {
            "game": "Lab Flash",
            "file": { "asset": "LabFlash_coop.swf", "url": format!("{rel}/LabFlash_coop.swf"), "sha256": SHA, "size": 1234 },
            "keys": { "asset": "lab-flash.keys", "url": format!("{rel}/lab-flash.keys"), "sha256": SHA },
            "maxPlayers": 3,
            "offline": { "local": true },
            "ruffle": "1.0.5"
        }
    })
}

#[test]
fn a_developers_flash_record_downloads_its_pinned_files_and_gets_the_standard_buttons() {
    let g = read_rec(&flash_record()).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!((g.id.as_str(), g.name.as_str(), g.site_id.as_str()), ("lab-flash-coop", "Lab Flash", "lab-flash-coop"));
    assert_eq!(g.categories, ["community", "flash"]);
    assert_eq!((g.online_id.as_str(), g.join_action.as_str(), g.max_players), ("ruffle-coop", "online", 3));
    assert_eq!(g.min_player, "1.0.5");
    assert!(g.mod_spec.is_none() && !g.coming_soon);
    let names: Vec<(&str, &str, &str)> = g.downloads.iter().map(|f| (f.dir.as_str(), f.name.as_str(), f.url.as_str())).collect();
    assert_eq!(
        names,
        [
            ("community\\lab-flash-coop", "LabFlash_coop.swf", "https://github.com/someone/lab-flash/releases/download/v2.0/LabFlash_coop.swf"),
            ("community\\lab-flash-coop", "lab-flash.keys", "https://github.com/someone/lab-flash/releases/download/v2.0/lab-flash.keys"),
        ]
    );
    assert_eq!((g.downloads[0].size, g.downloads[1].size), (1234, 0));
    let ids: Vec<&str> = g.actions.iter().map(|a| a.id.as_str()).collect();
    assert_eq!(ids, ["online", "room3", "host", "host3", "join", "local"]);
    for a in &g.actions {
        assert_eq!(a.swf, ["${GAMES_DIR}\\community\\lab-flash-coop\\LabFlash_coop.swf"], "{}", a.id);
        let k = a.args.iter().position(|x| x == "--coop-keys").unwrap();
        assert_eq!(a.args[k + 1], "${GAMES_DIR}\\community\\lab-flash-coop\\lab-flash.keys", "{}", a.id);
        assert!(a.server.exe.is_empty() && a.link.is_empty() && a.page.is_empty(), "{}", a.id);
    }
    assert_eq!(g.actions[5].description, "Up to 3 players on this PC");
    assert!(g.guide.tips[0].starts_with("Made by someone, not by Multiplayer Everything. We use the exact version we looked at (2.0)"));
    assert_eq!(g.guide.links.last().unwrap().url, "https://github.com/someone/lab-flash");
    // A built-in key map instead of a file: one download.
    let mut r = flash_record();
    r["flash"]["keys"] = "gun-mayhem-2".into();
    let g = read_rec(&r).unwrap();
    assert_eq!(g.downloads.len(), 1);
    assert!(g.actions[0].args.windows(2).any(|w| w == ["--coop-keys", "gun-mayhem-2"]));
}

#[test]
fn a_developers_flash_record_downloads_only_its_own_pinned_release() {
    let mut cases: Vec<(serde_json::Value, &str)> = Vec::new();
    let mut add = |f: &dyn Fn(&mut serde_json::Value), want: &'static str| {
        let mut r = flash_record();
        f(&mut r);
        cases.push((r, want));
    };
    add(&|r| r["flash"]["file"]["url"] = "https://github.com/other/repo/releases/download/v2.0/LabFlash_coop.swf".into(), "flash.file.url must be");
    add(&|r| r["flash"]["file"]["url"] = "https://evil.example/LabFlash_coop.swf".into(), "flash.file.url must be");
    add(&|r| r["flash"]["file"]["url"] = "https://github.com/someone/lab-flash/releases/download/v1.0/LabFlash_coop.swf".into(), "flash.file.url must be");
    add(&|r| r["flash"]["file"]["sha256"] = "abc".into(), "flash.file.sha256");
    add(&|r| r["flash"]["file"]["asset"] = "LabFlash.exe".into(), "flash.file.asset: a .swf file");
    add(&|r| r["flash"]["file"]["asset"] = "..\\x.swf".into(), "flash.file.asset");
    add(&|r| r["flash"]["file"]["size"] = (-1).into(), "flash.file.size");
    add(&|r| r["flash"]["file"]["dir"] = "C:\\".into(), "flash.file.dir: not allowed");
    add(&|r| r["flash"]["file"] = "${GAMES_DIR}\\x.swf".into(), "flash.file: the release file");
    add(&|r| r["flash"]["keys"]["asset"] = "lab.swf".into(), "flash.keys.asset: a .keys file");
    add(&|r| r["flash"]["keys"] = "C:\\x.keys".into(), "flash.keys: a key map");
    add(&|r| { r["flash"].as_object_mut().unwrap().remove("keys"); }, "flash.keys");
    add(&|r| r["flash"]["original"] = serde_json::json!({ "files": ["${GAMES_DIR}\\x.swf"] }), "flash.original: not allowed");
    add(&|r| r["flash"]["args"] = serde_json::json!(["--x"]), "flash.args: not allowed");
    add(&|r| { r["flash"].as_object_mut().unwrap().remove("game"); }, "flash.game");
    add(&|r| r["actions"] = serde_json::json!([]), "record.actions: not allowed");
    add(&|r| r["mod"] = serde_json::json!({}), "record.mod: not allowed");
    add(&|r| r["categories"] = serde_json::json!(["flash"]), "record.categories: not allowed");
    add(&|r| r["online"] = serde_json::json!({ "id": "x" }), "record.online: not allowed");
    add(&|r| r["tag"] = "../x".into(), "tag must be a plain name");
    add(&|r| r["id"] = "gunmayhem2".into(), "already has");
    add(&|r| r["kind"] = "flash2".into(), "isn't known to this launcher");
    for (r, want) in cases {
        let e = read_rec(&r).map(|g| g.id).unwrap_err();
        assert!(e.contains(want), "wanted {want:?}, got {e:?}");
    }
}

#[test]
fn a_flash_record_for_a_newer_launcher_is_named_after_its_game() {
    let mut r = flash_record();
    r["minLauncher"] = "999.0".into();
    r["flash"]["somethingNew"] = true.into();
    let g = read_rec(&r).unwrap();
    assert!(g.coming_soon && g.actions.is_empty() && g.downloads.is_empty());
    assert_eq!(g.name, "Lab Flash");
}

#[test]
fn a_local_release_is_only_trusted_when_mods_info_is_local_too() {
    let mut r = record();
    r["url"] = "http://127.0.0.1:9/someone/lab-coop/releases/download/v1.2.0/LabCoop-1.2.0.zip".into();
    assert!(read_rec(&r).unwrap_err().contains("url must be"));
    let ok = super::community::read_record(&r.to_string(), &shipped_config(), true);
    assert!(ok.is_ok(), "{:?}", ok.map(|g| g.id));
}

#[test]
fn community_records_are_fetched_from_mods_info_at_most_once_an_hour() {
    use super::community::{Cache, load_cache, merge, mods_info_base, refresh_cache, save_cache};
    let dir = TempDir::new("community");
    let cache = dir.join("community.json");
    let s = Server::new();
    let base = s.url("/mods-info/");
    // No community folder yet: no community mods, and that is kept as an answer.
    assert_eq!(refresh_cache(&base, &cache), Ok(true));
    assert!(load_cache(&cache).unwrap().raw.is_empty());
    // Two listed, one missing on the server, the index with a bad id and a repeat.
    s.set("/mods-info/community/index.json", br#"{"mods":["lab-coop","gone","Bad Id","lab-coop"]}"#);
    s.set("/mods-info/community/lab-coop.json", record().to_string().as_bytes());
    let old = load_cache(&cache).unwrap();
    save_cache(&cache, &Cache { fetched_at: old.fetched_at - 3600, raw: old.raw });
    assert_eq!(refresh_cache(&base, &cache), Ok(true));
    let c = load_cache(&cache).unwrap();
    assert_eq!(c.raw.len(), 1);
    assert_eq!(s.hits("/mods-info/community/gone.json"), 1);
    // Under an hour old: mods-info isn't asked again.
    assert_eq!(refresh_cache(&base, &cache), Ok(false));
    assert_eq!(s.hits("/mods-info/community/index.json"), 2, "the 404 and the fetch after it, nothing since");
    // A broken index keeps the last good copy.
    save_cache(&cache, &Cache { fetched_at: c.fetched_at - 3600, raw: c.raw.clone() });
    s.set("/mods-info/community/index.json", b"<html>");
    assert!(refresh_cache(&base, &cache).is_err());
    assert_eq!(load_cache(&cache).unwrap().raw, c.raw);
    assert!(refresh_cache("http://example.com/", &dir.join("x.json")).is_err(), "only https");
    // The merge adds what passes and says why the others were left out.
    let mut cfg = shipped_config();
    let n = cfg.games.len();
    let bad = { let mut r = record(); r["id"] = "other-mod".into(); r.to_string() };
    let left_out = merge(&mut cfg, &Cache { fetched_at: 0, raw: vec![record().to_string(), bad] });
    assert_eq!(cfg.games.len(), n + 1);
    assert_eq!(left_out.len(), 1, "{left_out:?}");
    assert!(mods_info_base(&cfg).ends_with('/'));
}

#[test]
fn website_projects_become_community_cards() {
    use super::site::{mark_community, merge_projects, parse_site, projects_url};
    assert_eq!(projects_url("https://www.multiplayereverything.com/games.json"), "https://www.multiplayereverything.com/api/projects");
    let api = br#"{"games":[
        {"id":"test","kind":"pc","name":"TEST","maintainer":"multiplayereverything","repo":"multiplayereverything/multiplayer-mods-launcher","description":"A test."},
        {"id":"lab-coop","kind":"pc","name":"Lab Game","maintainer":"someone","repo":"someone/lab-coop","description":"Four players in one world.","players":"1-4 players"}
    ],"attach":{}}"#;
    let mut list = parse_site(api).unwrap();
    mark_community(&mut list);
    assert!(list.iter().all(|s| s.community));
    assert_eq!(list[0].author, "multiplayereverything");
    assert_eq!(list[0].author_url, "https://github.com/multiplayereverything/multiplayer-mods-launcher");
    let mut cfg = shipped_config();
    cfg.games.push(read_rec(&record()).unwrap());
    let n = cfg.games.len();
    merge_projects(&mut cfg, &list, "covers");
    // The approved one gets the website's description; the other is a card that can't be installed yet.
    let lab = cfg.games.iter().find(|g| g.id == "lab-coop").unwrap();
    assert_eq!(lab.description, "Four players in one world.");
    assert_eq!(lab.player_count, "1-4 players");
    assert_eq!(cfg.games.len(), n + 1);
    let t = cfg.games.last().unwrap();
    assert!(t.id == "site-test" && t.name == "TEST" && t.coming_soon && t.mod_spec.is_none());
    assert_eq!(t.categories, ["community"]);
    assert_eq!(t.description, "A test.");
}
