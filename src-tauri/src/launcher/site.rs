//! What the website's games.json says about each game (old/src/core/site.cpp): stage, grade, version, what stays in
//! sync. The launcher keeps the last copy in its data folder (site.json) and folds it into its own list. The site is
//! not trusted with names or links: a game id, a repository and a cover path are each held to a strict shape
//! (docs/SECURITY_REVIEW.md), so the website can't steer a file name or a link the launcher uses.

use std::collections::BTreeMap;

use super::config::{Config, GameSpec, parse_color};
use super::http;
use super::json::{self, Value};
use super::paths;
use super::text::to_lower;

#[derive(Clone, Debug)]
pub struct SiteGame {
    pub id: String,
    pub mod_id: String,
    pub kind: String,
    pub name: String,
    pub status: String,
    pub quality: String,
    pub version: String,
    pub updated: String,
    pub connect: String,
    pub maintainer: String,
    pub engine: String,
    pub description: String,
    pub players: String,
    pub platform: String,
    pub color: String,
    pub color_end: String,
    pub cover: String,
    pub repo: String,
    pub community: bool,
    pub reviewed: bool,
    pub author: String,
    pub author_url: String,
    pub slots: i32,
    pub cover_focus: f32,
    pub synced: BTreeMap<String, String>,
    pub progress: BTreeMap<String, String>,
}

/// Letters, digits and `extra`; the C++ check also lets a NUL byte through (strchr finds the terminator), kept so
/// both read the same files the same way.
fn only_chars(s: &str, extra: &[u8], max_len: usize) -> bool {
    !s.is_empty() && s.len() <= max_len && s.bytes().all(|c| c.is_ascii_alphanumeric() || c == 0 || extra.contains(&c))
}

fn string_map(o: &Value, key: &str) -> BTreeMap<String, String> {
    o.find(key)
        .filter(|v| v.is_object())
        .map(|v| v.entries().iter().filter_map(|(k, v)| v.as_str().map(|s| (k.clone(), s.to_string()))).collect())
        .unwrap_or_default()
}

/// "owner/name" of letters, digits, '-', '_' and '.'.
pub fn is_github_repo(repo: &str) -> bool {
    let Some((owner, name)) = repo.split_once('/') else { return false };
    let part = |p: &str| only_chars(p, b"-_.", 100) && p != "." && p != "..";
    !name.contains('/') && part(owner) && part(name)
}

/// "covers/<name>.png|jpg|webp" and nothing else.
pub fn is_site_cover(path: &str) -> bool {
    let dir = "covers/";
    if path.len() <= dir.len() + 4 || !path.starts_with(dir) {
        return false;
    }
    let name = &path[dir.len()..];
    let Some(dot) = name.rfind('.') else { return false };
    let ext = name[dot..].to_ascii_lowercase();
    if ext != ".png" && ext != ".jpg" && ext != ".webp" {
        return false;
    }
    only_chars(&name[..dot], b"-_.", 80) && !name.contains("..")
}

/// Exactly https://github.com/<owner>/<name>.
pub fn is_github_repo_url(url: &str) -> bool {
    url.strip_prefix("https://github.com/").is_some_and(is_github_repo)
}

/// The site's list; None when the text isn't a games.json.
pub fn parse_site(text: &[u8]) -> Option<Vec<SiteGame>> {
    let root = json::parse(text).unwrap_or_default();
    let games = root.find("games").filter(|g| g.is_array())?;
    let mut out = Vec::new();
    for g in games.items() {
        if !g.is_object() {
            continue;
        }
        let id = g.text("id");
        // The id becomes part of a file name (the cover) and of a game id here.
        if !only_chars(&id, b"-_", 48) {
            continue;
        }
        let cover = g.text("cover");
        let repo = g.text("repo");
        let author_url = g.text("authorUrl");
        let origin = g.text("origin");
        out.push(SiteGame {
            id,
            mod_id: g.text("modId"),
            kind: g.text("kind"),
            name: g.text("name"),
            status: g.text("status"),
            quality: g.text("quality"),
            version: g.text("version"),
            updated: g.text("updated"),
            connect: g.text("connect"),
            maintainer: g.text("maintainer"),
            engine: g.text("engine"),
            description: g.text("description"),
            players: g.text("players"),
            platform: g.text("platform"),
            color: g.text("color"),
            color_end: g.text("colorEnd"),
            cover: if is_site_cover(&cover) { cover } else { String::new() },
            repo: if is_github_repo(&repo) { repo } else { String::new() },
            // No origin is official; any other value reads as community, the careful reading.
            community: !origin.is_empty() && origin != "official",
            reviewed: g.find("reviewed").and_then(Value::as_bool).unwrap_or(false),
            author: g.text("author"),
            author_url: if is_github_repo_url(&author_url) { author_url } else { String::new() },
            slots: g.find("slots").and_then(Value::as_f64).map(|n| n as i32).unwrap_or(0),
            cover_focus: g.find("coverFocus").and_then(Value::as_f64).map(|n| (n as f32).clamp(0.0, 1.0)).unwrap_or(0.5),
            synced: string_map(g, "synced"),
            progress: string_map(g, "progress"),
        });
    }
    Some(out)
}

/// Where the website lists the community projects it published: /api/projects beside games.json, in the same shape.
pub fn projects_url(site_url: &str) -> String {
    site_url.rfind('/').map(|i| format!("{}api/projects", &site_url[..=i])).unwrap_or_default()
}

/// The website's community projects (/api/projects) as site entries: every one is someone else's mod, credited to its
/// maintainer and repository.
pub fn mark_community(list: &mut [SiteGame]) {
    for s in list {
        s.community = true;
        if s.author.is_empty() {
            s.author = s.maintainer.clone();
        }
        if s.author_url.is_empty() && !s.repo.is_empty() {
            s.author_url = format!("https://github.com/{}", s.repo);
        }
    }
}

/// A project the website lists that the launcher can't install yet (no approved record in mods-info) becomes a card
/// in its platform's list that says so; one it can install gets the project's description, players and cover.
pub fn merge_projects(cfg: &mut Config, list: &[SiteGame], cover_dir: &str) {
    let before = cfg.games.len();
    merge_site(cfg, list, cover_dir);
    for g in &mut cfg.games[before..] {
        let s = list.iter().find(|s| s.id == g.id);
        let flash = s.is_some_and(|s| s.platform.to_lowercase().contains("flash"));
        g.categories = vec![if flash { "flash" } else { "pc" }.to_string()];
        g.community = true;
        if let Some(s) = s {
            g.author = s.author.clone();
            g.author_url = s.author_url.clone();
        }
        if g.description == "A multiplayer mod we are planning." {
            g.description = "A community mod listed on the website. It can't be installed from the launcher yet.".into();
        }
    }
}

/// The entry for a launcher game: its own `site` id, else its id, else the id its mod has on the server.
pub fn find_site<'a>(list: &'a [SiteGame], site_id: &str, game_id: &str, mod_id: &str) -> Option<usize> {
    let by = |id: &str, by_mod: bool| {
        if id.is_empty() {
            return None;
        }
        list.iter().position(|s| (if by_mod { &s.mod_id } else { &s.id }) == id)
    };
    by(site_id, false).or_else(|| by(game_id, false)).or_else(|| by(mod_id, true))
}

/// The last answer kept on disk: when it was fetched and games.json exactly as it came.
pub struct SiteCache {
    pub fetched_at: i64,
    pub raw: String,
}

impl SiteCache {
    pub fn fresh(&self, now: i64, max_age: i64) -> bool {
        !self.raw.is_empty() && now >= self.fetched_at && now - self.fetched_at < max_age
    }
}

pub fn load_site_cache(path: &str) -> Option<SiteCache> {
    let root = json::parse(&paths::read_file(path)?).ok()?;
    let at = root.find("at").and_then(Value::as_f64)?;
    let raw = root.find("games").and_then(Value::as_str)?;
    Some(SiteCache { fetched_at: at as i64, raw: raw.to_string() })
}

pub fn save_site_cache(path: &str, cache: &SiteCache) -> bool {
    let text = serde_json::to_string_pretty(&serde_json::json!({ "at": cache.fetched_at, "games": cache.raw })).unwrap_or_default();
    paths::ensure_dir(&paths::parent(path));
    paths::write_file(path, format!("{text}\n").as_bytes())
}

/// How long a copy of games.json is used before the website is asked again.
pub const MAX_AGE_SECONDS: i64 = 3600;

fn now_unix() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

fn fetchable(url: &str) -> bool {
    let low = to_lower(url);
    low.starts_with("https://") || low.starts_with("http://127.0.0.1")
}

/// games.json from the website, unless the saved copy is under an hour old (old/src/core/site.cpp `loadSite`). Ok(true)
/// when a copy that differs from the saved one came; Ok(false) when the saved copy is still fresh or the same. A failed
/// fetch keeps the saved copy and says why.
pub fn refresh_site_cache(url: &str, cache_path: &str) -> Result<bool, String> {
    refresh_site_cache_within(url, cache_path, MAX_AGE_SECONDS)
}

/// The same with the saved copy's age limit given; 0 always asks the website (Ctrl+F5).
pub fn refresh_site_cache_within(url: &str, cache_path: &str, max_age: i64) -> Result<bool, String> {
    let saved = load_site_cache(cache_path).filter(|c| parse_site(c.raw.as_bytes()).is_some());
    let now = now_unix();
    if saved.as_ref().is_some_and(|c| c.fresh(now, max_age)) {
        return Ok(false);
    }
    if !fetchable(url) {
        return Err("No https address for the website's data".into());
    }
    let body = http::get_text(url)?;
    if parse_site(&body).is_none() {
        return Err("The website's data isn't in the expected shape".into());
    }
    let raw = String::from_utf8_lossy(&body).to_string();
    let changed = saved.as_ref().is_none_or(|c| c.raw != raw);
    save_site_cache(cache_path, &SiteCache { fetched_at: now, raw });
    Ok(changed)
}

/// A cover picture from the website, kept only when it is a PNG, JPEG or WebP of at most 3 MB (install.cpp
/// `downloadImage`); written beside its place first, so a cut-off download never shows.
pub fn download_image(url: &str, dest: &str) -> Result<(), String> {
    if !fetchable(url) {
        return Err("Pictures are only fetched over https".into());
    }
    let data = http::get(url, 3 * 1024 * 1024).map_err(|e| if e.starts_with("Could not write") { "The picture is too large".into() } else { e })?;
    let png = data.len() > 8 && data.starts_with(&[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]);
    let jpg = data.len() > 3 && data.starts_with(&[0xFF, 0xD8]);
    let webp = data.len() > 12 && data.starts_with(b"RIFF") && &data[8..12] == b"WEBP";
    if !png && !jpg && !webp {
        return Err("That isn't a PNG, JPEG or WebP picture".into());
    }
    let part = format!("{dest}.part");
    paths::ensure_dir(&paths::parent(dest));
    if !paths::write_file(&part, &data) || paths::move_file(&part, dest, true).is_err() {
        paths::delete_file(&part);
        return Err(format!("Cannot write {dest}"));
    }
    Ok(())
}

/// The covers a merged list shows from the website and doesn't have yet (site.cpp `fetchSiteCovers`), fetched from
/// beside games.json; how many came.
pub fn fetch_site_covers(merged: &Config, list: &[SiteGame], site_url: &str, cover_dir: &str) -> usize {
    let Some(slash) = site_url.rfind('/') else { return 0 };
    let base = &site_url[..=slash];
    let mut got = 0;
    for g in &merged.games {
        let Some(k) = find_site(list, &g.site_id, &g.id, &g.online_id) else { continue };
        let s = &list[k];
        if s.cover.is_empty() {
            continue;
        }
        let dest = site_cover_file(cover_dir, s);
        if g.image != dest || paths::is_file(&dest) {
            continue; // not using the website's cover, or already has it
        }
        if download_image(&format!("{base}{}", s.cover), &dest).is_ok() {
            got += 1;
        }
    }
    got
}

/// Where the cover of site game `s` is kept: <coverDir>\site-<id>.png (or .jpg, .webp).
pub fn site_cover_file(cover_dir: &str, s: &SiteGame) -> String {
    let tail = to_lower(if s.cover.len() > 5 { &s.cover[s.cover.len() - 5..] } else { &s.cover });
    let ext = if tail == ".webp" {
        ".webp"
    } else if tail.len() >= 4 && tail.ends_with(".jpg") {
        ".jpg"
    } else {
        ".png"
    };
    paths::join(cover_dir, &format!("site-{}{ext}", s.id))
}

/// Folds the website's list into the launcher's own: a game the website lists that the launcher doesn't have
/// becomes a "Coming soon" card, and a launcher game that leaves out its description, player count or cover gets
/// the site's.
pub fn merge_site(cfg: &mut Config, list: &[SiteGame], cover_dir: &str) {
    let mut used = vec![false; list.len()];
    for g in &mut cfg.games {
        let Some(k) = find_site(list, &g.site_id, &g.id, &g.online_id) else { continue };
        let s = &list[k];
        used[k] = true;
        if g.description.is_empty() {
            g.description = s.description.clone();
        }
        if g.player_count.is_empty() {
            g.player_count = s.players.clone();
        }
        if g.max_players == 0 && s.slots > 0 {
            g.max_players = s.slots;
        }
        if g.image.is_empty() && !s.cover.is_empty() {
            g.image = site_cover_file(cover_dir, s);
            g.image_focus = s.cover_focus;
        }
    }
    for (i, s) in list.iter().enumerate() {
        if used[i] || s.name.is_empty() {
            continue;
        }
        let mut g = GameSpec {
            id: format!("site-{}", s.id),
            site_id: s.id.clone(),
            name: s.name.clone(),
            description: if s.description.is_empty() { "A multiplayer mod we are planning.".into() } else { s.description.clone() },
            player_count: s.players.clone(),
            max_players: s.slots,
            coming_soon: true, // the launcher has no install for it, whatever stage the site says it is in
            categories: vec![if s.kind == "pc" { "pc".into() } else { "flash".into() }],
            ..Default::default()
        };
        if let Some(c) = parse_color(&s.color) {
            g.color = c;
        }
        if let Some(c) = parse_color(&s.color_end) {
            g.color_end = c;
        }
        if !s.cover.is_empty() {
            g.image = site_cover_file(cover_dir, s);
            g.image_focus = s.cover_focus;
        }
        cfg.games.push(g);
    }
}
