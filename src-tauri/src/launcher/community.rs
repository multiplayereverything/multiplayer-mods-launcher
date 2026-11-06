//! Mods mods-info lists for the launcher (docs/COMMUNITY_MODS.md). Every mod has one record in records/, and the
//! record's repository decides which rules read it, never anything the record says about itself:
//!
//! - Any other owner: a community record, a developer's mod the owner approved. A developer describes their mod in a
//!   manifest in their own GitHub release; at approval the website pins the release and, when the owner offers it,
//!   writes the record. Strict: the keys below only. Records the team writes by hand may also carry the card's text, or
//!   be a card that only links to the mod's page ("kind": "link").
//! - multiplayereverything: a team record, Multiplayer Everything's own mod, written by the website's team-only path. It
//!   is a launcher.json games[] entry with the record's keys added, so it may use every building block launcher.json
//!   has, and it follows its repository's newest release. It is a PC mod (a "mod") or a Flash game (its "actions":
//!   buttons that start the launcher's own player with a game file).
//!
//! Either may say "minLauncher": an older launcher shows the game as needing an update instead of reading the rest.
//!
//! The community rules are old/src/core/config.cpp's `readCommunityMod`: a record may only use the keys listed here
//! (anything else is refused, not ignored), writes only inside the game folder, downloads only its pinned github.com
//! release with a SHA-256, and starts only through steam://. Then it gets every check a launcher.json mod gets. A record
//! that fails is left out whole. The copy kept in the data folder (records.json) is what the C++ mod commands read too.

use super::config::{ConfigIssue, Issues, Severity, add_mod_actions, read_card_of, read_mod, validate_config};
use super::config::{Config, GameSpec, PinnedFile, read_game};
use super::embedded::LAUNCHER_VERSION;
use super::text::compare_versions;
use super::http;
use super::json::{self, Value};
use super::paths;

const MAX_MODS: usize = 200;
pub const MAX_AGE_SECONDS: i64 = 3600;

/// mods-info's folder: launcher.json's downloads.modsInfo, else the folder of rooms.servers (Config::modsInfoBase).
pub fn mods_info_base(cfg: &Config) -> String {
    let base = if !cfg.downloads.mods_info.is_empty() {
        cfg.downloads.mods_info.clone()
    } else {
        cfg.rooms.servers.rfind('/').map(|i| cfg.rooms.servers[..=i].to_string()).unwrap_or_default()
    };
    if base.is_empty() || base.ends_with('/') { base } else { format!("{base}/") }
}

/// mods-info is fetched over https; a loopback server stands in for it, and for github.com, in the tests.
fn local(base: &str) -> bool {
    base.starts_with("http://127.0.0.1")
}

fn id_ok(id: &str) -> bool {
    !id.is_empty() && id.len() <= 48 && id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
}

/// The ids records/index.json lists ({"mods": ["id", ...]}), at most 200, each once.
pub fn parse_index(text: &[u8]) -> Option<Vec<String>> {
    let root = json::parse(text).ok()?;
    let mods = root.find("mods").filter(|m| m.is_array())?;
    let mut ids: Vec<String> = Vec::new();
    for v in mods.items() {
        if let Some(id) = v.as_str().filter(|id| id_ok(id)) {
            if !ids.iter().any(|x| x == id) && ids.len() < MAX_MODS {
                ids.push(id.to_string());
            }
        }
    }
    Some(ids)
}

/// The copy kept on disk: when it was fetched and each record exactly as it came (old/src/core/community.h).
#[derive(Debug, Default, PartialEq)]
pub struct Cache {
    pub fetched_at: i64,
    pub raw: Vec<String>,
}

pub fn load_cache(path: &str) -> Option<Cache> {
    let root = json::parse(&paths::read_file(path)?).ok()?;
    let at = root.find("at").and_then(Value::as_f64)?;
    let mods = root.find("mods").filter(|m| m.is_array())?;
    Some(Cache { fetched_at: at as i64, raw: mods.items().iter().filter_map(|m| m.as_str().map(str::to_string)).collect() })
}

pub fn save_cache(path: &str, cache: &Cache) -> bool {
    let text = serde_json::to_string_pretty(&serde_json::json!({ "at": cache.fetched_at, "mods": cache.raw })).unwrap_or_default();
    paths::ensure_dir(&paths::parent(path));
    let part = format!("{path}.part");
    paths::write_file(&part, format!("{text}\n").as_bytes()) && paths::move_file(&part, path, true).is_ok()
}

pub(super) fn now_unix() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

/// Brings records.json up to date from mods-info's records/, unless it is under `max_age` seconds old; 0 always asks
/// mods-info (Ctrl+F5). Ok(true) when the records changed. A mods-info without the folder (404) means no mods; any other
/// failure keeps the last copy.
pub fn refresh_records_within(base: &str, cache_path: &str, max_age: i64) -> Result<bool, String> {
    let folder = "records";
    let saved = load_cache(cache_path);
    let now = now_unix();
    if saved.as_ref().is_some_and(|c| now >= c.fetched_at && now - c.fetched_at < max_age) {
        return Ok(false);
    }
    if !(base.starts_with("https://") || local(base)) {
        return Err("No https address for mods-info".into());
    }
    let ids = match http::get_text(&format!("{base}{folder}/index.json")) {
        Ok(text) => parse_index(&text).ok_or(format!("mods-info's {folder}/index.json isn't a list of mods"))?,
        Err(e) if e.contains("HTTP 404") => Vec::new(),
        Err(e) => return Err(e),
    };
    let mut fresh = Cache { fetched_at: now, raw: Vec::new() };
    for id in ids {
        // One record that can't be read is left out; the others still come.
        if let Ok(text) = http::get_text(&format!("{base}{folder}/{id}.json")) {
            fresh.raw.push(String::from_utf8_lossy(&text).to_string());
        }
    }
    let changed = saved.as_ref().is_none_or(|c| c.raw != fresh.raw);
    save_cache(cache_path, &fresh);
    Ok(changed)
}

// ---- the record's rules ----------------------------------------------------------------------------------------

fn only_keys(o: &Value, keys: &[&str], place: &str) -> Result<(), String> {
    match o.entries().iter().find(|(k, _)| !keys.contains(&k.as_str())) {
        Some((k, _)) => Err(format!("{place}.{k}: not allowed in a community mod")),
        None => Ok(()),
    }
}

fn text<'a>(o: &'a Value, key: &str) -> &'a str {
    o.find(key).and_then(Value::as_str).unwrap_or("")
}

fn starts_with_i(s: &str, p: &str) -> bool {
    s.len() >= p.len() && s.is_char_boundary(p.len()) && s[..p.len()].eq_ignore_ascii_case(p)
}

/// A path the mod writes to: inside the game folder (or the mod's own folder in it), never climbing out.
fn inside_game(p: &str) -> bool {
    (starts_with_i(p, "${GAME_DIR}") || starts_with_i(p, "${INSTALL_DIR}")) && !p.contains("..")
}

/// A path inside the package: relative, no variables, no climbing out.
fn inside_package(p: &str) -> bool {
    !p.is_empty() && !p.contains("..") && !p.contains(':') && !p.contains('$') && !p.starts_with('/') && !p.starts_with('\\')
}

/// One URL path segment of a release: an owner, a repository, a tag or a file name.
fn plain_segment(s: &str) -> bool {
    !s.is_empty() && s.len() <= 120 && s != "." && s != ".." && s.chars().all(|c| c.is_alphanumeric() || "-_.+".contains(c))
}

fn sha256_hex(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn repo_name(repo: &str) -> bool {
    repo.split_once('/').is_some_and(|(o, n)| !n.contains('/') && plain_segment(o) && plain_segment(n))
}

fn digits(s: &str) -> bool {
    s.bytes().all(|b| b.is_ascii_digit())
}

/// A release file's address: `https://github.com/<repo>/releases/download/<tag>/<file>`, or the same path on a loopback
/// server when mods-info itself is one (the tests).
fn release_url_ok(url: &str, repo: &str, tag: &str, asset: &str, local_release: bool) -> bool {
    let path = format!("/{repo}/releases/download/{tag}/{asset}");
    url == format!("https://github.com{path}")
        || (local_release && url.starts_with("http://127.0.0.1:") && url[17..].find('/').is_some_and(|at| url[17 + at..] == path))
}

/// The same for any tag and file of `repo`'s releases (a team record's pinned package or loader).
fn release_url_of(url: &str, repo: &str, local_release: bool) -> bool {
    let rest = url.strip_prefix("https://github.com").or_else(|| {
        (local_release && url.starts_with("http://127.0.0.1:")).then(|| url[17..].find('/').map(|at| &url[17 + at..])).flatten()
    });
    let Some(rest) = rest else { return false };
    let prefix = format!("/{repo}/releases/download/");
    if !starts_with_i(rest, &prefix) {
        return false;
    }
    let tail: Vec<&str> = rest[prefix.len()..].split('/').collect();
    tail.len() == 2 && tail.iter().all(|s| plain_segment(s))
}

fn check_shape(r: &Value, id: &str, repo: &str, local_release: bool) -> Result<(), String> {
    let m = r.find("mod").filter(|m| m.is_object()).ok_or("no \"mod\" object")?;
    only_keys(m, &["id", "publisher", "website", "game", "install", "components", "launch", "package"], "mod")?;
    // A loader the mod's own releases carry (BepInEx, say), pinned by the approval like the mod: its tag, file and
    // SHA-256. The record never chooses another repository.
    if let Some(p) = m.find("package") {
        if !p.is_object() {
            return Err("mod.package must be an object".into());
        }
        only_keys(p, &["extras"], "mod.package")?;
        let ex = p.find("extras").filter(|e| e.is_array()).ok_or("mod.package.extras: a list")?;
        if ex.items().is_empty() || ex.items().len() > 3 {
            return Err("mod.package.extras: one to three".into());
        }
        for (i, e) in ex.items().iter().enumerate() {
            let place = format!("mod.package.extras[{i}]");
            if !e.is_object() {
                return Err(format!("{place}: must be an object"));
            }
            only_keys(e, &["dir", "tag", "asset", "url", "sha256"], &place)?;
            if !inside_package(text(e, "dir")) {
                return Err(format!("{place}.dir: a folder name inside the package"));
            }
            let (tag, asset) = (text(e, "tag"), text(e, "asset"));
            if !plain_segment(tag) || !plain_segment(asset) {
                return Err(format!("{place}: tag and asset must be plain names"));
            }
            if !sha256_hex(text(e, "sha256")) {
                return Err(format!("{place}.sha256 must be 64 lower-case hex digits"));
            }
            if !release_url_ok(text(e, "url"), repo, tag, asset, local_release) {
                return Err(format!("{place}.url must be https://github.com/{repo}/releases/download/{tag}/{asset}"));
            }
        }
    }
    if text(m, "id") != id {
        return Err("mod.id must be the record's id".into());
    }
    let site = text(m, "website");
    if !site.is_empty() && site != format!("https://github.com/{repo}") {
        return Err(format!("mod.website must be https://github.com/{repo}"));
    }
    let g = m.find("game").filter(|g| g.is_object()).ok_or("no mod.game")?;
    only_keys(g, &["name", "executable", "alternateExecutables", "steamAppId", "validationFiles", "processNames", "detection"], "mod.game")?;
    if let Some(alt) = g.find("alternateExecutables") {
        let bad = |n: &Value| n.as_str().is_none_or(|n| n.is_empty() || n.contains(['/', '\\', ':', '$']));
        if !alt.is_array() || alt.items().is_empty() || alt.items().len() > 5 || alt.items().iter().any(bad) {
            return Err("mod.game.alternateExecutables: one to five file names".into());
        }
    }
    if text(g, "name").is_empty() {
        return Err("mod.game.name is missing".into());
    }
    if text(g, "executable").contains(['/', '\\', ':', '$']) {
        return Err("mod.game.executable must be a file name".into());
    }
    if !digits(text(g, "steamAppId")) {
        return Err("mod.game.steamAppId must be digits".into());
    }
    if let Some(d) = g.find("detection").filter(|d| d.is_array()) {
        for (i, rule) in d.items().iter().enumerate() {
            let place = format!("mod.game.detection[{i}]");
            if !rule.is_object() {
                return Err(format!("{place}: must be an object"));
            }
            match text(rule, "type") {
                "steam" => {
                    only_keys(rule, &["type", "appId", "installDir", "subPath"], &place)?;
                    // The game's folder inside its Steam folder (Atlas\Binaries\Win64): it may only go further in.
                    if rule.find("subPath").is_some() && !inside_package(text(rule, "subPath")) {
                        return Err(format!("{place}.subPath: a folder inside the game's Steam folder"));
                    }
                }
                "path" => only_keys(rule, &["type", "path"], &place)?,
                _ => return Err(format!("{place}: only \"steam\" and \"path\" rules")),
            }
        }
    }
    if let Some(inst) = m.find("install") {
        if !inst.is_object() {
            return Err("mod.install must be an object".into());
        }
        only_keys(inst, &["targetDir", "stateDir", "preserve", "requires", "uninstallCleanup", "uninstallCleanupWith"], "mod.install")?;
        // Delete uninstallCleanup only with this component (the loader): a component the mod ships, by its id.
        if let Some(with) = inst.find("uninstallCleanupWith") {
            let ids: Vec<&str> = m.find("components").map(|c| c.items().iter().map(|c| text(c, "id")).collect()).unwrap_or_default();
            if with.as_str().is_none_or(|w| w.is_empty() || !ids.contains(&w)) {
                return Err("mod.install.uninstallCleanupWith: the id of one of mod.components".into());
            }
        }
        let target = text(inst, "targetDir");
        if !target.is_empty() && (!starts_with_i(target, "${GAME_DIR}") || target.contains("..")) {
            return Err("mod.install.targetDir must be inside ${GAME_DIR}".into());
        }
        // Where the install record is kept: a mod moved from launcher.json keeps its old place, so installs stay found.
        if inst.find("stateDir").is_some() && !inside_game(text(inst, "stateDir")) {
            return Err("mod.install.stateDir must be inside ${GAME_DIR} or ${INSTALL_DIR}".into());
        }
        if let Some(req) = inst.find("requires").filter(|r| r.is_array()) {
            for r in req.items() {
                if !r.is_object() {
                    return Err("mod.install.requires: objects with path and message".into());
                }
                only_keys(r, &["path", "message", "url", "sha256", "absent", "warn"], "mod.install.requires")?;
            }
        }
    }
    let cs = m.find("components").filter(|c| c.is_array() && !c.items().is_empty()).ok_or("mod.components: at least one")?;
    for (i, c) in cs.items().iter().enumerate() {
        let place = format!("mod.components[{i}]");
        if !c.is_object() {
            return Err(format!("{place}: must be an object"));
        }
        only_keys(c, &["id", "name", "source", "sourceGame", "target", "targetName", "required", "skipIfPresent", "onlyIfFile"], &place)?;
        // Left out when the game already has one of these (a loader another mod installed): paths in the game folder.
        if let Some(sk) = c.find("skipIfPresent") {
            if !sk.is_array() || sk.items().is_empty() || sk.items().len() > 10 || sk.items().iter().any(|p| p.as_str().is_none_or(|p| !inside_package(p))) {
                return Err(format!("{place}.skipIfPresent: one to ten paths inside the game folder, such as BepInEx/core"));
            }
        }
        // A copy of a file the game already has, under a name of its own, for a loader that wraps it: it reads and writes
        // only inside the game folder, and the copy goes on uninstall.
        if let Some(sg) = c.find("sourceGame") {
            if !sg.is_array() || sg.items().is_empty() || sg.items().len() > 5 || sg.items().iter().any(|p| p.as_str().is_none_or(|p| !inside_package(p))) {
                return Err(format!("{place}.sourceGame: one to five paths inside the game folder"));
            }
            if text(c, "targetName").is_empty() {
                return Err(format!("{place}.sourceGame needs a targetName"));
            }
        } else if !inside_package(text(c, "source")) {
            return Err(format!("{place}.source: a path inside the package"));
        }
        if let Some(only) = c.find("onlyIfFile") {
            if !only.is_object() {
                return Err(format!("{place}.onlyIfFile: an object with path and contains"));
            }
            only_keys(only, &["path", "contains"], &format!("{place}.onlyIfFile"))?;
            let contains = text(only, "contains");
            if !inside_package(text(only, "path")) || contains.is_empty() || contains.chars().count() > 200 {
                return Err(format!("{place}.onlyIfFile: a path inside the game folder and up to 200 characters of text"));
            }
        }
        let target = text(c, "target");
        if !target.is_empty() && !inside_game(target) {
            return Err(format!("{place}.target must be inside ${{GAME_DIR}}"));
        }
    }
    let l = m.find("launch").filter(|l| l.is_object()).ok_or("mod.launch is missing")?;
    only_keys(l, &["uri", "executable", "hint"], "mod.launch")?;
    // Through Steam, or a program in the game's folder (the mod's own launcher, say): one of the two.
    let (uri, exe) = (text(l, "uri"), text(l, "executable"));
    if !uri.is_empty() == !exe.is_empty() {
        return Err("mod.launch: either uri (steam://rungameid/<app id>) or executable (${GAME_DIR}\\<program>.exe)".into());
    }
    if !exe.is_empty() {
        let the_game = exe.eq_ignore_ascii_case("${GAME_DIR}\\${GAME_EXE}"); // under whichever name this folder has
        let ok = the_game
            || (starts_with_i(exe, "${GAME_DIR}\\") && !exe.contains("..") && !exe[12..].contains(['$', ':', '/'])
                && exe.to_ascii_lowercase().ends_with(".exe"));
        return if ok { Ok(()) } else { Err("mod.launch.executable must be ${GAME_DIR}\\<program>.exe, inside the game folder".into()) };
    }
    let tail = if starts_with_i(uri, "steam://rungameid/") { &uri[18..] } else { "" };
    if !(tail == "${STEAM_APP_ID}" || (!tail.is_empty() && digits(tail))) {
        return Err("mod.launch.uri must be steam://rungameid/<app id>".into());
    }
    Ok(())
}

/// One community record as a game of the launcher, or why it is left out. `local_release`: the release may come from
/// a local test server, only when mods-info itself is one.
pub fn read_record(text_: &str, cfg: &Config, local_release: bool) -> Result<GameSpec, String> {
    let r = json::parse(text_.as_bytes()).map_err(|e| format!("not a JSON object ({e})"))?;
    if !r.is_object() {
        return Err("not a JSON object".into());
    }
    let g = read_record_of(&r, cfg, local_release)?;
    Ok(credited(g, &r, cfg, text(&r, "repo")))
}

/// Someone else's mod says so, whatever the record calls its author: the team's name is never theirs to use.
fn credited(mut g: GameSpec, r: &Value, cfg: &Config, repo: &str) -> GameSpec {
    let owner = repo.split('/').next().unwrap_or(repo);
    let author = text(r, "author").trim();
    let ours = |a: &str| a.eq_ignore_ascii_case(&cfg.branding.title) || a.eq_ignore_ascii_case(TEAM_OWNER);
    g.author = if author.is_empty() || ours(author) { owner.to_string() } else { author.to_string() };
    let url = text(r, "authorUrl");
    g.author_url = if url.starts_with("https://github.com/") { url.to_string() } else { format!("https://github.com/{owner}") };
    g.community = true;
    g
}

/// Which list a developer's record goes in. A mod the launcher installs is a PC mod and a Flash record a Flash game,
/// whatever "platform" says; only a link card, which could be either, takes its word.
fn platform_of<'a>(r: &'a Value, kind: &str) -> Result<&'a str, String> {
    let said = r.find("platform").map(|_| text(r, "platform"));
    if said.is_some_and(|p| p != "pc" && p != "flash") {
        return Err("platform: \"pc\" or \"flash\"".into());
    }
    Ok(match kind {
        "flash" => "flash",
        "link" => said.unwrap_or("pc"),
        _ => "pc",
    })
}

fn read_record_of(r: &Value, cfg: &Config, local_release: bool) -> Result<GameSpec, String> {
    // "pc" (the default): a mod the launcher installs. "link": a card that only links to the mod's page. "flash": a Flash
    // game played in Ruffle Together (1.0.43). A kind this launcher doesn't know is left out whole, so an older
    // launcher never shows half of one.
    let kind = r.find("kind").map(|_| text(r, "kind")).unwrap_or("pc");
    let (id, repo) = (text(r, "id"), text(r, "repo"));
    if !id_ok(id) {
        return Err("id: lowercase letters, digits and '-', up to 48".into());
    }
    if !repo_name(repo) {
        return Err("repo must be \"owner/name\"".into());
    }
    let platform = platform_of(r, kind)?;
    if let Some(min) = needs_newer(r)? {
        return Ok(update_card(r, cfg, id, &min, vec![platform.to_string()]));
    }
    match kind {
        "pc" => only_keys(r, &[RECORD_KEYS, CARD_KEYS, &["tag", "version", "asset", "url", "sha256", "manifest_sha256", "mod"]].concat(), "record")?,
        "link" => only_keys(r, &[RECORD_KEYS, CARD_KEYS, &["link"]].concat(), "record")?,
        "flash" => only_keys(r, &[RECORD_KEYS, CARD_KEYS, &["tag", "version", "manifest_sha256", "flash"]].concat(), "record")?,
        _ => return Err(format!("kind \"{kind}\" isn't known to this launcher")),
    }
    if cfg.games.iter().any(|g| g.id == id || g.site_id == id || g.mod_spec.as_ref().is_some_and(|m| m.info.id == id)) {
        return Err(format!("launcher.json already has \"{id}\""));
    }
    if kind == "flash" {
        return read_dev_flash_record(r, cfg, id, repo, local_release);
    }
    let issues: Issues = Default::default();
    let mut g = GameSpec { id: id.to_string(), site_id: id.to_string(), categories: vec![platform.to_string()], ..Default::default() };
    check_card(r)?;
    read_card_of(r, &format!("community.{id}"), &issues, &mut g);
    if kind == "link" {
        return read_link(r, cfg, g, issues);
    }
    let (tag, asset) = (text(r, "tag"), text(r, "asset"));
    let (version, url, sha) = (text(r, "version"), text(r, "url"), text(r, "sha256"));
    if !plain_segment(tag) || !plain_segment(asset) {
        return Err("tag and asset must be plain names".into());
    }
    if !plain_segment(version) {
        return Err("version is missing".into());
    }
    if !sha256_hex(sha) {
        return Err("sha256 must be 64 lower-case hex digits".into());
    }
    // The download is the pinned release asset on github.com, under the record's own repository.
    if !release_url_ok(url, repo, tag, asset, local_release) {
        return Err(format!("url must be https://github.com/{repo}/releases/download/{tag}/{asset}"));
    }
    check_shape(r, id, repo, local_release)?;

    let m = r.find("mod").unwrap();
    g.name = text(m.find("game").unwrap(), "name").to_string(); // a community card is named after the game
    let mut spec = read_mod(m, &g, &format!("community.{id}.mod"), &issues);
    // The package is the one the approval pinned, never the record's to choose.
    spec.package.version = version.to_string();
    spec.package.url = url.to_string();
    spec.package.sha256 = sha.to_string();
    if spec.info.publisher.is_empty() {
        spec.info.publisher = text(r, "author").to_string();
    }
    if spec.info.website.is_empty() {
        spec.info.website = format!("https://github.com/{repo}");
    }
    // The author's credit, as every community entry in launcher.json gives it. The description comes from the website
    // (library_json.rs), or says whose mod it is.
    let author = if spec.info.publisher.is_empty() { repo.split('/').next().unwrap_or(repo).to_string() } else { spec.info.publisher.clone() };
    credit(&mut g, repo, &author, &format!(
        "Made by {author}, not by Multiplayer Everything. We install the exact version we looked at ({version}) and update it only after we have looked at the new one. Report problems to its author."
    ));
    g.mod_spec = Some(spec);
    add_mod_actions(&mut g);
    // A loopback release (only accepted above when mods-info is loopback too: the tests) isn't https.
    let test_release = local_release && url.starts_with("http://127.0.0.1:");
    checked(g, cfg, issues, test_release)
}

/// Keys every record may have, and the card's own text (what launcher.json's games[] give a card besides its name).
/// Card text is for records the team writes by hand; the website's form never writes it (docs/COMMUNITY_MODS.md).
const RECORD_KEYS: &[&str] = &["id", "repo", "kind", "platform", "author", "authorUrl", "approved_at", "approved_by", "minLauncher"];
const CARD_KEYS: &[&str] = &["description", "players", "maxPlayers", "color", "colorEnd", "guide", "connection"];

/// The card text a record may carry: plain text and https links only, short enough to fit on the card.
fn check_card(r: &Value) -> Result<(), String> {
    for key in ["description", "players"] {
        if r.find(key).is_some_and(|v| v.as_str().is_none_or(|s| s.chars().count() > 600)) {
            return Err(format!("{key}: text, at most 600 characters"));
        }
    }
    if let Some(c) = r.find("connection") {
        if !c.is_object() {
            return Err("connection must be an object".into());
        }
        only_keys(c, &["ports", "vpn", "note"], "connection")?;
        if c.find("ports").is_some_and(|p| !p.is_array() || p.items().len() > 6) {
            return Err("connection.ports: up to 6".into());
        }
        if c.find("note").is_some_and(|n| n.as_str().is_none_or(|n| n.chars().count() > 300)) {
            return Err("connection.note: text, at most 300 characters".into());
        }
    }
    let Some(gd) = r.find("guide") else { return Ok(()) };
    if !gd.is_object() {
        return Err("guide must be an object".into());
    }
    only_keys(gd, &["needs", "steps", "tips", "keys", "links"], "guide")?;
    for key in ["needs", "steps", "tips"] {
        if gd.find(key).is_some_and(|l| !l.is_array() || l.items().len() > 12 || l.items().iter().any(|t| t.as_str().is_none_or(|s| s.chars().count() > 600))) {
            return Err(format!("guide.{key}: up to 12 texts of at most 600 characters"));
        }
    }
    if let Some(keys) = gd.find("keys") {
        if !keys.is_array() || keys.items().len() > 12 {
            return Err("guide.keys: up to 12".into());
        }
        for k in keys.items() {
            only_keys(k, &["key", "does"], "guide.keys")?;
        }
    }
    // The launcher adds the mod's own repository as the last link, so three are left for the record.
    if let Some(links) = gd.find("links") {
        if !links.is_array() || links.items().len() > 3 {
            return Err("guide.links: up to 3 (the repository's link is added)".into());
        }
        for l in links.items() {
            only_keys(l, &["label", "url"], "guide.links")?;
        }
    }
    Ok(())
}

/// Who made it, first among the tips, and their repository as the card's last link.
fn credit(g: &mut GameSpec, repo: &str, author: &str, tip: &str) {
    g.guide.tips.insert(0, tip.to_string());
    let name = repo.split('/').nth(1).unwrap_or(repo);
    g.guide.links.push(super::config::GuideLink { label: format!("{name} by {author} on GitHub"), url: format!("https://github.com/{repo}") });
}

/// A link card: a mod with its own installer, or none yet. It installs nothing and has one button, to the mod's page
/// in the record's own repository.
fn read_link(r: &Value, cfg: &Config, mut g: GameSpec, issues: Issues) -> Result<GameSpec, String> {
    let repo = text(r, "repo");
    let l = r.find("link").filter(|l| l.is_object()).ok_or("no \"link\" object")?;
    only_keys(l, &["game", "url", "label"], "link")?;
    g.name = text(l, "game").to_string();
    if g.name.is_empty() {
        return Err("link.game is missing".into());
    }
    let url = text(l, "url");
    let home = format!("https://github.com/{repo}");
    if !(url == home || url.strip_prefix(&home).is_some_and(|rest| rest.starts_with(['/', '#', '?']))) {
        return Err(format!("link.url must be on {home}"));
    }
    let author = match text(r, "author") {
        "" => repo.split('/').next().unwrap_or(repo).to_string(),
        a => a.to_string(),
    };
    credit(&mut g, repo, &author, &format!("Made by {author}, not by Multiplayer Everything. The launcher doesn't install, update or support it."));
    let label = match text(l, "label") {
        "" => "Open the mod's page",
        s => s,
    };
    g.actions.push(super::config::ActionSpec {
        id: "page".into(),
        label: label.to_string(),
        icon: "E774".into(),
        description: format!("github.com/{repo}"),
        link: url.to_string(),
        ..Default::default()
    });
    checked(g, cfg, issues, false)
}

/// "minLauncher": the oldest launcher that understands the record, when this one is older.
fn needs_newer(r: &Value) -> Result<Option<String>, String> {
    let Some(v) = r.find("minLauncher") else { return Ok(None) };
    let min = v.as_str().filter(|s| !s.is_empty() && s.len() <= 20 && s.bytes().all(|b| b.is_ascii_digit() || b == b'.'));
    let min = min.ok_or("minLauncher: a version such as \"1.0.42\"")?;
    Ok((compare_versions(min, LAUNCHER_VERSION) > 0).then(|| min.to_string()))
}

/// A record this launcher is too old for: a card that says so, named from what any record has, with nothing to press.
/// The rest of the record isn't read (it may use keys this launcher doesn't know).
fn update_card(r: &Value, cfg: &Config, id: &str, min: &str, categories: Vec<String>) -> GameSpec {
    let game_name = r.find("mod").and_then(|m| m.find("game")).map(|g| text(g, "name")).unwrap_or("");
    let link_name = r.find("link").map(|l| text(l, "game")).unwrap_or("");
    let flash_name = r.find("flash").map(|f| text(f, "game")).unwrap_or("");
    let name = [text(r, "name"), game_name, link_name, flash_name, id].into_iter().find(|s| !s.is_empty()).unwrap_or(id);
    GameSpec {
        id: id.to_string(),
        site_id: id.to_string(),
        name: name.to_string(),
        description: cfg.t("mod.needsLauncher", &[("VERSION", min)]),
        coming_soon: true,
        categories,
        ..Default::default()
    }
}

// ---- team records --------------------------------------------------------------------------------------------------

/// The only owner a team record's repository, and every repository it downloads from, may have.
const TEAM_OWNER: &str = "multiplayereverything";
/// The record's own keys; the rest is a launcher.json games[] entry: the card and the mod, no buttons of its own.
const TEAM_RECORD_KEYS: &[&str] = &["id", "repo", "platform", "author", "minLauncher", "approved_at", "approved_by"];
const TEAM_GAME_KEYS: &[&str] = &[
    "name", "description", "players", "maxPlayers", "color", "colorEnd", "guide", "connection", "categories", "site", "online", "image", "imageFocus",
    "mod", "actions", "flash", "settings",
];
/// What a Flash game's button may say: the game file, the player's arguments and its texts. No program, page, server,
/// working folder or link of its own: a team record's button only ever starts one of launcher.json's players.
const FLASH_ACTION_KEYS: &[&str] = &["id", "label", "icon", "description", "hint", "swf", "args", "joinArgs", "more", "offline", "edition", "flashvars", "players"];

/// Where a Flash game's file may be looked for: the games folder (the catalog's downloads), or a copy Flashpoint or the
/// Desktop already has. It is read, never written.
fn flash_file(p: &str) -> bool {
    ["${GAMES_DIR}\\", "${FLASHPOINT}\\", "${folder:Desktop}\\"].iter().any(|root| starts_with_i(p, root))
        && !p.contains("..")
        && !p[p.find('}').unwrap_or(0) + 1..].contains(['$', ':'])
}

fn check_flash_actions(actions: &Value) -> Result<(), String> {
    if !actions.is_array() || actions.items().is_empty() || actions.items().len() > 20 {
        return Err("actions: one to twenty buttons".into());
    }
    for (i, a) in actions.items().iter().enumerate() {
        let place = format!("actions[{i}]");
        if !a.is_object() {
            return Err(format!("{place}: must be an object"));
        }
        only_keys(a, FLASH_ACTION_KEYS, &format!("team record.{place}"))?;
        let files: Vec<&Value> = match a.find("swf") {
            Some(v) if v.is_array() => v.items().iter().collect(),
            Some(v) => vec![v],
            None => Vec::new(),
        };
        if files.is_empty() || files.iter().any(|f| f.as_str().is_none_or(|f| !flash_file(f))) {
            return Err(format!("{place}.swf: game files in ${{GAMES_DIR}}, ${{FLASHPOINT}} or ${{folder:Desktop}}"));
        }
    }
    Ok(())
}

fn team_repo(repo: &str) -> bool {
    repo_name(repo) && repo.split('/').next().is_some_and(|o| o.eq_ignore_ascii_case(TEAM_OWNER))
}

/// One team record as a game of the launcher, or why it is left out.
pub fn read_team_record(text_: &str, cfg: &Config, local_release: bool) -> Result<GameSpec, String> {
    let r = json::parse(text_.as_bytes()).map_err(|e| format!("not a JSON object ({e})"))?;
    if !r.is_object() {
        return Err("not a JSON object".into());
    }
    let (id, repo) = (text(&r, "id"), text(&r, "repo"));
    if !id_ok(id) {
        return Err("id: lowercase letters, digits and '-', up to 48".into());
    }
    if !team_repo(repo) {
        return Err(format!("repo must be \"{TEAM_OWNER}/<name>\": team records are the team's own repositories"));
    }
    let categories: Vec<String> = r.find("categories").map(|c| c.items().iter().filter_map(|v| v.as_str().map(str::to_string)).collect()).unwrap_or_default();
    if let Some(min) = needs_newer(&r)? {
        let known = categories.into_iter().filter(|c| c != "community" && cfg.categories.iter().any(|k| &k.id == c)).collect();
        return Ok(update_card(&r, cfg, id, &min, known));
    }
    only_keys(&r, &[TEAM_RECORD_KEYS, TEAM_GAME_KEYS].concat(), "team record")?;
    if cfg.games.iter().any(|g| g.id == id || g.mod_spec.as_ref().is_some_and(|m| m.info.id == id)) {
        return Err(format!("launcher.json already has \"{id}\""));
    }
    if categories.iter().any(|c| c == "community") {
        return Err("categories: \"community\" is for developers' records".into());
    }
    // A PC mod, a Flash game with its own buttons, or a Flash game the launcher makes the buttons for: one of the three.
    if ["mod", "actions", "flash"].iter().filter(|k| r.find(k).is_some()).count() != 1 {
        return Err("a team record is a PC mod (\"mod\") or a Flash game (\"flash\", or its own \"actions\"): one of them".into());
    }
    if let Some(f) = r.find("flash") {
        let fl = read_team_flash(f)?;
        let issues: Issues = Default::default();
        let mut g = read_game(&flash_entry(team_entry(&r), cfg, &fl), &format!("team.{id}"), &issues);
        g.min_player = fl.ruffle;
        return checked(g, cfg, issues, local_release);
    }
    if r.find("mod").is_none() {
        check_flash_actions(r.find("actions").unwrap())?;
        return team_game(&r, id, cfg, local_release);
    }
    let m = r.find("mod").filter(|m| m.is_object()).ok_or("no \"mod\" object")?;
    if m.find("id").is_some_and(|v| v.as_str() != Some(id)) {
        return Err("mod.id must be the record's id".into());
    }
    // Downloads only from the team's own repositories: the record's for the mod, the team's for a loader.
    let p = m.find("package").filter(|p| p.is_object()).ok_or("mod.package is missing")?;
    let gh = text(p, "github");
    if !gh.is_empty() && !gh.eq_ignore_ascii_case(repo) {
        return Err(format!("mod.package.github must be {repo}"));
    }
    if p.find("url").is_some() && !release_url_of(text(p, "url"), repo, local_release) {
        return Err(format!("mod.package.url must be a release file of {repo}"));
    }
    if let Some(ex) = p.find("extras") {
        for (i, e) in ex.items().iter().enumerate() {
            let from = match text(e, "github") {
                "" => repo,
                g => g,
            };
            if !team_repo(from) {
                return Err(format!("mod.package.extras[{i}].github must be a {TEAM_OWNER} repository"));
            }
            if e.find("url").is_some() && !release_url_of(text(e, "url"), from, local_release) {
                return Err(format!("mod.package.extras[{i}].url must be a release file of {from}"));
            }
        }
    }
    team_game(&r, id, cfg, local_release)
}

/// The record without its own keys, read as the launcher.json games[] entry it is, then checked like one.
fn team_game(r: &Value, id: &str, cfg: &Config, local_release: bool) -> Result<GameSpec, String> {
    let issues: Issues = Default::default();
    // Exactly what launcher.json makes of it: no website id it doesn't give.
    let g = read_game(&team_entry(r), &format!("team.{id}"), &issues);
    checked(g, cfg, issues, local_release)
}

/// The record without its own keys (and without "flash", which becomes buttons).
fn team_entry(r: &Value) -> Value {
    let mut entry = Value::object();
    for (k, v) in r.entries() {
        if k != "flash" && (k == "id" || !TEAM_RECORD_KEYS.contains(&k.as_str())) {
            entry.set(k, v.clone());
        }
    }
    entry
}

/// Adds every team record that passes to the games; the reasons the others were left out. Before the community
/// records: a developer's record never takes one of the team's ids.
pub fn merge_team(cfg: &mut Config, cache: &Cache) -> Vec<String> {
    let local_release = local(&mods_info_base(cfg));
    let mut left_out = Vec::new();
    for raw in &cache.raw {
        match read_team_record(raw, cfg, local_release) {
            Ok(g) => cfg.games.push(g),
            Err(e) => left_out.push(e),
        }
    }
    left_out
}

// ---- Flash records: the record describes the game, the launcher makes the buttons -------------------------------

/// The port every generated "Host with your IP" button listens on (Ruffle Together's own default).
const FLASH_PORT: &str = "7790";
/// A team record's "flash": a game file the catalog (or Flashpoint, or the Desktop) has, a built-in key map, and the
/// unmodded game as an extra button.
const FLASH_TEAM_KEYS: &[&str] = &["file", "keys", "maxPlayers", "offline", "original", "ruffle"];
/// A developer's "flash": the game file (and maybe its key map) pinned from their own release.
const FLASH_DEV_KEYS: &[&str] = &["game", "file", "keys", "maxPlayers", "offline", "ruffle"];
/// The most players a generated button hosts (Ruffle Together's limit).
const FLASH_MAX_PLAYERS: i64 = 6;

/// What a Flash record says about its game; every button follows from it.
struct FlashGame {
    /// The co-op game file: a path `flash_file` accepts.
    file: String,
    /// `--coop-keys`: a map Ruffle Together has built in, or the path of a .keys file.
    keys: String,
    max: i64,
    offline: Option<FlashOffline>,
    /// The unmodded game ("Play the original"): where it may be, and the button's text.
    original: Option<(Vec<String>, String)>,
    /// The oldest Ruffle Together the game runs in.
    ruffle: String,
}

struct FlashOffline {
    /// Everyone on one keyboard ("Play on one keyboard"), not one player on their own.
    local: bool,
    keys: String,
    label: String,
    sub: String,
}

/// A short line of text the record may give a button, at most `max` characters.
fn short_text(v: Option<&Value>, place: &str, max: usize) -> Result<String, String> {
    match v {
        None => Ok(String::new()),
        Some(v) => v
            .as_str()
            .filter(|s| s.chars().count() <= max && !s.contains(['\n', '\r']))
            .map(str::to_string)
            .ok_or(format!("{place}: text, at most {max} characters")),
    }
}

/// A built-in key map's name, as Ruffle Together spells them (air-battle, gun-mayhem-2). Never one that reads as an
/// option of its own.
fn key_map_name(s: &str) -> bool {
    id_ok(s) && !s.starts_with('-')
}

fn version_text(s: &str) -> bool {
    !s.is_empty() && s.len() <= 20 && s.bytes().all(|b| b.is_ascii_digit() || b == b'.') && !s.starts_with('.') && !s.ends_with('.')
}

/// The parts a team record and a developer's record write the same way: players, the offline button, Ruffle Together.
fn read_flash_rest(f: &Value, file: String, keys: String) -> Result<FlashGame, String> {
    let max = f.find("maxPlayers").and_then(Value::as_f64).ok_or("flash.maxPlayers: how many play online, 2 to 6")?;
    if max.fract() != 0.0 || !(2.0..=FLASH_MAX_PLAYERS as f64).contains(&max) {
        return Err("flash.maxPlayers: how many play online, 2 to 6".into());
    }
    let offline = match f.find("offline") {
        None => None,
        Some(o) if o.is_object() => {
            only_keys(o, &["local", "keys", "label", "description"], "flash.offline")?;
            let local = match o.find("local") {
                None => false,
                Some(v) => v.as_bool().ok_or("flash.offline.local: true or false")?,
            };
            // Its own built-in map (one player's keys), or false for none at all.
            let own = match o.find("keys") {
                None => keys.clone(),
                Some(Value::Bool(false)) => String::new(),
                Some(v) => v.as_str().filter(|k| key_map_name(k)).map(str::to_string).ok_or("flash.offline.keys: a key map Ruffle Together has built in, or false")?,
            };
            Some(FlashOffline {
                local,
                keys: own,
                label: short_text(o.find("label"), "flash.offline.label", 40)?,
                sub: short_text(o.find("description"), "flash.offline.description", 120)?,
            })
        }
        Some(_) => return Err("flash.offline: an object ({} for the plain button)".into()),
    };
    let ruffle = text(f, "ruffle");
    if f.find("ruffle").is_some() && !version_text(ruffle) {
        return Err("flash.ruffle: a Ruffle Together version such as \"1.0.3.7\"".into());
    }
    Ok(FlashGame { file, keys, max: max as i64, offline, original: None, ruffle: ruffle.to_string() })
}

/// A team record's "flash".
fn read_team_flash(f: &Value) -> Result<FlashGame, String> {
    if !f.is_object() {
        return Err("flash must be an object".into());
    }
    only_keys(f, FLASH_TEAM_KEYS, "flash")?;
    let file = text(f, "file");
    if !flash_file(file) {
        return Err("flash.file: a game file in ${GAMES_DIR}, ${FLASHPOINT} or ${folder:Desktop}".into());
    }
    if !key_map_name(text(f, "keys")) {
        return Err("flash.keys: a key map Ruffle Together has built in".into());
    }
    let mut fl = read_flash_rest(f, file.to_string(), text(f, "keys").to_string())?;
    if let Some(o) = f.find("original") {
        if !o.is_object() {
            return Err("flash.original must be an object".into());
        }
        only_keys(o, &["files", "description"], "flash.original")?;
        let listed = o.find("files").filter(|l| l.is_array()).map_or(0, |l| l.items().len());
        let files: Vec<String> =
            o.find("files").map(|l| l.items().iter().filter_map(|p| p.as_str().filter(|p| flash_file(p)).map(str::to_string)).collect()).unwrap_or_default();
        if files.is_empty() || files.len() > 8 || files.len() != listed {
            return Err("flash.original.files: one to eight game files in ${GAMES_DIR}, ${FLASHPOINT} or ${folder:Desktop}".into());
        }
        fl.original = Some((files, short_text(o.find("description"), "flash.original.description", 120)?));
    }
    Ok(fl)
}

/// One file a developer's Flash record pins from its release: its name, address, SHA-256 and size.
fn pinned_file(v: Option<&Value>, place: &str, repo: &str, tag: &str, ext: &str, local_release: bool) -> Result<PinnedFile, String> {
    let v = v.filter(|v| v.is_object()).ok_or(format!("{place}: the release file, {{\"asset\", \"url\", \"sha256\", \"size\"}}"))?;
    only_keys(v, &["asset", "url", "sha256", "size"], place)?;
    let asset = text(v, "asset");
    if !plain_segment(asset) || !asset.to_ascii_lowercase().ends_with(ext) {
        return Err(format!("{place}.asset: a {ext} file of the release"));
    }
    let sha = text(v, "sha256");
    if !sha256_hex(sha) {
        return Err(format!("{place}.sha256 must be 64 lower-case hex digits"));
    }
    let url = text(v, "url");
    if !release_url_ok(url, repo, tag, asset, local_release) {
        return Err(format!("{place}.url must be https://github.com/{repo}/releases/download/{tag}/{asset}"));
    }
    let size = match v.find("size") {
        None => 0.0,
        Some(n) => n
            .as_f64()
            .filter(|n| n.fract() == 0.0 && *n > 0.0 && *n <= 512.0 * 1024.0 * 1024.0)
            .ok_or(format!("{place}.size: bytes, at most 512 MB"))?,
    };
    Ok(PinnedFile { dir: String::new(), name: asset.to_string(), url: url.to_string(), sha256: sha.to_string(), size: size as u64 })
}

/// A developer's "flash": the files come from the pinned release into the games folder's community\<id> folder.
fn read_dev_flash(f: &Value, id: &str, repo: &str, tag: &str, local_release: bool) -> Result<(FlashGame, Vec<PinnedFile>), String> {
    if !f.is_object() {
        return Err("flash must be an object".into());
    }
    only_keys(f, FLASH_DEV_KEYS, "flash")?;
    let dir = format!("community\\{id}");
    let mut files = vec![PinnedFile { dir: dir.clone(), ..pinned_file(f.find("file"), "flash.file", repo, tag, ".swf", local_release)? }];
    let keys = match f.find("keys") {
        Some(k) if k.is_string() && key_map_name(text(f, "keys")) => text(f, "keys").to_string(),
        Some(k) if k.is_object() => {
            let map = PinnedFile { dir: dir.clone(), ..pinned_file(Some(k), "flash.keys", repo, tag, ".keys", local_release)? };
            let path = format!("${{GAMES_DIR}}\\{dir}\\{}", map.name);
            files.push(map);
            path
        }
        _ => return Err("flash.keys: a key map Ruffle Together has built in, or the release's .keys file".into()),
    };
    let file = format!("${{GAMES_DIR}}\\{dir}\\{}", files[0].name);
    Ok((read_flash_rest(f, file, keys)?, files))
}

fn strings_value(v: &[&str]) -> Value {
    Value::Array(v.iter().map(|s| Value::from(*s)).collect())
}

/// The buttons every Flash game gets, in the order the game page lists them: online through the relay (rooms for 2 to
/// maxPlayers), host and join by IP, the offline button, the original game. Texts come from launcher.json (flash.*).
fn flash_actions(cfg: &Config, fl: &FlashGame) -> Value {
    let max = fl.max.to_string();
    let button = |id: &str, label: String, icon: &str, sub: String, hint: String, args: Vec<&str>| {
        let mut b = Value::object();
        b.set("id", id);
        b.set("label", label);
        if !icon.is_empty() {
            b.set("icon", icon);
        }
        b.set("description", sub);
        if !hint.is_empty() {
            b.set("hint", hint);
        }
        b.set("swf", strings_value(&[fl.file.as_str()]));
        if !args.is_empty() {
            b.set("args", strings_value(&args));
        }
        b
    };
    let keys = fl.keys.as_str();
    let mut out = Vec::new();
    let hint = if fl.max > 2 { cfg.t("flash.online.hintMore", &[("MAX", &max)]) } else { cfg.t("flash.online.hint", &[]) };
    let mut online = button(
        "online",
        cfg.t("flash.online", &[]),
        "E774",
        cfg.t("flash.online.sub", &[]),
        hint,
        vec!["--coop-host-relay", "${SERVER}", "--coop-name", "${PLAYER_NAME}", "--coop-keys", keys, "--coop-players", "2"],
    );
    online.set("joinArgs", strings_value(&["--coop-join-relay", "${SERVER}", "--coop-code", "${CODE}", "--coop-name", "${PLAYER_NAME}", "--coop-keys", keys]));
    out.push(online);
    for n in 3..=fl.max {
        let (ns, friends) = (n.to_string(), (n - 1).to_string());
        let vars = [("N", ns.as_str()), ("FRIENDS", friends.as_str())];
        let mut b = button(
            &format!("room{n}"),
            cfg.t("flash.room", &vars),
            "E774",
            cfg.t("flash.room.sub", &vars),
            cfg.t("flash.room.hint", &vars),
            vec!["--coop-host-relay", "${SERVER}", "--coop-name", "${PLAYER_NAME}", "--coop-keys", keys, "--coop-players", &ns],
        );
        b.set("more", true);
        out.push(b);
    }
    for n in 2..=fl.max {
        let (ns, friends) = (n.to_string(), (n - 1).to_string());
        let vars = [("N", ns.as_str()), ("FRIENDS", friends.as_str()), ("PORT", FLASH_PORT)];
        let (id, sub, hint) = if n == 2 {
            ("host".to_string(), cfg.t("flash.host.sub", &vars), cfg.t("flash.host.hint", &vars))
        } else {
            (format!("host{n}"), cfg.t("flash.hostMore.sub", &vars), cfg.t("flash.hostMore.hint", &vars))
        };
        let mut b = button(
            &id,
            cfg.t("flash.host", &vars),
            "E774",
            sub,
            hint,
            vec!["--coop-host", FLASH_PORT, "--coop-name", "${PLAYER_NAME}", "--coop-keys", keys, "--coop-players", &ns],
        );
        b.set("more", true);
        out.push(b);
    }
    let mut join = button(
        "join",
        cfg.t("flash.join", &[]),
        "E774",
        cfg.t("flash.join.sub", &[]),
        cfg.t("flash.join.hint", &[]),
        vec!["--coop-join", "${ADDRESS}", "--coop-name", "${PLAYER_NAME}", "--coop-keys", keys],
    );
    join.set("more", true);
    out.push(join);
    if let Some(o) = &fl.offline {
        let (id, key, icon) = if o.local { ("local", "flash.local", "E765") } else { ("offline", "flash.offline", "E77B") };
        let label = if o.label.is_empty() { cfg.t(key, &[]) } else { o.label.clone() };
        let sub = if o.sub.is_empty() { cfg.t(&format!("{key}.sub"), &[("MAX", &max)]) } else { o.sub.clone() };
        let args = if o.keys.is_empty() { Vec::new() } else { vec!["--coop-keys", o.keys.as_str()] };
        let mut b = button(id, label, icon, sub, String::new(), args);
        b.set("offline", true);
        out.push(b);
    }
    if let Some((files, sub)) = &fl.original {
        let sub = if sub.is_empty() { cfg.t("flash.original.sub", &[]) } else { sub.clone() };
        let mut b = button("play", cfg.t("flash.original", &[]), "", sub, String::new(), Vec::new());
        b.set("edition", "original");
        b.set("swf", Value::Array(files.iter().map(|f| Value::from(f.as_str())).collect()));
        out.push(b);
    }
    Value::Array(out)
}

/// The launcher.json games[] entry a Flash record stands for: its card with the buttons above, online through Ruffle
/// Together's relay rooms.
fn flash_entry(mut entry: Value, cfg: &Config, fl: &FlashGame) -> Value {
    entry.set("actions", flash_actions(cfg, fl));
    if entry.find("online").is_none() {
        let mut on = Value::object();
        on.set("id", "ruffle-coop");
        on.set("join", "online");
        entry.set("online", on);
    }
    if entry.find("maxPlayers").is_none() {
        entry.set("maxPlayers", fl.max);
    }
    entry
}

/// A developer's Flash game (records/<id>.json, "kind": "flash"): the pinned release's game file, the buttons the
/// launcher makes, and the author's credit.
fn read_dev_flash_record(r: &Value, cfg: &Config, id: &str, repo: &str, local_release: bool) -> Result<GameSpec, String> {
    let (tag, version) = (text(r, "tag"), text(r, "version"));
    if !plain_segment(tag) {
        return Err("tag must be a plain name".into());
    }
    if !plain_segment(version) {
        return Err("version is missing".into());
    }
    check_card(r)?;
    let f = r.find("flash").ok_or("no \"flash\" object")?;
    let (fl, files) = read_dev_flash(f, id, repo, tag, local_release)?;
    let game = short_text(f.find("game"), "flash.game", 80)?;
    if game.is_empty() {
        return Err("flash.game: the game's name".into());
    }
    let mut entry = Value::object();
    entry.set("id", id);
    entry.set("name", game);
    for k in CARD_KEYS {
        if let Some(v) = r.find(k) {
            entry.set(k, v.clone());
        }
    }
    entry.set("categories", strings_value(&["flash"]));
    let entry = flash_entry(entry, cfg, &fl);
    let issues: Issues = Default::default();
    let mut g = read_game(&entry, &format!("community.{id}"), &issues);
    g.site_id = id.to_string();
    g.downloads = files;
    g.min_player = fl.ruffle;
    let author = match text(r, "author") {
        "" => repo.split('/').next().unwrap_or(repo).to_string(),
        a => a.to_string(),
    };
    let tip = cfg.t("flash.credit", &[("AUTHOR", &author), ("VERSION", version)]);
    credit(&mut g, repo, &author, &tip);
    checked(g, cfg, issues, false)
}

/// Every check a launcher.json game gets, on top of the record's own.
fn checked(g: GameSpec, cfg: &Config, issues: Issues, test_release: bool) -> Result<GameSpec, String> {
    let mut all: Vec<ConfigIssue> = issues.into_inner();
    let one = Config { games: vec![g.clone()], ..cfg.clone() };
    validate_config(&one, &mut all);
    // A loopback release (accepted only when mods-info is loopback too: the tests) isn't https.
    let loopback_url = |p: &str| test_release && p.contains(".package") && p.ends_with(".url");
    if let Some(e) = all.iter().find(|i| i.severity == Severity::Error && !loopback_url(&i.place)) {
        return Err(format!("{}: {}", e.place, e.message));
    }
    Ok(g)
}

/// records/: every mod in one list. The repository picks the rules (team_repo checks the owner), and the team's come
/// first, so a developer's record never takes one of their ids.
pub fn merge_records(cfg: &mut Config, cache: &Cache) -> Vec<String> {
    let is_team = |raw: &String| json::parse(raw.as_bytes()).is_ok_and(|r| team_repo(text(&r, "repo")));
    let (team, others): (Vec<String>, Vec<String>) = cache.raw.iter().cloned().partition(is_team);
    let mut left_out = merge_team(cfg, &Cache { fetched_at: cache.fetched_at, raw: team });
    left_out.extend(merge(cfg, &Cache { fetched_at: cache.fetched_at, raw: others }));
    left_out
}

/// Adds every record that passes to the games; the reasons the others were left out.
pub fn merge(cfg: &mut Config, cache: &Cache) -> Vec<String> {
    let local_release = local(&mods_info_base(cfg));
    let mut left_out = Vec::new();
    for raw in &cache.raw {
        match read_record(raw, cfg, local_release) {
            Ok(g) => cfg.games.push(g),
            Err(e) => left_out.push(e),
        }
    }
    left_out
}
