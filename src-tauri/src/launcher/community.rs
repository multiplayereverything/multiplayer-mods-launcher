//! Community mods the owner approved for the launcher (docs/COMMUNITY_MODS.md). A developer describes their mod in a
//! manifest in their own GitHub release; at approval the website pins the release and opens a pull request on
//! mods-info, and only the owner's merge publishes it: community/index.json lists the ids, community/<id>.json holds
//! each one's pinned release and install recipe. The launcher adds those games without an update of its own.
//!
//! The rules are old/src/core/config.cpp's `readCommunityMod`: a record may only use the keys listed here (anything
//! else is refused, not ignored), writes only inside the game folder, downloads only its pinned github.com release
//! with a SHA-256, and starts only through steam://. Then it gets every check a launcher.json mod gets. A record that
//! fails is left out whole. The copy kept in the data folder (community.json) is what the C++ mod commands read too.

use super::config::{ConfigIssue, Issues, Severity, add_mod_actions, read_mod, validate_config};
use super::config::{Config, GameSpec};
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

/// The ids community/index.json lists ({"mods": ["id", ...]}), at most 200, each once.
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

fn now_unix() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

/// Brings community.json up to date from mods-info, unless it is under an hour old. Ok(true) when the records changed.
/// A mods-info without a community folder yet (404) means no community mods; any other failure keeps the last copy.
pub fn refresh_cache(base: &str, cache_path: &str) -> Result<bool, String> {
    refresh_cache_within(base, cache_path, MAX_AGE_SECONDS)
}

/// The same with the saved copy's age limit given; 0 always asks mods-info (Ctrl+F5).
pub fn refresh_cache_within(base: &str, cache_path: &str, max_age: i64) -> Result<bool, String> {
    let saved = load_cache(cache_path);
    let now = now_unix();
    if saved.as_ref().is_some_and(|c| now >= c.fetched_at && now - c.fetched_at < max_age) {
        return Ok(false);
    }
    if !(base.starts_with("https://") || local(base)) {
        return Err("No https address for mods-info".into());
    }
    let ids = match http::get_text(&format!("{base}community/index.json")) {
        Ok(text) => parse_index(&text).ok_or("mods-info's community/index.json isn't a list of mods")?,
        Err(e) if e.contains("HTTP 404") => Vec::new(),
        Err(e) => return Err(e),
    };
    let mut fresh = Cache { fetched_at: now, raw: Vec::new() };
    for id in ids {
        // One record that can't be read is left out; the others still come.
        if let Ok(text) = http::get_text(&format!("{base}community/{id}.json")) {
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

fn check_shape(r: &Value, id: &str, repo: &str) -> Result<(), String> {
    let m = r.find("mod").filter(|m| m.is_object()).ok_or("no \"mod\" object")?;
    only_keys(m, &["id", "publisher", "website", "game", "install", "components", "launch"], "mod")?;
    if text(m, "id") != id {
        return Err("mod.id must be the record's id".into());
    }
    let site = text(m, "website");
    if !site.is_empty() && site != format!("https://github.com/{repo}") {
        return Err(format!("mod.website must be https://github.com/{repo}"));
    }
    let g = m.find("game").filter(|g| g.is_object()).ok_or("no mod.game")?;
    only_keys(g, &["name", "executable", "steamAppId", "validationFiles", "processNames", "detection"], "mod.game")?;
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
                "steam" => only_keys(rule, &["type", "appId", "installDir"], &place)?,
                "path" => only_keys(rule, &["type", "path"], &place)?,
                _ => return Err(format!("{place}: only \"steam\" and \"path\" rules")),
            }
        }
    }
    if let Some(inst) = m.find("install") {
        if !inst.is_object() {
            return Err("mod.install must be an object".into());
        }
        only_keys(inst, &["targetDir", "preserve", "requires", "uninstallCleanup"], "mod.install")?;
        let target = text(inst, "targetDir");
        if !target.is_empty() && (!starts_with_i(target, "${GAME_DIR}") || target.contains("..")) {
            return Err("mod.install.targetDir must be inside ${GAME_DIR}".into());
        }
        if let Some(req) = inst.find("requires").filter(|r| r.is_array()) {
            for r in req.items() {
                if !r.is_object() {
                    return Err("mod.install.requires: objects with path and message".into());
                }
                only_keys(r, &["path", "message"], "mod.install.requires")?;
            }
        }
    }
    let cs = m.find("components").filter(|c| c.is_array() && !c.items().is_empty()).ok_or("mod.components: at least one")?;
    for (i, c) in cs.items().iter().enumerate() {
        let place = format!("mod.components[{i}]");
        if !c.is_object() {
            return Err(format!("{place}: must be an object"));
        }
        only_keys(c, &["id", "name", "source", "target", "targetName", "required"], &place)?;
        if !inside_package(text(c, "source")) {
            return Err(format!("{place}.source: a path inside the package"));
        }
        let target = text(c, "target");
        if !target.is_empty() && !inside_game(target) {
            return Err(format!("{place}.target must be inside ${{GAME_DIR}}"));
        }
    }
    let l = m.find("launch").filter(|l| l.is_object()).ok_or("mod.launch is missing")?;
    only_keys(l, &["uri", "hint"], "mod.launch")?;
    let uri = text(l, "uri");
    let tail = if starts_with_i(uri, "steam://rungameid/") { &uri[18..] } else { "" };
    if !(tail == "${STEAM_APP_ID}" || (!tail.is_empty() && digits(tail))) {
        return Err("mod.launch.uri must be steam://rungameid/<app id>".into());
    }
    Ok(())
}

/// One community/<id>.json as a game of the launcher, or why it is left out. `local_release`: the release may come from
/// a local test server, only when mods-info itself is one.
pub fn read_record(text_: &str, cfg: &Config, local_release: bool) -> Result<GameSpec, String> {
    let r = json::parse(text_.as_bytes()).map_err(|e| format!("not a JSON object ({e})"))?;
    if !r.is_object() {
        return Err("not a JSON object".into());
    }
    only_keys(&r, &["id", "repo", "tag", "version", "asset", "url", "sha256", "manifest_sha256", "author", "authorUrl", "approved_at", "approved_by", "mod"], "record")?;
    let (id, repo, tag, asset) = (text(&r, "id"), text(&r, "repo"), text(&r, "tag"), text(&r, "asset"));
    let (version, url, sha) = (text(&r, "version"), text(&r, "url"), text(&r, "sha256"));
    if !id_ok(id) {
        return Err("id: lowercase letters, digits and '-', up to 48".into());
    }
    if !repo_name(repo) {
        return Err("repo must be \"owner/name\"".into());
    }
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
    let path = format!("/{repo}/releases/download/{tag}/{asset}");
    let mut url_ok = url == format!("https://github.com{path}");
    if !url_ok && local_release && url.starts_with("http://127.0.0.1:") {
        url_ok = url[17..].find('/').is_some_and(|at| url[17 + at..] == path);
    }
    if !url_ok {
        return Err(format!("url must be https://github.com{path}"));
    }
    if cfg.games.iter().any(|g| g.id == id || g.site_id == id || g.mod_spec.as_ref().is_some_and(|m| m.info.id == id)) {
        return Err(format!("launcher.json already has \"{id}\""));
    }
    check_shape(&r, id, repo)?;

    let m = r.find("mod").unwrap();
    let mut g = GameSpec {
        id: id.to_string(),
        site_id: id.to_string(),
        name: text(m.find("game").unwrap(), "name").to_string(), // a community card is named after the game
        categories: vec!["community".into()],
        ..Default::default()
    };
    let issues: Issues = Default::default();
    let mut spec = read_mod(m, &g, &format!("community.{id}.mod"), &issues);
    // The package is the one the approval pinned, never the record's to choose.
    spec.package.version = version.to_string();
    spec.package.url = url.to_string();
    spec.package.sha256 = sha.to_string();
    if spec.info.publisher.is_empty() {
        spec.info.publisher = text(&r, "author").to_string();
    }
    if spec.info.website.is_empty() {
        spec.info.website = format!("https://github.com/{repo}");
    }
    // The author's credit, as every community entry in launcher.json gives it. The description comes from the website
    // (library_json.rs), or says whose mod it is.
    let author = if spec.info.publisher.is_empty() { repo.split('/').next().unwrap_or(repo).to_string() } else { spec.info.publisher.clone() };
    g.guide.tips.push(format!(
        "Made by {author}, not by Multiplayer Everything. We install the exact version we looked at ({version}) and update it only after we have looked at the new one. Report problems to its author."
    ));
    g.guide.links.push(super::config::GuideLink { label: format!("{} by {author} on GitHub", repo.split('/').nth(1).unwrap_or(repo)), url: format!("https://github.com/{repo}") });
    g.mod_spec = Some(spec);
    add_mod_actions(&mut g);
    // Then every check a launcher.json mod gets.
    let mut all: Vec<ConfigIssue> = issues.into_inner();
    let one = Config { games: vec![g.clone()], ..cfg.clone() };
    validate_config(&one, &mut all);
    // A loopback release (only accepted above when mods-info is loopback too: the tests) isn't https.
    let test_release = local_release && url.starts_with("http://127.0.0.1:");
    if let Some(e) = all.iter().find(|i| i.severity == Severity::Error && !(test_release && i.place.ends_with(".package.url"))) {
        return Err(format!("{}: {}", e.place, e.message));
    }
    Ok(g)
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
