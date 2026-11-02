//! What the website's games.json says about each game (old/src/core/site.cpp): stage, grade, version, what stays in
//! sync. The launcher keeps the last copy in its data folder (site.json) and folds it into its own list. The site is
//! not trusted with names or links: a game id, a repository and a cover path are each held to a strict shape
//! (docs/SECURITY_REVIEW.md), so the website can't steer a file name or a link the launcher uses.

use std::collections::BTreeMap;

use super::config::{Config, GameSpec, parse_color};
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
