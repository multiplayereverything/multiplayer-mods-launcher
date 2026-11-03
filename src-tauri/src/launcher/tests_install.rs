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
    let r = load_config_file(&paths::normalize(&path));
    assert!(r.ok());
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

#[test]
fn a_community_record_that_could_do_more_than_it_shows_is_refused_whole() {
    let mut cases: Vec<(serde_json::Value, &str)> = Vec::new();
    let mut add = |f: &dyn Fn(&mut serde_json::Value), want: &'static str| {
        let mut r = record();
        f(&mut r);
        cases.push((r, want));
    };
    add(&|r| r["extra"] = "x".into(), "record.extra: not allowed");
    add(&|r| r["mod"]["package"] = serde_json::json!({ "github": "x/y" }), "mod.package: not allowed");
    add(&|r| r["mod"]["launch"]["executable"] = "cmd.exe".into(), "mod.launch.executable: not allowed");
    add(&|r| r["mod"]["launch"]["uri"] = "https://example.com".into(), "steam://rungameid");
    add(&|r| r["mod"]["install"]["allowedRoots"] = serde_json::json!(["C:/"]), "allowedRoots: not allowed");
    add(&|r| r["mod"]["install"]["targetDir"] = "C:/Windows".into(), "targetDir must be inside");
    add(&|r| r["mod"]["install"]["targetDir"] = "${GAME_DIR}/../..".into(), "targetDir must be inside");
    add(&|r| r["mod"]["components"][0]["target"] = "%APPDATA%".into(), "target must be inside");
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
