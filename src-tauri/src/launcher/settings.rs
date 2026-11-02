//! settings.json in the data folder (old/src/core/settings.cpp): favorites, play stats, the player and files chosen
//! per game, and the Settings window's choices. The window's commands in lib.rs write it; this reads it as the C++
//! launcher does, field by field, so a value of the wrong type falls back to its default instead of failing.

use std::collections::BTreeMap;

use super::json::{self, Value};
use super::paths;

#[derive(Clone, Debug, Default)]
pub struct GameStats {
    pub favorite: bool,
    pub plays: i32,
    pub seconds: i64,
    pub last_played: i64,
    pub player: String,
    pub swf: BTreeMap<String, String>, // action id -> file chosen with Locate
    pub game_dir: String,
    pub settings: BTreeMap<String, String>,
}

#[derive(Clone, Debug)]
pub struct UserData {
    pub games: BTreeMap<String, GameStats>,
    pub selected_game: String,
    pub category: String,
    pub minimize: i32, // -1: launcher.json's behavior.minimizeOnLaunch
    pub online: bool,
    pub default_player: String,
    pub games_dir: String,
    pub player_name: String,
    pub show_originals: bool,
    pub auto_update: bool,
    pub sort_mode: i32,
    pub compact_cards: bool,
    pub show_offline: bool,
}

impl Default for UserData {
    fn default() -> Self {
        UserData {
            games: BTreeMap::new(),
            selected_game: String::new(),
            category: String::new(),
            minimize: -1,
            online: true,
            default_player: String::new(),
            games_dir: String::new(),
            player_name: String::new(),
            show_originals: false,
            auto_update: true,
            sort_mode: 0,
            compact_cards: false,
            show_offline: false,
        }
    }
}

impl UserData {
    pub fn find(&self, game: &str) -> Option<&GameStats> {
        self.games.get(game)
    }
    pub fn stats(&mut self, game: &str) -> &mut GameStats {
        self.games.entry(game.to_string()).or_default()
    }
}

fn num(o: &Value, k: &str) -> f64 {
    o.find(k).and_then(Value::as_f64).unwrap_or(0.0)
}

fn flag(o: &Value, k: &str, out: &mut bool) {
    if let Some(b) = o.find(k).and_then(Value::as_bool) {
        *out = b;
    }
}

/// A damaged file is moved aside to settings.json.bad, as the C++ launcher does, so it can be looked at and isn't
/// silently overwritten by the next save.
pub fn load_user_data(path: &str) -> UserData {
    let mut d = UserData::default();
    let Some(text) = paths::read_file(path) else { return d };
    let root = match json::parse(&text) {
        Ok(v) if v.is_object() => v,
        _ => {
            let _ = std::fs::rename(path, format!("{path}.bad")).or_else(|_| {
                let _ = std::fs::remove_file(format!("{path}.bad"));
                std::fs::rename(path, format!("{path}.bad"))
            });
            return d;
        }
    };
    d.selected_game = root.text("selectedGame");
    d.category = root.text("category");
    if let Some(m) = root.find("minimize").and_then(Value::as_f64) {
        d.minimize = m as i32;
    }
    flag(&root, "online", &mut d.online);
    d.default_player = root.text("defaultPlayer");
    d.games_dir = root.text("gamesDir");
    d.player_name = root.text("playerName");
    flag(&root, "showOriginals", &mut d.show_originals);
    flag(&root, "autoUpdate", &mut d.auto_update);
    if let Some(m) = root.find("sortMode").and_then(Value::as_f64) {
        d.sort_mode = (m as i32).clamp(0, 3);
    }
    flag(&root, "compactCards", &mut d.compact_cards);
    flag(&root, "showOffline", &mut d.show_offline);
    if let Some(games) = root.find("games").filter(|g| g.is_object()) {
        for (id, g) in games.entries() {
            if !g.is_object() {
                continue;
            }
            let mut s = GameStats {
                favorite: g.find("favorite").and_then(Value::as_bool).unwrap_or(false),
                plays: num(g, "plays") as i32,
                seconds: num(g, "seconds") as i64,
                last_played: num(g, "lastPlayed") as i64,
                player: g.text("player"),
                game_dir: g.text("gameDir"),
                ..Default::default()
            };
            for (action, file) in g.find("swf").map(Value::entries).unwrap_or_default() {
                if let Some(f) = file.as_str() {
                    s.swf.insert(action.clone(), f.to_string());
                }
            }
            for (key, value) in g.find("settings").map(Value::entries).unwrap_or_default() {
                if let Some(v) = value.as_str() {
                    s.settings.insert(key.clone(), v.to_string());
                }
            }
            d.games.insert(id.clone(), s);
        }
    }
    d
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_what_the_window_writes_and_keeps_a_damaged_file() {
        let dir = crate::launcher::tests::TempDir::new("settings");
        let path = dir.join("data\\settings.json");
        std::fs::create_dir_all(dir.join("data")).unwrap();
        std::fs::write(
            &path,
            r#"{ "selectedGame": "g", "category": "cat:online", "minimize": 1, "online": false, "sortMode": 9,
                 "games": { "g": { "favorite": true, "plays": 3, "seconds": 4000, "lastPlayed": 1700000000, "player": "ruffle",
                                   "swf": { "host": "C:\\Games\\\u00e9t\u00e9.swf", "bad": 5 }, "settings": { "ff": "1" } },
                            "x": 5 } }"#,
        )
        .unwrap();
        let d = load_user_data(&path);
        assert_eq!(d.selected_game, "g");
        assert_eq!(d.minimize, 1);
        assert!(!d.online && d.auto_update);
        assert_eq!(d.sort_mode, 3);
        let g = d.find("g").unwrap();
        assert!(g.favorite && g.plays == 3 && g.seconds == 4000 && g.last_played == 1700000000);
        assert_eq!(g.player, "ruffle");
        assert_eq!(g.swf.get("host").map(String::as_str), Some("C:\\Games\\\u{e9}t\u{e9}.swf"));
        assert_eq!(g.swf.len(), 1);
        assert!(d.find("x").is_none());

        std::fs::write(&path, "{ not json").unwrap();
        let fresh = load_user_data(&path);
        assert!(fresh.games.is_empty());
        assert!(std::path::Path::new(&format!("{path}.bad")).is_file());
    }
}
