//! The game catalog (catalog.json of the flash-games repository, the copy the last session downloaded into the data
//! folder) and what the host picks before a Flash game starts (old/src/core/install.cpp, game_options.cpp): which
//! files have a newer build, the catalog's game settings, and Ruffle Together's co-op save option.

use std::cell::RefCell;
use std::collections::BTreeMap;

use super::config::{Config, GameSpec, PlayerSpec, Setting, read_settings};
use super::json::{self, Value};
use super::library::Library;
use super::paths;
use super::settings::UserData;
use super::text::{compare_versions, to_lower};

/// The release tag of a launcher-installed player is kept in this file inside its folder.
pub const PLAYER_VERSION_FILE: &str = ".version";

#[derive(Clone, Debug)]
pub struct CatalogFile {
    pub game: String,
    pub name: String,
    pub url: String,
    pub sha256: String,
    pub description: String,
    pub size: u64,
}

/// `base_url` is the catalog's own address; file paths are relative to its folder. A file name with a folder in it
/// could write outside the games folder, so such entries are dropped.
pub fn parse_catalog(text: &[u8], base_url: &str) -> Result<Vec<CatalogFile>, String> {
    let parsed = json::parse(text);
    let root = parsed.clone().unwrap_or_default();
    let files = root.find("files").filter(|f| f.is_array());
    let (Ok(_), Some(files)) = (&parsed, files) else {
        let detail = parsed.err().map(|e| format!(" ({e})")).unwrap_or_default();
        return Err(format!("The game catalog is not valid{detail}"));
    };
    let base = match base_url.rfind('/') {
        Some(p) => &base_url[..=p],
        None => "",
    };
    let mut out = Vec::new();
    for f in files.items() {
        let c = CatalogFile {
            game: f.text("game"),
            name: f.text("name"),
            sha256: to_lower(&f.text("sha256")),
            description: f.text("description"),
            size: f.find("size").and_then(Value::as_f64).map(|n| n as u64).unwrap_or(0),
            url: String::new(),
        };
        let path = f.text("path");
        if c.game.is_empty() || c.name.is_empty() || path.is_empty() || c.name.contains(['\\', '/', ':']) || c.name.contains("..") {
            continue;
        }
        let url = if path.starts_with("https://") || path.starts_with("http://") { path } else { format!("{base}{path}") };
        out.push(CatalogFile { url, ..c });
    }
    Ok(out)
}

/// The catalog's "settings": game id -> that game's settings. A game with no entry, or one that doesn't parse,
/// keeps the settings from launcher.json.
pub fn parse_catalog_settings(text: &[u8]) -> BTreeMap<String, Vec<Setting>> {
    let mut out = BTreeMap::new();
    let Ok(root) = json::parse(text) else { return out };
    let Some(all) = root.find("settings").filter(|a| a.is_object()) else { return out };
    for (game, list) in all.entries() {
        if !list.is_array() {
            continue;
        }
        let issues = RefCell::new(Vec::new());
        let settings = read_settings(list, &format!("catalog settings.{game}"), &issues);
        if !settings.is_empty() {
            out.insert(game.clone(), settings);
        }
    }
    out
}

pub fn cached_catalog_settings(data_dir: &str) -> BTreeMap<String, Vec<Setting>> {
    paths::read_file(&paths::join(data_dir, "catalog.json")).map(|t| parse_catalog_settings(&t)).unwrap_or_default()
}

/// Catalog files that exist in `dir` with a different SHA-256: a newer build is published.
pub fn outdated_game_files(catalog: &[CatalogFile], dir: &str) -> Vec<CatalogFile> {
    catalog
        .iter()
        .filter(|f| {
            let path = paths::join(dir, &f.name);
            !f.sha256.is_empty() && paths::is_file(&path) && paths::sha256_file(&path) != f.sha256
        })
        .cloned()
        .collect()
}

/// The release tag the launcher wrote next to a player it installed ("1.0.3.6"), empty for a copy it didn't install.
pub fn installed_player_version(exe: &str) -> String {
    if exe.is_empty() {
        return String::new();
    }
    let Some(tag) = paths::read_file(&paths::join(&paths::parent(exe), PLAYER_VERSION_FILE)) else { return String::new() };
    let have = String::from_utf8_lossy(&tag);
    let have = have.trim_end_matches(['\r', '\n', ' ']);
    have.strip_prefix(['v', 'V']).unwrap_or(have).to_string()
}

/// Ruffle Together's lockstep co-op games keep one co-op save per game file. The C++ launcher recognises them by
/// this online id; kept as it is so both agree on which games offer the save choice.
pub fn coop_save_game(g: &GameSpec) -> bool {
    g.online_id == "ruffle-coop"
}

/// Read-only over the config, the players found and the user's saved choices.
pub struct GameOptions<'a> {
    pub cfg: &'a Config,
    pub lib: &'a Library<'a>,
    pub user: &'a UserData,
    pub catalog: &'a BTreeMap<String, Vec<Setting>>,
}

impl<'a> GameOptions<'a> {
    /// The player that takes a co-op save option, if any.
    pub fn coop_save_player(&self) -> Option<&'a PlayerSpec> {
        self.cfg.players.iter().find(|p| !p.coop_save_arg.is_empty())
    }
    /// That player is installed and new enough.
    pub fn coop_save_supported(&self) -> bool {
        self.coop_save_player().is_some_and(|p| self.player_at_least(&p.coop_save_since))
    }
    pub fn player_at_least(&self, version: &str) -> bool {
        let Some(p) = self.coop_save_player() else { return false };
        let Some(ps) = self.lib.player(&p.id).filter(|s| s.found()) else { return false };
        if version.is_empty() {
            return true;
        }
        let have = installed_player_version(&ps.exe);
        !have.is_empty() && compare_versions(&have, version) >= 0
    }
    /// Where Ruffle Together keeps this game's co-op save.
    pub fn coop_save_file(&self, g: &GameSpec) -> String {
        for a in &g.actions {
            let Some(first) = a.swf.first() else { continue };
            let name = paths::file_name(&paths::normalize(&self.lib.vars().expand(first)));
            if name.is_empty() {
                continue;
            }
            return paths::join(&self.lib.vars().expand("${folder:LocalAppData}\\ruffle\\SharedObjects\\coop"), &format!("{name}.save"));
        }
        String::new()
    }
    /// The catalog's settings for the game, else launcher.json's.
    pub fn settings_for(&self, g: &'a GameSpec) -> &'a [Setting] {
        match self.catalog.get(&g.id) {
            Some(list) if !list.is_empty() => list,
            _ => &g.settings,
        }
    }
    /// "1"/"0" for a checkbox, the choice id for a choice; a choice the catalog has since removed falls back to
    /// the default.
    pub fn setting_value(&self, g: &GameSpec, s: &Setting) -> String {
        if let Some(v) = self.user.find(&g.id).and_then(|st| st.settings.get(&s.id)) {
            if s.kind == "checkbox" && (v == "1" || v == "0") {
                return v.clone();
            }
            if s.kind == "choice" && s.choices.iter().any(|c| c.id == *v) {
                return v.clone();
            }
        }
        if s.kind == "checkbox" {
            return if s.def == "on" { "1".into() } else { "0".into() };
        }
        if let Some(c) = s.choices.iter().find(|c| c.id == s.def) {
            return c.id.clone();
        }
        s.choices.first().map(|c| c.id.clone()).unwrap_or_default()
    }
    /// The settings' options for an action: host settings for an action that hosts, join ones for one that joins.
    pub fn setting_args(&self, g: &'a GameSpec, action_args: &[String]) -> Vec<String> {
        let starts = |prefix: &str| action_args.iter().any(|a| a.starts_with(prefix));
        let (hosting, joining) = (starts("--coop-host"), starts("--coop-join"));
        let mut out = Vec::new();
        for s in self.settings_for(g) {
            if !(s.scope == "all" || (s.scope == "host" && hosting) || (s.scope == "join" && joining)) {
                continue;
            }
            if !self.player_at_least(&s.since) {
                continue;
            }
            let v = self.setting_value(g, s);
            if s.kind == "checkbox" {
                if v == "1" {
                    out.extend(s.args.iter().cloned());
                }
            } else {
                for c in s.choices.iter().filter(|c| c.id == v) {
                    out.extend(c.args.iter().cloned());
                }
            }
        }
        out
    }
    /// Adds the host's save choice (0 continue, 1 from the solo save, 2 new game) and the game's settings to an
    /// action's arguments. "Continue" needs no option, and a player that doesn't know an option would refuse to
    /// start, so those wait for their release.
    pub fn add_host_options(&self, g: &'a GameSpec, args: &mut Vec<String>, save_choice: i32) {
        if coop_save_game(g) {
            let hosting = args.iter().any(|s| s.starts_with("--coop-host"));
            if hosting && save_choice > 0 && self.coop_save_supported() {
                args.push(self.coop_save_player().unwrap().coop_save_arg.clone());
                args.push(if save_choice == 1 { "solo".into() } else { "new".into() });
            }
        }
        let more = self.setting_args(g, args);
        args.extend(more);
    }
}
