//! The C++ tests for this part (old/tests/test_core.cpp: config, validation, library lookup; test_site.cpp: the
//! website's list and its security rules), ported case for case against real temporary folders.

use super::catalog::{parse_catalog, parse_catalog_settings};
use super::config::{ConfigIssue, LoadResult, Severity, load_config, load_config_file};
use super::embedded::{embedded_name, read_embedded};
use super::library::{ActionState, Library};
use super::paths;
use super::settings::UserData;
use super::site::*;
use super::vars::Vars;

/// A scratch folder under %TEMP%\fl-tests, removed when the test ends.
pub struct TempDir(String);

impl TempDir {
    pub fn new(name: &str) -> Self {
        static COUNTER: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        let n = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let p = paths::join(&paths::temp_dir(), &format!("fl-tests\\rs-{name}-{}-{n}", std::process::id()));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        TempDir(p)
    }
    pub fn path(&self) -> &str {
        &self.0
    }
    pub fn join(&self, rel: &str) -> String {
        paths::join(&self.0, rel)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn write_text(path: &str, text: &str) {
    paths::ensure_dir(&paths::parent(path));
    std::fs::write(path, text).unwrap();
}

fn minimal_config(games: &str, players: Option<&str>) -> String {
    let p = players.unwrap_or(
        r#"[{ "id": "ruffle", "paths": ["${PLAYERS_DIR}\\ruffle.exe"] },
            { "id": "flash", "paths": ["${PLAYERS_DIR}\\flash*.exe"], "args": ["-x"] }]"#,
    );
    format!(
        r#"{{ "paths": {{ "games": "${{LAUNCHER_DIR}}\\games", "players": "${{LAUNCHER_DIR}}\\players",
                          "data": "${{LAUNCHER_DIR}}\\data", "flashpoint": ["${{LAUNCHER_DIR}}\\fp"] }},
              "categories": [{{ "id": "online" }}],
              "players": {p}, "games": {games}}}"#
    )
}

fn load(text: &str, dir: &str) -> LoadResult {
    load_config(text.as_bytes(), dir)
}

fn has_issue(r: &LoadResult, place: &str, sev: Severity) -> bool {
    r.issues.iter().any(|i: &ConfigIssue| i.severity == sev && i.place.contains(place))
}

fn base_vars(dir: &TempDir) -> Vars {
    let mut v = Vars::default();
    v.set("LAUNCHER_DIR", dir.path());
    v
}

fn shipped() -> LoadResult {
    let path = format!("{}\\..\\launcher\\launcher.json", env!("CARGO_MANIFEST_DIR"));
    let r = load_config_file(&paths::normalize(&path));
    assert!(r.ok(), "{}", super::config::format_issues(&r.issues));
    r
}

#[test]
fn config_loads_shipped_launcher_json() {
    // The real config must stay valid: it is what users run.
    let r = shipped();
    assert!(r.issues.is_empty(), "{}", super::config::format_issues(&r.issues));
    assert!(r.config.find_game("gunmayhem2").is_some());
    assert!(r.config.find_game("stealthhunter2").is_some());
    // Gun Mayhem 2 online is Ruffle Together's lockstep co-op: options on the command line, no relay of ours, and
    // no FlashVars (every PC must run with identical settings).
    let gm2 = r.config.find_game("gunmayhem2").unwrap();
    let (mut online, mut host4) = (false, false);
    for a in &gm2.actions {
        assert!(!a.relay.enabled && a.flashvars.is_empty());
        if a.id == "online" {
            online = !a.join_args.is_empty() && a.args.len() > 1 && a.args[0] == "--coop-host-relay" && a.join_args.iter().any(|x| x == "${CODE}");
        }
        if a.id == "host4" {
            host4 = a.args.iter().any(|x| x == "--coop-players") && a.args.last().map(String::as_str) == Some("4");
        }
    }
    assert!(online && host4);
}

#[test]
fn config_reports_typos_and_bad_references() {
    let r = load(
        &minimal_config(
            r#"[
        { "id": "a", "nmae": "typo", "categories": ["nope"],
          "actions": [{ "id": "play", "swf": "x.swf", "players": ["ghost"] }] },
        { "id": "a", "actions": [] }
    ]"#,
            None,
        ),
        "C:\\x",
    );
    assert!(!r.ok());
    assert!(has_issue(&r, "games[0].nmae", Severity::Warning));
    assert!(has_issue(&r, "games[0].categories", Severity::Error));
    assert!(has_issue(&r, "games[0].actions[0].players", Severity::Error));
    assert!(has_issue(&r, "games[1].id", Severity::Error)); // duplicate
    assert!(has_issue(&r, "games[1].actions", Severity::Error)); // no actions
}

#[test]
fn config_rejects_bad_json_with_position() {
    let r = load("{ \"games\": [ }", "C:\\x");
    assert!(!r.ok());
    assert!(r.issues[0].message.contains("line"));
}

#[test]
fn config_text_substitution() {
    let r = load(&minimal_config(r#"[{ "id": "g", "actions": [{ "id": "p", "swf": "a.swf" }] }]"#, None), "C:\\x");
    assert!(r.ok());
    assert!(r.config.t("bar.found", &[("PLAYER", "Ruffle")]).starts_with("Ruffle"));
}

#[test]
fn library_uses_join_args_when_a_room_code_comes_with_the_launch() {
    let dir = TempDir::new("lib-join-args");
    write_text(&dir.join("players\\ruffle.exe"), "x");
    write_text(&dir.join("games\\game.swf"), "x");
    let r = load(
        &minimal_config(
            r#"[{ "id": "g", "name": "Game",
        "actions": [{ "id": "online", "swf": ["${GAMES_DIR}\\game.swf"],
                      "args": ["--host", "${SERVER}"], "joinArgs": ["--join", "${SERVER}", "--code", "${CODE}"] }] }]"#,
            None,
        ),
        dir.path(),
    );
    assert!(r.ok());
    let lib = Library::new(&r.config, base_vars(&dir));
    let user = UserData::default();
    let g = &r.config.games[0];
    let host = lib.resolve(g, &g.actions[0], &user, false);
    assert!(host.state == ActionState::Ready && host.args.len() == 3);
    assert_eq!(host.args[0], "--host");
    assert_eq!(host.args[1], "${SERVER}"); // filled in by the launcher at start
    let join = lib.resolve(g, &g.actions[0], &user, true);
    assert_eq!(join.args.len(), 5);
    assert_eq!(join.args[0], "--join");
    assert_eq!(join.args[3], "${CODE}");
    assert_eq!(join.args[4], join.swf);
}

#[test]
fn library_resolves_player_swf_and_args() {
    let dir = TempDir::new("lib-resolve");
    write_text(&dir.join("players\\ruffle.exe"), "x");
    write_text(&dir.join("games\\game.swf"), "x");
    let r = load(
        &minimal_config(
            r#"[{ "id": "g", "name": "Game",
        "actions": [{ "id": "host", "swf": ["${GAMES_DIR}\\missing.swf", "${GAMES_DIR}\\game.swf"],
                      "overrides": { "ruffle": { "args": ["--tcp-connections", "allow"] } },
                      "relay": { "port": 7777 } }] }]"#,
            None,
        ),
        dir.path(),
    );
    assert!(r.ok());
    let lib = Library::new(&r.config, base_vars(&dir));
    let a = lib.resolve(&r.config.games[0], &r.config.games[0].actions[0], &UserData::default(), false);
    assert_eq!(a.state, ActionState::Ready);
    assert_eq!(a.player.unwrap().id, "ruffle");
    assert_eq!(paths::file_name(&a.swf), "game.swf");
    assert_eq!(a.args.len(), 3); // override args, then the SWF appended
    assert_eq!(a.args[0], "--tcp-connections");
    assert_eq!(a.args[2], a.swf);
    assert_eq!(paths::normalize(&a.work_dir), paths::normalize(&dir.join("games")));
}

#[test]
fn library_falls_back_and_honours_preferred_player() {
    let dir = TempDir::new("lib-fallback");
    write_text(&dir.join("players\\flash_32_sa.exe"), "x");
    write_text(&dir.join("players\\flash_9_sa.exe"), "x");
    write_text(&dir.join("games\\game.swf"), "x");
    let r = load(&minimal_config(r#"[{ "id": "g", "actions": [{ "id": "p", "swf": "${GAMES_DIR}\\game.swf" }] }]"#, None), dir.path());
    assert!(r.ok());
    let mut lib = Library::new(&r.config, base_vars(&dir));
    let mut user = UserData::default();
    let g = &r.config.games[0];
    let a = lib.resolve(g, &g.actions[0], &user, false);
    assert_eq!(a.state, ActionState::Ready);
    assert_eq!(a.player.unwrap().id, "flash"); // Ruffle missing -> next player
    assert_eq!(paths::file_name(&a.player_exe), "flash_32_sa.exe"); // natural order: 32 > 9
    assert_eq!(a.args.len(), 2);
    assert_eq!(a.args[0], "-x");

    write_text(&dir.join("players\\ruffle.exe"), "x");
    lib.refresh();
    assert_eq!(lib.resolve(g, &g.actions[0], &user, false).player.unwrap().id, "ruffle");
    user.stats("g").player = "flash".into();
    assert_eq!(lib.resolve(g, &g.actions[0], &user, false).player.unwrap().id, "flash");
}

#[test]
fn library_reports_missing_pieces() {
    let dir = TempDir::new("lib-missing");
    let r = load(&minimal_config(r#"[{ "id": "g", "actions": [{ "id": "p", "swf": "${GAMES_DIR}\\game.swf" }] }]"#, None), dir.path());
    assert!(r.ok());
    let lib = Library::new(&r.config, base_vars(&dir));
    let user = UserData::default();
    let g = &r.config.games[0];
    assert_eq!(lib.resolve(g, &g.actions[0], &user, false).state, ActionState::NoSwf);
    write_text(&dir.join("games\\game.swf"), "x");
    assert_eq!(lib.resolve(g, &g.actions[0], &user, false).state, ActionState::NoPlayer);
    assert_eq!(lib.game_state(g, &user), ActionState::NoPlayer);
}

#[test]
fn library_uses_located_file_and_skips_missing_flashpoint() {
    let dir = TempDir::new("lib-located");
    write_text(&dir.join("players\\ruffle.exe"), "x");
    write_text(&dir.join("elsewhere\\my game.swf"), "x");
    let r = load(
        &minimal_config(r#"[{ "id": "g", "actions": [{ "id": "p", "swf": ["${FLASHPOINT}\\game.swf", "${GAMES_DIR}\\game.swf"] }] }]"#, None),
        dir.path(),
    );
    assert!(r.ok());
    let mut lib = Library::new(&r.config, base_vars(&dir));
    assert!(lib.flashpoint_dir().is_empty());
    let mut user = UserData::default();
    let g = &r.config.games[0];
    assert_eq!(lib.resolve(g, &g.actions[0], &user, false).state, ActionState::NoSwf);
    user.stats("g").swf.insert("p".into(), dir.join("elsewhere\\my game.swf"));
    let a = lib.resolve(g, &g.actions[0], &user, false);
    assert_eq!(a.state, ActionState::Ready);
    assert!(a.user_swf);

    // With a Flashpoint folder present, ${FLASHPOINT} candidates count.
    write_text(&dir.join("fp\\FPSoftware\\.keep"), "");
    write_text(&dir.join("fp\\game.swf"), "x");
    lib.refresh();
    assert_eq!(paths::normalize(&lib.flashpoint_dir()), paths::normalize(&dir.join("fp")));
    user.stats("g").swf.clear();
    assert_eq!(paths::normalize(&lib.resolve(g, &g.actions[0], &user, false).swf), paths::normalize(&dir.join("fp\\game.swf")));
}

#[test]
fn library_passes_urls_through() {
    let dir = TempDir::new("lib-url");
    write_text(&dir.join("players\\ruffle.exe"), "x");
    let r = load(&minimal_config(r#"[{ "id": "g", "actions": [{ "id": "p", "swf": "http://localflash/x/game.swf" }] }]"#, None), dir.path());
    assert!(r.ok());
    let lib = Library::new(&r.config, base_vars(&dir));
    let a = lib.resolve(&r.config.games[0], &r.config.games[0].actions[0], &UserData::default(), false);
    assert_eq!(a.state, ActionState::Ready);
    assert!(a.swf_is_url);
    assert_eq!(a.args.last().map(String::as_str), Some("http://localflash/x/game.swf"));
}

#[test]
fn config_player_tv_block() {
    let r = load(
        &minimal_config(
            r#"[{ "id": "g", "actions": [{ "id": "p", "swf": "a.swf" }] }]"#,
            Some(r#"[{ "id": "ruffle", "paths": ["r.exe"], "tv": { "args": ["--fullscreen"], "env": { "RUFFLE_TOGETHER_TV": "1" } } }]"#),
        ),
        "C:\\x",
    );
    assert!(r.ok());
    assert!(r.issues.is_empty());
    let p = &r.config.players[0];
    assert_eq!(p.tv_args, vec!["--fullscreen".to_string()]);
    assert_eq!(p.tv_env, vec![("RUFFLE_TOGETHER_TV".to_string(), "1".to_string())]);

    let bad = load(
        &minimal_config(
            r#"[{ "id": "g", "actions": [{ "id": "p", "swf": "a.swf" }] }]"#,
            Some(r#"[{ "id": "ruffle", "paths": ["r.exe"], "tv": { "env": { "X": 1 }, "argz": [] } }]"#),
        ),
        "C:\\x",
    );
    assert!(has_issue(&bad, "players[0].tv.env", Severity::Error));
    assert!(has_issue(&bad, "players[0].tv.argz", Severity::Warning));
}

#[test]
fn catalog_parsing_and_unsafe_names() {
    let json = r#"{ "schemaVersion": 1, "files": [
        { "name": "gm2.swf", "game": "gunmayhem2", "path": "games/gunmayhem2/gm2.swf", "size": 10, "sha256": "ABCDEF" },
        { "name": "../evil.swf", "game": "x", "path": "games/x/evil.swf", "size": 1, "sha256": "00" },
        { "name": "sub/evil.swf", "game": "x", "path": "games/x/evil.swf", "size": 1, "sha256": "00" },
        { "name": "abs.swf", "game": "y", "path": "https://example.org/abs.swf", "size": 2, "sha256": "11" }
    ] }"#;
    let files = parse_catalog(json.as_bytes(), "https://raw.githubusercontent.com/o/r/main/catalog.json").unwrap();
    assert_eq!(files.len(), 2); // names that could escape the games folder are dropped
    assert_eq!(files[0].url, "https://raw.githubusercontent.com/o/r/main/games/gunmayhem2/gm2.swf");
    assert_eq!(files[0].sha256, "abcdef");
    assert_eq!(files[0].size, 10);
    assert_eq!(files[1].url, "https://example.org/abs.swf");
    assert!(parse_catalog(b"{ nope", "https://x/catalog.json").is_err());
}

#[test]
fn library_blames_the_missing_file_when_a_player_is_installed() {
    // Ruffle is installed but its file is missing; Flash Player's copy is a web address. The fix is the file, so
    // the state must say so.
    let dir = TempDir::new("lib-blame");
    write_text(&dir.join("players\\ruffle.exe"), "x");
    let r = load(
        &minimal_config(
            r#"[{ "id": "g", "actions": [{ "id": "p", "swf": "${GAMES_DIR}\\g.swf",
        "overrides": { "flash": { "swf": ["http://localflash/g.swf"] } } }] }]"#,
            None,
        ),
        dir.path(),
    );
    assert!(r.ok());
    let lib = Library::new(&r.config, base_vars(&dir));
    let a = lib.resolve(&r.config.games[0], &r.config.games[0].actions[0], &UserData::default(), false);
    assert_eq!(a.state, ActionState::NoSwf);
    assert_eq!(a.player.map(|p| p.id.as_str()), Some("ruffle"));
}

#[test]
fn config_lobby_block() {
    let r = load(
        &minimal_config(
            r#"[
        { "id": "a", "online": { "id": "gm2", "join": "p", "lobby": { "version": "1.3", "port": 7800 } },
          "actions": [{ "id": "p", "swf": "a.swf" }] },
        { "id": "b", "online": { "id": "sh2", "join": "p", "lobby": { "enabled": false } }, "actions": [{ "id": "p", "swf": "b.swf" }] },
        { "id": "c", "actions": [{ "id": "p", "swf": "c.swf" }] }
    ]"#,
            None,
        ),
        "C:\\x",
    );
    assert!(r.ok());
    let g = &r.config.games;
    assert!(g[0].lobby.enabled && g[0].lobby.version == "1.3" && g[0].lobby.port == 7800);
    assert!(!g[1].lobby.enabled);
    assert!(!g[2].lobby.enabled && g[2].lobby.port == 7790);
    let bad = load(&minimal_config(r#"[{ "id": "a", "online": { "join": "p", "lobby": {} }, "actions": [{ "id": "p", "swf": "a.swf" }] }]"#, None), "C:\\x");
    assert!(!bad.ok());
}

#[test]
fn html5_action_resolves_to_a_browser() {
    let dir = TempDir::new("lib-web");
    write_text(&dir.join("players\\edge.exe"), "x");
    write_text(&dir.join("players\\ruffle.exe"), "x");
    write_text(&dir.join("mod\\server\\server.js"), "x");
    write_text(&dir.join("node\\node.exe"), "x");
    let players = r#"[{ "id": "ruffle", "paths": ["${PLAYERS_DIR}\\ruffle.exe"] },
        { "id": "edge", "kind": "browser", "paths": ["${PLAYERS_DIR}\\edge.exe"], "args": ["--app=${URL}"] }]"#;
    let r = load(
        &minimal_config(
            r#"[{ "id": "pb2", "actions": [
        { "id": "host", "file": "${LAUNCHER_DIR}\\mod\\server\\server.js",
          "server": { "name": "Node.js", "exe": ["${FILE_DIR}\\..\\..\\node\\node.exe"], "args": ["server\\server.js", "2053"],
                      "cwd": "${FILE_DIR}\\..", "url": "http://localhost:2053/", "share": "https://[a-z]+\\.example" } },
        { "id": "join", "joinLink": true },
        { "id": "page", "page": "${LAUNCHER_DIR}\\web\\index.html" } ] }]"#,
            Some(players),
        ),
        dir.path(),
    );
    assert!(r.ok(), "{}", super::config::format_issues(&r.issues));
    let lib = Library::new(&r.config, base_vars(&dir));
    let user = UserData::default();
    let g = &r.config.games[0];
    let host = lib.resolve(g, &g.actions[0], &user, false);
    assert_eq!(host.state, ActionState::Ready);
    assert!(host.web && host.player.unwrap().id == "edge"); // never Ruffle for an HTML5 action
    assert_eq!(paths::file_name(&host.server_exe), "node.exe");
    assert_eq!(paths::normalize(&host.server_cwd), paths::normalize(&dir.join("mod")));
    assert_eq!(host.url, "http://localhost:2053/");
    assert_eq!(host.args, vec!["--app=${URL}".to_string()]); // filled in when the game starts
    assert_eq!(lib.resolve(g, &g.actions[1], &user, false).state, ActionState::Ready);
    assert_eq!(lib.resolve(g, &g.actions[2], &user, false).state, ActionState::NoSwf); // no index.html yet
    write_text(&dir.join("web\\index.html"), "<p>hi</p>");
    let page = lib.resolve(g, &g.actions[2], &user, false);
    assert!(page.state == ActionState::Ready && page.page_local);

    std::fs::remove_file(dir.join("node\\node.exe")).unwrap();
    assert_eq!(lib.resolve(g, &g.actions[0], &user, false).state, ActionState::NoRuntime);
}

#[test]
fn config_rejects_html5_action_with_flash_player() {
    let r = load(
        &minimal_config(
            r#"[{ "id": "g", "actions": [ { "id": "p", "page": "https://x.example/", "players": ["ruffle"] } ] }]"#,
            Some(r#"[{ "id": "ruffle", "paths": ["r.exe"] }, { "id": "edge", "kind": "browser", "paths": ["e.exe"] }]"#),
        ),
        "C:\\x",
    );
    assert!(!r.ok());
}

#[test]
fn online_first_action_options_parse() {
    let r = load(
        &minimal_config(
            r#"[{ "id": "g", "online": { "id": "g1", "join": "online" }, "actions": [
        { "id": "online", "swf": "a.swf", "flashvars": { "online": "1", "name": "${PLAYER_NAME}" } },
        { "id": "host", "swf": "a.swf", "more": true },
        { "id": "play", "swf": "b.swf", "edition": "original" } ] }]"#,
            None,
        ),
        "C:\\x",
    );
    assert!(r.ok());
    let a = &r.config.games[0].actions;
    assert_eq!(a[0].flashvars.len(), 2);
    assert!(a[0].flashvars[0].0 == "online" && a[0].flashvars[1].1 == "${PLAYER_NAME}");
    assert!(!a[0].more && a[1].more && !a[1].original && a[2].original);
    assert_eq!(r.config.games[0].join_action, "online");
    assert!(r.issues.iter().all(|i| !i.message.contains("unknown"))); // PLAYER_NAME is known
}

#[test]
fn category_icons_and_discord_are_checked() {
    let base = minimal_config(r#"[{ "id": "g", "actions": [ { "id": "p", "swf": "a.swf" } ] }]"#, None);
    // `cats` replaces the helper's own category list; `extra` goes in front.
    let with = |cats: &str, extra: &str| {
        let mine = r#""categories": [{ "id": "online" }]"#;
        let j = base.replace(mine, &format!("\"categories\": {cats}"));
        let k = j.find('{').unwrap() + 1;
        load(&format!("{}{extra}{}", &j[..k], &j[k..]), "C:\\x")
    };
    let ok = with(r#"[ { "id": "online", "icon": "E774" } ]"#, r#""discord": { "clientId": "1234567890" },"#);
    assert!(ok.ok(), "{}", super::config::format_issues(&ok.issues));
    assert_eq!(ok.config.categories[0].icon, "\u{E774}");
    assert_eq!(ok.config.discord_client_id, "1234567890");
    assert!(!with(r#"[ { "id": "online", "icon": "star" } ]"#, "").ok());
    assert!(!with(r#"[ { "id": "online" } ]"#, r#""discord": { "clientId": "abc" },"#).ok());
}

#[test]
fn embedded_names_match_the_resource_script() {
    // build.rs names the files the way the C++ CMakeLists.txt names its resources.
    assert_eq!(embedded_name("launcher.json"), "F_LAUNCHER_JSON");
    assert_eq!(embedded_name("covers\\gunmayhem2.png"), "F_COVERS_GUNMAYHEM2_PNG");
    assert_eq!(embedded_name("covers/Pb2.jpg"), "F_COVERS_PB2_JPG");
    assert!(read_embedded("covers\\nope.png").is_none());
    // Unlike the C++ test exe, the app carries its files, and the built-in launcher.json is the shipped one.
    let shipped = std::fs::read(format!("{}\\..\\launcher\\launcher.json", env!("CARGO_MANIFEST_DIR"))).unwrap();
    assert_eq!(read_embedded("launcher.json"), Some(shipped.as_slice()));
}

#[test]
fn shipped_config_keeps_stealth_hunter_2_on_ruffle() {
    // Flash Player is never an option for Stealth Hunter 2: its online actions are Ruffle Together's own options,
    // which only that player understands.
    let r = shipped();
    let g = r.config.find_game("stealthhunter2").unwrap();
    for a in &g.actions {
        let coop = a.args.iter().any(|x| x == "--coop-keys");
        assert!(coop || a.original || a.offline);
        assert!(!a.overrides.contains_key("flash"));
    }
    assert!(r.config.updates_enabled());
}

#[test]
fn shipped_config_gives_dad_n_me_a_friendly_fire_host_option() {
    let r = shipped();
    let g = r.config.find_game("dadnme").unwrap();
    assert_eq!(g.settings.len(), 1);
    let s = &g.settings[0];
    assert!(s.id == "friendlyfire" && s.kind == "checkbox" && s.scope == "host" && s.since == "1.0.3.7");
    assert!(s.args.len() == 2 && s.args[1] == "friendlyfire=1");
}

#[test]
fn shipped_config_gives_zombotron_four_host_options_and_up_to_four_players() {
    let r = shipped();
    let g = r.config.find_game("zombotron").unwrap();
    assert_eq!(g.max_players, 4);
    assert_eq!(g.settings.len(), 4);
    assert!(g.settings[0].id == "camera" && g.settings[0].kind == "choice" && g.settings[0].choices.len() == 2);
    assert!(g.settings[1].id == "zoom" && g.settings[1].choices.len() == 3);
    assert!(g.settings[2].id == "friendlyfire" && g.settings[3].id == "bump");
}

#[test]
fn catalog_settings_accept_the_live_dad_n_me_camera_choice() {
    let out = parse_catalog_settings(
        br#"{ "files": [], "settings": { "dadnme": [
      { "id": "friendlyfire", "type": "checkbox", "scope": "host", "label": "Friendly fire", "since": "1.0.3.7", "args": ["--coop-option", "friendlyfire=1"] },
      { "id": "camera", "type": "choice", "scope": "host", "label": "Camera", "since": "1.0.3.7", "default": "shared", "choices": [
        { "id": "shared", "label": "Shared screen", "args": [] },
        { "id": "follow", "label": "Follow each player", "args": ["--coop-option", "camera=follow"] },
        { "id": "zoom", "label": "Follow each player, zoomed in", "args": ["--coop-option", "camera=zoom"] } ] } ] } }"#,
    );
    assert_eq!(out["dadnme"].len(), 2);
    let cam = &out["dadnme"][1];
    assert!(cam.kind == "choice" && cam.def == "shared" && cam.choices.len() == 3);
    assert!(cam.choices[0].args.is_empty());
    assert!(cam.choices[2].args.len() == 2 && cam.choices[2].args[1] == "camera=zoom");
}

#[test]
fn shipped_config_keeps_chaos_faction_2_to_six_players_on_its_key_map() {
    let r = shipped();
    let g = r.config.find_game("chaosfaction2").unwrap();
    assert_eq!(g.max_players, 6);
    let mut rooms = 0;
    for a in &g.actions {
        if a.id.starts_with("room") || (a.id.starts_with("host") && a.id.len() > 4) {
            rooms += 1;
        }
        let mut keyed = false;
        for w in a.args.windows(2) {
            if w[0] == "--coop-keys" {
                keyed = w[1] == "chaos-faction-2";
            }
        }
        if !a.args.is_empty() {
            assert!(keyed, "{}", a.id);
        }
    }
    assert_eq!(rooms, 8); // a room and an IP host for 3, 4, 5 and 6 players
}

#[test]
fn catalog_settings_checkboxes_and_choices() {
    let out = parse_catalog_settings(
        br#"{ "files": [], "settings": {
        "dadnme": [
          { "id": "ff", "label": "Friendly fire", "since": "1.0.3.7", "args": ["--coop-option", "friendlyfire=1"] },
          { "id": "diff", "type": "choice", "label": "Difficulty", "default": "hard", "scope": "all",
            "choices": [ { "id": "easy", "label": "Easy", "args": ["--coop-option", "d=0"] },
                         { "id": "hard", "label": "Hard", "args": ["--coop-option", "d=2"] } ] },
          { "id": "bad", "type": "choice", "label": "One choice", "choices": [ { "id": "a", "label": "A" } ] },
          { "id": "bad2", "label": "No args" }
        ],
        "other": 5 } }"#,
    );
    assert_eq!(out["dadnme"].len(), 2); // the two broken ones are left out
    assert_eq!(out["dadnme"][0].scope, "host");
    assert!(out["dadnme"][1].kind == "choice" && out["dadnme"][1].choices.len() == 2 && out["dadnme"][1].def == "hard");
    assert!(!out.contains_key("other"));
    assert!(parse_catalog_settings(b"not json").is_empty());
}

// ---- the website's games.json (test_site.cpp) ----

const SITE: &str = r#"{
  "games": [
    { "id": "gunmayhem2", "kind": "flash", "modId": "gm2", "name": "Gun Mayhem 2", "status": "available", "quality": "B",
      "version": "1", "updated": "2026-10-24", "connect": "Hosted relay", "maintainer": "Multiplayer Everything",
      "synced": { "see": "synced", "story": "unknown" } },
    { "id": "baldi", "kind": "pc", "modId": "baldi", "name": "Baldi Together", "status": "testing", "quality": "",
      "version": "0.9.1", "synced": { "see": "synced", "story": "partly" },
      "progress": { "lab": "done", "release": "none" }, "somethingNew": [1, 2] },
    { "name": "no id, skipped" },
    "not an object"
  ]
}"#;

#[test]
fn site_json_is_parsed_and_unknown_fields_are_ignored() {
    let list = parse_site(SITE.as_bytes()).unwrap();
    assert_eq!(list.len(), 2); // the entry without an id and the non-object are skipped
    assert!(list[0].id == "gunmayhem2" && list[0].mod_id == "gm2" && list[0].kind == "flash");
    assert!(list[0].status == "available" && list[0].quality == "B" && list[0].version == "1");
    assert!(list[0].connect == "Hosted relay" && list[0].updated == "2026-10-24");
    assert!(list[0].synced["see"] == "synced" && list[0].synced["story"] == "unknown");
    assert!(list[1].quality.is_empty() && list[1].synced["story"] == "partly");
    assert_eq!(list[1].progress["lab"], "done");
    assert!(parse_site(b"not json").is_none());
    assert!(parse_site(b"{ \"other\": [] }").is_none());
}

#[test]
fn a_game_finds_its_site_entry_by_site_id_then_id_then_mod_id() {
    let list = parse_site(SITE.as_bytes()).unwrap();
    assert_eq!(find_site(&list, "", "gunmayhem2", "gm2").map(|k| list[k].id.as_str()), Some("gunmayhem2"));
    assert_eq!(find_site(&list, "", "something-else", "gm2").map(|k| list[k].id.as_str()), Some("gunmayhem2"));
    assert_eq!(find_site(&list, "baldi", "baldi-together", "").map(|k| list[k].id.as_str()), Some("baldi"));
    assert!(find_site(&list, "", "baldi-together", "").is_none()); // no guessing from similar names
    assert!(find_site(&list, "", "", "").is_none());
}

#[test]
fn the_site_copy_on_disk_round_trips_and_goes_stale_after_an_hour() {
    let dir = TempDir::new("site");
    let file = dir.join("site.json");
    assert!(load_site_cache(&file).is_none());
    assert!(save_site_cache(&file, &SiteCache { fetched_at: 1000000, raw: SITE.into() }));
    let back = load_site_cache(&file).unwrap();
    assert!(back.fetched_at == 1000000 && back.raw == SITE);
    assert!(back.fresh(1000000 + 3599, 3600) && !back.fresh(1000000 + 3600, 3600) && !back.fresh(999999, 3600));
    assert!(!SiteCache { fetched_at: 0, raw: String::new() }.fresh(5, 3600));
    write_text(&file, "{ broken");
    assert!(load_site_cache(&file).is_none());
}

const SITE_PATHS: &str = r#""paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
    "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ]"#;

#[test]
fn the_site_address_and_a_games_site_id_come_from_launcher_json() {
    let r = load(
        &format!(
            r#"{{ {SITE_PATHS}, "downloads": {{ "site": "https://example.com/games.json" }},
        "games": [ {{ "id": "g", "name": "G", "site": "gee", "actions": [ {{ "id": "p", "swf": "a.swf" }} ] }} ] }}"#
        ),
        "C:\\x",
    );
    assert!(r.ok());
    assert!(r.config.downloads.site == "https://example.com/games.json" && r.config.games[0].site_id == "gee");
    let def = load(&format!(r#"{{ {SITE_PATHS}, "games": [ {{ "id": "g", "name": "G", "actions": [ {{ "id": "p", "swf": "a.swf" }} ] }} ] }}"#), "C:\\x");
    assert!(def.ok());
    assert_eq!(def.config.downloads.site, "https://www.multiplayereverything.com/games.json");
    let bad = load(&format!(r#"{{ {SITE_PATHS}, "downloads": {{ "site": "http://example.com/games.json" }}, "games": [] }}"#), "C:\\x");
    assert!(!bad.ok());
}

#[test]
fn games_the_launcher_lacks_become_coming_soon_and_the_site_fills_in_what_is_missing() {
    let json = r##"{ "games": [
      { "id": "native", "name": "Native", "kind": "flash", "status": "available", "quality": "B", "players": "1-4 players",
        "slots": 4, "description": "From the site.", "cover": "covers/native.png", "coverFocus": 0.25 },
      { "id": "planned", "name": "Planned Game", "kind": "pc", "status": "testing", "players": "2-4 players", "slots": 4,
        "description": "A mod in the works.", "color": "#112233", "colorEnd": "#445566", "cover": "covers/planned.png" },
      { "id": "bare", "name": "Bare", "kind": "flash", "status": "coming-soon" }
    ] }"##;
    let list = parse_site(json.as_bytes()).unwrap();
    let r = load(
        &format!(
            r#"{{ {SITE_PATHS},
        "games": [ {{ "id": "native", "name": "Native", "actions": [ {{ "id": "p", "swf": "a.swf" }} ] }},
                   {{ "id": "own", "name": "Has everything", "description": "Mine.", "players": "2 players", "maxPlayers": 2,
                     "image": "mine.png", "actions": [ {{ "id": "p", "swf": "b.swf" }} ] }} ] }}"#
        ),
        "C:\\x",
    );
    assert!(r.ok());
    let mut cfg = r.config.clone();
    merge_site(&mut cfg, &list, "covers");
    assert_eq!(cfg.games.len(), 4); // two of its own, and the two the site lists that it lacks
    // A game that leaves things out gets the site's.
    assert!(cfg.games[0].description == "From the site." && cfg.games[0].player_count == "1-4 players");
    assert!(cfg.games[0].max_players == 4 && cfg.games[0].image.contains("site-native.png"));
    assert_eq!(cfg.games[0].image_focus, 0.25);
    // One that says it all keeps its own words.
    assert!(cfg.games[1].description == "Mine." && cfg.games[1].image == "mine.png" && cfg.games[1].max_players == 2);
    // The rest become "Coming soon" cards, whatever stage the site gives them.
    let planned = cfg.find_game("site-planned").unwrap();
    assert!(planned.coming_soon && planned.actions.is_empty() && planned.name == "Planned Game");
    assert!(planned.description == "A mod in the works." && planned.player_count == "2-4 players" && planned.max_players == 4);
    assert!(planned.color.r == 0x11 && planned.color_end.b == 0x66);
    assert_eq!(planned.categories, vec!["pc".to_string()]);
    assert!(planned.image.contains("site-planned.png"));
    let bare = cfg.find_game("site-bare").unwrap();
    assert!(bare.coming_soon && !bare.description.is_empty() && bare.image.is_empty() && bare.categories[0] == "flash");
    // A launcher game's own `site` id claims its entry, so the site's entry is not added again.
    let mut again = r.config.clone();
    again.games[0].site_id = "planned".into();
    merge_site(&mut again, &list, "covers");
    assert!(again.find_game("site-planned").is_none());
    assert!(again.find_game("site-native").is_some()); // the entry the native game no longer claims is listed instead
}

#[test]
fn the_website_cannot_steer_a_file_name_or_a_link_the_launcher_uses() {
    let json = r#"{ "games": [
      { "id": "../escape", "name": "Bad id" },
      { "id": "ok-game_1", "name": "Fine", "repo": "owner/name.v2", "cover": "covers/fine.png" },
      { "id": "has space", "name": "Space" },
      { "id": "x", "name": "Bad repo", "repo": "owner/name/../../other" },
      { "id": "y", "name": "Bad repo 2", "repo": "owner/na me?x=1" },
      { "id": "z", "name": "Bad covers", "cover": "../covers/z.png" },
      { "id": "w", "name": "Other covers", "cover": "https://evil.example/w.png" },
      { "id": "v", "name": "Wrong type", "cover": "covers/v.exe" }
    ] }"#;
    let list = parse_site(json.as_bytes()).unwrap();
    assert_eq!(list.len(), 6); // the two with a bad id are dropped
    assert!(list[0].id == "ok-game_1" && list[0].repo == "owner/name.v2" && list[0].cover == "covers/fine.png");
    for s in &list[1..] {
        assert!(s.repo.is_empty()); // a bad repo is never kept
        assert!(s.cover.is_empty()); // nor a cover outside covers/<name>.png|jpg
    }
    assert!(is_github_repo("owner/name") && is_github_repo("a-b_c/d.e") && !is_github_repo("owner") && !is_github_repo("a/b/c"));
    assert!(!is_github_repo("../x") && !is_github_repo("a/..") && !is_github_repo("a/b c") && !is_github_repo(""));
    assert!(is_site_cover("covers/x.png") && is_site_cover("covers/Some-Game_2.JPG") && !is_site_cover("covers/"));
    assert!(!is_site_cover("covers/a/b.png") && !is_site_cover("covers/..png") && !is_site_cover("x/covers/a.png"));
    // The cover file always sits directly in the covers folder.
    for s in &list {
        let file = site_cover_file("covers", s);
        assert!(!file.contains("..") && !file.contains('/'));
    }
}

#[test]
fn community_mods_are_marked_unverified_and_their_links_stay_https_github() {
    let json = r#"{ "games": [
      { "id": "a", "name": "Ours" },
      { "id": "b", "name": "Theirs", "origin": "community", "author": "Sam", "authorUrl": "https://github.com/sam/mod" },
      { "id": "c", "name": "Checked", "origin": "community", "reviewed": true, "authorUrl": "http://github.com/sam/mod" },
      { "id": "d", "name": "Odd", "origin": "whatever", "authorUrl": "https://evil.example/sam/mod" },
      { "id": "e", "name": "Explicit", "origin": "official", "reviewed": "yes" }
    ] }"#;
    let list = parse_site(json.as_bytes()).unwrap();
    assert_eq!(list.len(), 5);
    assert!(!list[0].community); // no origin: official
    assert!(list[1].community && !list[1].reviewed && list[1].author == "Sam" && list[1].author_url == "https://github.com/sam/mod");
    assert!(list[2].community && list[2].reviewed && list[2].author_url.is_empty()); // http is dropped
    assert!(list[3].community && !list[3].reviewed && list[3].author_url.is_empty()); // an unknown origin reads as community
    assert!(!list[4].community && !list[4].reviewed); // reviewed must be a real boolean
    assert!(is_github_repo_url("https://github.com/a/b") && !is_github_repo_url("https://github.com/a/b/c"));
    assert!(!is_github_repo_url("https://github.com.evil/a/b") && !is_github_repo_url("https://github.com/../x"));
}

#[test]
fn site_covers_accept_png_jpg_and_webp_only() {
    assert!(is_site_cover("covers/zombotron.webp"));
    assert!(is_site_cover("covers/Bowman.PNG"));
    assert!(is_site_cover("covers/raze.jpg"));
    assert!(!is_site_cover("covers/x.gif"));
    assert!(!is_site_cover("covers/../x.webp"));
    assert!(!is_site_cover("other/x.webp"));
}
