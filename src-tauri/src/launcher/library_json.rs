//! What `MultiplayerEverythingLauncher.exe --list --json` prints (old/src/app/main.cpp and cli_json.cpp), built here:
//! every game, its actions and their state, the UI texts, the user's stats and settings. The window reads exactly
//! this, so it must stay field for field what the C++ launcher printed (tests/parity/library.mjs checks it).

use std::collections::BTreeSet;

use serde_json::{Map, Value, json};

use super::community;
use super::catalog::{GameOptions, cached_catalog_settings, coop_save_game, installed_player_version, outdated_game_files, parse_catalog};
use super::config::{ActionSpec, Config, ConfigIssue, GameSpec, format_issues, load_config, load_config_file};
use super::embedded::{LAUNCHER_VERSION, read_embedded};
use super::install::refresh_catalog_within;
use super::library::Library;
use super::log;
use super::paths;
use super::settings::{UserData, load_user_data};
use super::site::{SiteGame, fetch_site_covers, find_site, load_site_cache, mark_community, merge_projects, merge_site, parse_site, projects_url, refresh_site_cache_within};
use super::text::{atoi, compare_versions};
use super::vars::Vars;

/// Where the config comes from: `FL_LAUNCHER_CONFIG` (the lab's sandbox, as `--config` was for the C++ launcher),
/// else launcher.json next to the app, else the copy built into it. An error, as the C++ launcher's exit 3, when it
/// has problems.
pub fn load_launcher_config() -> Result<Config, String> {
    load_launcher_config_issues().map(|(cfg, _)| cfg)
}

/// The same, with the warnings launcher.json loaded with (the C++ commands write them to the log).
pub fn load_launcher_config_issues() -> Result<(Config, Vec<ConfigIssue>), String> {
    let exe_dir = paths::self_exe_dir();
    let chosen = std::env::var("FL_LAUNCHER_CONFIG").unwrap_or_default();
    let config_path = if chosen.is_empty() { paths::join(&exe_dir, "launcher.json") } else { paths::full_path(&chosen) };
    let built_in = if chosen.is_empty() && !paths::is_file(&config_path) { read_embedded("launcher.json") } else { None };
    let lr = match built_in {
        Some(text) => load_config(text, &exe_dir),
        None => load_config_file(&config_path),
    };
    if !lr.ok() {
        return Err(format!("launcher.json has problems:

{}
{config_path}", format_issues(&lr.issues)));
    }
    let (cfg, issues) = (lr.config, lr.issues);
    let mut base = Vars::default();
    base.set("LAUNCHER_DIR", &cfg.config_dir);
    // The C++ launcher opens its log in the data folder first, so the folder exists from the first start (the
    // window's "Open log folder" and settings.json both need it).
    paths::ensure_dir(&paths::normalize(&base.expand(&cfg.paths.data)));
    Ok((cfg, issues))
}

/// The data folder launcher.json names (settings.json, site.json, records.json live there).
fn data_dir_of(cfg: &Config) -> String {
    let mut base = Vars::default();
    base.set("LAUNCHER_DIR", &cfg.config_dir);
    paths::normalize(&base.expand(&cfg.paths.data))
}

/// Adds the mods from the copy of mods-info's records/ kept in the data folder (records.json;
/// src/launcher/community.rs), as the C++ mod commands do from the same file. FL_NO_COMMUNITY leaves them out.
pub fn add_community(cfg: &mut Config) {
    if std::env::var_os("FL_NO_COMMUNITY").is_some() {
        return;
    }
    if let Some(cache) = community::load_cache(&paths::join(&data_dir_of(cfg), "records.json")) {
        community::merge_records(cfg, &cache);
    }
}

/// launcher.json with the community mods added: what the window lists and what Launch looks a game up in.
pub fn load_with_community() -> Result<Config, String> {
    let mut cfg = load_launcher_config()?;
    add_community(&mut cfg);
    Ok(cfg)
}

/// The library and the user's settings, as the C++ launcher sets them up before any command.
pub fn open_library(cfg: &Config) -> (Library<'_>, UserData) {
    let mut base = Vars::default();
    base.set("LAUNCHER_DIR", &cfg.config_dir);
    let mut lib = Library::new(cfg, base);
    let user = load_user_data(&paths::join(&lib.data_dir(), "settings.json"));
    if !user.games_dir.is_empty() {
        lib.set_games_dir_override(&user.games_dir);
        lib.refresh();
    }
    // The game and player folders belong to the user (Documents by default), so they exist from the first start
    // and are easy to find, not only after the first download.
    paths::ensure_dir(&lib.vars().get("GAMES_DIR"));
    paths::ensure_dir(&lib.vars().get("PLAYERS_DIR"));
    (lib, user)
}

pub fn library_json() -> Result<Value, String> {
    let cfg = load_launcher_config()?;
    let (lib, user) = open_library(&cfg);

    // As the window lists them: the approved community mods, then the website's copy from the last run, which adds
    // its games and fills gaps.
    let mut listed = cfg.clone();
    add_community(&mut listed);
    let mut site_list = Vec::new();
    if !cfg.downloads.site.is_empty() && std::env::var_os("FL_NO_SITE").is_none() {
        let covers = paths::join(&lib.data_dir(), "covers");
        // The website's community projects first, so an approved mod's card gets its description before games.json
        // could add a second card for it.
        if let Some(mut list) = load_site_cache(&paths::join(&lib.data_dir(), "projects.json")).and_then(|c| parse_site(c.raw.as_bytes())) {
            mark_community(&mut list);
            merge_projects(&mut listed, &list, &covers);
            site_list = list;
        }
        if let Some(list) = load_site_cache(&paths::join(&lib.data_dir(), "site.json")).and_then(|c| parse_site(c.raw.as_bytes())) {
            merge_site(&mut listed, &list, &covers);
            site_list.extend(list);
        }
    }
    for g in &mut listed.games {
        if let Some(m) = g.mod_spec.as_ref().filter(|_| g.description.is_empty() && g.community) {
            g.description = format!("A co-op mod for {} by {}.", m.game.name, m.info.publisher);
        }
    }
    // One order for everything the window lists: by name, nobody's mods first.
    listed.games.sort_by_cached_key(|g| g.name.to_lowercase());
    Ok(build(&listed, &lib, &user, &site_list))
}

/// What the launcher reads from the internet about its games, brought up to date: mods-info's records and releases (its
/// zip, at most every 15 minutes), and, at most once an hour, the website's games.json with any new covers it points at
/// and the game catalog. True when the library should be read again. The C++ window fetched games.json on its own thread at start.
/// `force` (Ctrl+F5) asks again even when the saved copies are under an hour old.
pub fn refresh_online_lists(force: bool) -> Result<bool, String> {
    let max_age = if force { 0 } else { community::MAX_AGE_SECONDS };
    let cfg = load_launcher_config()?;
    let community = if std::env::var_os("FL_NO_COMMUNITY").is_some() {
        Ok(false)
    } else {
        super::mods_zip::refresh(&cfg.downloads.mods_info_zip, &community::mods_info_base(&cfg), &data_dir_of(&cfg), force)
    };
    let site = refresh_site(&cfg, max_age);
    // Which game files have a newer build. A failure here says nothing about the lists, and the saved copy still works.
    let catalog = !cfg.downloads.catalog.is_empty()
        && refresh_catalog_within(&cfg.downloads.catalog, &data_dir_of(&cfg), max_age).unwrap_or_else(|e| {
            log::warn(&format!("Game catalog not refreshed: {e}"));
            false
        });
    match (community, site) {
        (Ok(a), Ok(b)) => Ok(a || b || catalog),
        _ if catalog => Ok(true),
        (Ok(true), _) | (_, Ok(true)) => Ok(true),
        (Err(e), _) | (_, Err(e)) => Err(e),
    }
}

fn refresh_site(cfg: &Config, max_age: i64) -> Result<bool, String> {
    let cfg = cfg.clone();
    if cfg.downloads.site.is_empty() || std::env::var_os("FL_NO_SITE").is_some() {
        return Ok(false);
    }
    let (lib, _) = open_library(&cfg);
    let cache = paths::join(&lib.data_dir(), "site.json");
    let changed = refresh_site_cache_within(&cfg.downloads.site, &cache, max_age);
    let projects = refresh_site_cache_within(&projects_url(&cfg.downloads.site), &paths::join(&lib.data_dir(), "projects.json"), max_age);
    let changed = match (changed, projects) {
        (Ok(a), Ok(b)) => Ok(a || b),
        (Ok(true), _) | (_, Ok(true)) => Ok(true),
        (Err(e), _) | (_, Err(e)) => Err(e),
    };
    // Covers are looked for even when games.json didn't change: one that failed last time may come now.
    let mut covers = 0;
    if let Some(list) = load_site_cache(&cache).and_then(|c| parse_site(c.raw.as_bytes())) {
        let cover_dir = paths::join(&lib.data_dir(), "covers");
        let mut merged = cfg.clone();
        merge_site(&mut merged, &list, &cover_dir);
        covers = fetch_site_covers(&merged, &list, &cfg.downloads.site, &cover_dir);
    }
    match changed {
        Ok(c) => Ok(c || covers > 0),
        Err(_) if covers > 0 => Ok(true),
        Err(e) => Err(e),
    }
}

/// Where a game's cover is on disk: its own file, or the built-in one written out once under the data folder.
fn cover_path(g: &GameSpec, cfg: &Config, lib: &Library) -> String {
    if g.image.is_empty() {
        return String::new();
    }
    let rel = lib.vars().expand(&g.image);
    let path = if paths::is_absolute(&rel) { rel.clone() } else { paths::join(&cfg.config_dir, &rel) };
    if paths::is_file(&path) || paths::is_absolute(&rel) {
        return if paths::is_file(&path) { path } else { String::new() };
    }
    let Some(bytes) = read_embedded(&rel) else { return String::new() };
    let out = paths::join(&paths::join(&lib.data_dir(), "covers\\built-in"), &paths::file_name(&rel));
    if !paths::is_file(&out) && (!paths::ensure_dir(&paths::parent(&out)) || !paths::write_file(&out, bytes)) {
        return String::new();
    }
    out
}

/// The arguments a host or join action passes to Ruffle Together say what it is; the window's Host and Join use them.
fn coop_flags(a: &ActionSpec, o: &mut Map<String, Value>) {
    let (mut host, mut relay, mut join, mut address) = (false, false, !a.join_args.is_empty(), false);
    let mut players = 0;
    for (k, s) in a.args.iter().enumerate() {
        if s.starts_with("--coop-host") {
            host = true;
        }
        if s == "--coop-host-relay" {
            relay = true;
        }
        if s.starts_with("--coop-join") {
            join = true;
        }
        if s == "--coop-players" && k + 1 < a.args.len() {
            players = atoi(&a.args[k + 1]);
        }
        if s.contains("${ADDRESS}") {
            address = true;
        }
    }
    o.insert("coopHost".into(), json!(host));
    o.insert("coopJoin".into(), json!(join));
    o.insert("joinsByCode".into(), json!(!a.join_args.is_empty())); // --join <code> picks its joinArgs
    o.insert("hostPlayers".into(), json!(players));
    o.insert("viaRelay".into(), json!(relay));
    o.insert("asksAddress".into(), json!(address));
}

fn strings(list: &[String]) -> Value {
    Value::Array(list.iter().map(|s| json!(s)).collect())
}

/// The JSON itself, from a loaded config (the website's games already merged in), the library and the user's data.
pub fn build(cfg: &Config, lib: &Library, user: &UserData, site: &[SiteGame]) -> Value {
    let mut root = Map::new();
    root.insert("version".into(), json!(1));
    root.insert("configDir".into(), json!(cfg.config_dir));
    root.insert("dataDir".into(), json!(lib.data_dir()));
    root.insert("gamesDir".into(), json!(lib.vars().get("GAMES_DIR")));
    root.insert("flashpoint".into(), json!(lib.flashpoint_dir()));
    root.insert("launcherVersion".into(), json!(LAUNCHER_VERSION));
    root.insert("website".into(), json!(cfg.branding.website));
    root.insert("logFile".into(), json!(paths::join(&lib.data_dir(), "launcher.log")));
    root.insert("defaultGamesDir".into(), json!(paths::normalize(&lib.vars().expand(&cfg.paths.games))));
    root.insert(
        "branding".into(),
        json!({
            "title": cfg.branding.title,
            "subtitle": cfg.branding.subtitle,
            "playerColors": cfg.branding.player_colors.iter().map(|c| c.hex()).collect::<Vec<_>>(),
        }),
    );
    let players: Vec<Value> = lib
        .players()
        .iter()
        .map(|p| {
            let spec = &lib.config().players[p.index];
            json!({
                "id": spec.id,
                "name": spec.name,
                "found": p.found(),
                "exe": p.exe,
                "installable": spec.install.enabled(),
                "version": installed_player_version(&p.exe), // the release the launcher installed, if it did
                "website": spec.website,
            })
        })
        .collect();
    root.insert("players".into(), Value::Array(players));
    root.insert("theme".into(), json!(cfg.branding.theme));
    root.insert("roomsServer".into(), json!(cfg.rooms.server)); // where the status bar measures its ping
    root.insert("discord".into(), json!(!cfg.discord_client_id.is_empty()));
    let cats: Vec<Value> = cfg.categories.iter().map(|c| json!({ "id": c.id, "name": c.name, "icon": c.icon, "group": c.group })).collect();
    root.insert("categories".into(), Value::Array(cats));

    let catalog_settings = cached_catalog_settings(&lib.data_dir());
    // Game files the cached catalog has a newer build of: the card says "Update available" and Download fetches it.
    let mut outdated: BTreeSet<String> = BTreeSet::new();
    if !cfg.downloads.catalog.is_empty()
        && let Some(text) = paths::read_file(&paths::join(&lib.data_dir(), "catalog.json"))
        && let Ok(catalog) = parse_catalog(&text, &cfg.downloads.catalog)
    {
        outdated.extend(outdated_game_files(&catalog, &lib.vars().get("GAMES_DIR")).into_iter().map(|f| f.game));
    }
    // A record's pinned files (a developer's Flash game): one there with another SHA-256 means the approval moved on.
    for g in &cfg.games {
        let stale = |f: &&super::config::PinnedFile| {
            let path = paths::join(&paths::join(&lib.vars().get("GAMES_DIR"), &f.dir), &f.name);
            paths::is_file(&path) && paths::sha256_file(&path) != f.sha256
        };
        if g.downloads.iter().any(|f| stale(&f)) {
            outdated.insert(g.id.clone());
        }
    }
    let options = GameOptions { cfg, lib, user, catalog: &catalog_settings };
    let mut games = Vec::new();
    for g in &cfg.games {
        let mut o = Map::new();
        let name = match &g.mod_spec {
            Some(m) if !m.game.name.is_empty() => &m.game.name,
            _ => &g.name,
        };
        o.insert("id".into(), json!(g.id));
        o.insert("name".into(), json!(name));
        o.insert("description".into(), json!(g.description));
        o.insert("players".into(), json!(g.player_count));
        o.insert("maxPlayers".into(), json!(g.max_players));
        o.insert("color".into(), json!(g.color.hex()));
        o.insert("colorEnd".into(), json!(g.color_end.hex()));
        o.insert("image".into(), json!(cover_path(g, cfg, lib)));
        o.insert("imageFocus".into(), json!(f64::from(g.image_focus)));
        o.insert("onlineId".into(), json!(g.online_id));
        o.insert("joinAction".into(), json!(g.join_action));
        o.insert("comingSoon".into(), json!(g.coming_soon));
        o.insert("lobby".into(), json!(g.lobby.enabled && g.mod_spec.is_none())); // played through the launcher's lobby
        // The game's own settings and, for Ruffle Together co-op, which save the host starts from.
        let mut sets = Vec::new();
        if g.mod_spec.is_none() && !g.coming_soon {
            for st in options.settings_for(g) {
                sets.push(json!({
                    "id": st.id,
                    "type": st.kind,
                    "scope": st.scope,
                    "label": st.label,
                    "since": st.since,
                    "enabled": options.player_at_least(&st.since),
                    "value": options.setting_value(g, st),
                    "choices": st.choices.iter().map(|c| json!({ "id": c.id, "label": c.label })).collect::<Vec<_>>(),
                }));
            }
        }
        o.insert("settings".into(), Value::Array(sets));
        if g.mod_spec.is_none()
            && coop_save_game(g)
            && let Some(p) = options.coop_save_player()
        {
            let f = options.coop_save_file(g);
            o.insert(
                "coopSave".into(),
                json!({ "supported": options.coop_save_supported(), "since": p.coop_save_since, "progress": !f.is_empty() && paths::is_file(&f) }),
            );
        }
        o.insert("outdated".into(), json!(outdated.contains(&g.id)));
        // The player picked for this game, empty = automatic.
        o.insert("player".into(), json!(user.find(&g.id).map(|s| s.player.as_str()).unwrap_or("")));
        o.insert("categories".into(), strings(&g.categories));
        if g.connection != Default::default() {
            o.insert("connection".into(), json!({ "ports": strings(&g.connection.ports), "vpn": g.connection.vpn, "note": g.connection.note }));
        }
        if let Some(m) = &g.mod_spec {
            o.insert(
                "mod".into(),
                json!({
                    "modName": m.info.name,
                    "publisher": m.info.publisher,
                    "website": m.info.website,
                    "gameName": m.game.name,
                    "pinnedVersion": if m.package.sha256.is_empty() { "" } else { m.package.version.as_str() },
                }),
            );
        }
        o.insert(
            "guide".into(),
            json!({
                "needs": strings(&g.guide.needs),
                "steps": strings(&g.guide.steps),
                "tips": strings(&g.guide.tips),
                "links": g.guide.links.iter().map(|l| json!({ "label": l.label, "url": l.url })).collect::<Vec<_>>(),
            }),
        );
        let mut acts = Vec::new();
        for a in &g.actions {
            let mut ao = Map::new();
            ao.insert("id".into(), json!(a.id));
            ao.insert("label".into(), json!(a.label));
            ao.insert("description".into(), json!(a.description));
            ao.insert("hint".into(), json!(a.hint)); // what to do next, shown when it starts
            ao.insert("icon".into(), json!(a.icon));
            ao.insert("more".into(), json!(a.more));
            ao.insert("offline".into(), json!(a.offline));
            ao.insert("original".into(), json!(a.original));
            ao.insert("link".into(), json!(a.link));
            ao.insert("mod".into(), if a.is_mod() { json!(a.mod_op) } else { Value::Null });
            ao.insert("relayPort".into(), json!(if a.relay.enabled { a.relay.port } else { 0 }));
            coop_flags(a, &mut ao);
            if !a.is_mod() && !a.is_link() {
                let r = lib.resolve(g, a, user, false);
                // A game that needs a newer Ruffle Together than the one this button starts: the window offers the
                // update. A copy the launcher didn't install has no release tag to compare, and is let through.
                let have = if g.min_player.is_empty() { String::new() } else { installed_player_version(&r.player_exe) };
                let old = r.state.id() == "ready" && !have.is_empty() && compare_versions(&have, &g.min_player) < 0;
                ao.insert("state".into(), json!(if old { "oldPlayer" } else { r.state.id() }));
                if old {
                    ao.insert("needs".into(), json!(g.min_player));
                }
                ao.insert("player".into(), r.player.map_or(Value::Null, |p| json!(p.name)));
                ao.insert("playerId".into(), r.player.map_or(Value::Null, |p| json!(p.id)));
                ao.insert("file".into(), json!(r.swf));
            }
            acts.push(Value::Object(ao));
        }
        o.insert("actions".into(), Value::Array(acts));
        // Who made it. Anything the record or the website doesn't credit to someone else is the team's.
        let theirs = find_site(site, &g.site_id, &g.id, &g.online_id).map(|k| &site[k]).filter(|s| s.community);
        let author = match (g.author.as_str(), theirs) {
            ("", Some(s)) if !s.author.is_empty() => s.author.as_str(),
            ("", Some(s)) => s.maintainer.as_str(),
            ("", None) => cfg.branding.title.as_str(),
            (a, _) => a,
        };
        o.insert("author".into(), json!(author));
        o.insert("authorUrl".into(), json!(if g.author_url.is_empty() { theirs.map_or("", |s| s.author_url.as_str()) } else { g.author_url.as_str() }));
        o.insert("community".into(), json!(g.community || theirs.is_some()));
        // What the website says: the grade chip, the facts line and the "in sync" dots of the details panel.
        if let Some(k) = find_site(site, &g.site_id, &g.id, &g.online_id) {
            let sg = &site[k];
            o.insert(
                "site".into(),
                json!({
                    "status": sg.status,
                    "quality": sg.quality,
                    "version": sg.version,
                    "updated": sg.updated,
                    "connect": sg.connect,
                    "community": sg.community,
                    "reviewed": sg.reviewed,
                    "author": sg.author,
                    "authorUrl": sg.author_url,
                    "repo": sg.repo,
                    "maintainer": sg.maintainer,
                    "synced": sg.synced,
                }),
            );
        }
        games.push(Value::Object(o));
    }
    root.insert("games".into(), Value::Array(games));

    let stats: Map<String, Value> = user
        .games
        .iter()
        .map(|(id, s)| (id.clone(), json!({ "favorite": s.favorite, "plays": s.plays, "seconds": s.seconds, "lastPlayed": s.last_played })))
        .collect();
    root.insert(
        "user".into(),
        json!({
            "playerName": user.player_name,
            // Settings, as the C++ Settings window shows them (an unset "minimize" reads as on, as there).
            "minimize": user.minimize != 0,
            "online": user.online,
            "defaultPlayer": user.default_player,
            "gamesDir": user.games_dir,
            "showOffline": user.show_offline,
            "showOriginals": user.show_originals,
            "autoUpdate": user.auto_update,
            "compactCards": user.compact_cards,
            "games": stats,
        }),
    );
    root.insert("text".into(), json!(cfg.text));
    Value::Object(root)
}
