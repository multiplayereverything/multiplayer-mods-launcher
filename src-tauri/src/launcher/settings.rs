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
    // The C++ window's placement: not used here, but kept so writing settings.json doesn't lose it.
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
    pub maximized: bool,
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
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            maximized: false,
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
    if let Some(w) = root.find("window").filter(|w| w.is_object()) {
        d.x = num(w, "x") as i32;
        d.y = num(w, "y") as i32;
        d.width = num(w, "width") as i32;
        d.height = num(w, "height") as i32;
        d.maximized = w.find("maximized").and_then(Value::as_bool).unwrap_or(false);
    }
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

/// Writes every field the C++ launcher writes, in its order and layout, beside-and-move so a crash never leaves
/// half a file. Keys it doesn't know are dropped, as saveUserData dropped them.
pub fn save_user_data(d: &UserData, path: &str) -> Result<(), String> {
    let mut root = Value::object();
    root.set("selectedGame", d.selected_game.as_str());
    root.set("category", d.category.as_str());
    root.set("minimize", d.minimize);
    root.set("online", d.online);
    root.set("defaultPlayer", d.default_player.as_str());
    root.set("gamesDir", d.games_dir.as_str());
    root.set("playerName", d.player_name.as_str());
    root.set("showOriginals", d.show_originals);
    root.set("autoUpdate", d.auto_update);
    root.set("sortMode", d.sort_mode);
    root.set("compactCards", d.compact_cards);
    root.set("showOffline", d.show_offline);
    let mut w = Value::object();
    w.set("x", d.x);
    w.set("y", d.y);
    w.set("width", d.width);
    w.set("height", d.height);
    w.set("maximized", d.maximized);
    root.set("window", w);
    let mut games = Value::object();
    for (id, s) in &d.games {
        let mut g = Value::object();
        g.set("favorite", s.favorite);
        g.set("plays", s.plays);
        g.set("seconds", s.seconds);
        g.set("lastPlayed", s.last_played);
        if !s.player.is_empty() {
            g.set("player", s.player.as_str());
        }
        if !s.game_dir.is_empty() {
            g.set("gameDir", s.game_dir.as_str());
        }
        if !s.swf.is_empty() {
            let mut swf = Value::object();
            for (action, file) in &s.swf {
                swf.set(action, file.as_str());
            }
            g.set("swf", swf);
        }
        if !s.settings.is_empty() {
            let mut st = Value::object();
            for (k, v) in &s.settings {
                st.set(k, v.as_str());
            }
            g.set("settings", st);
        }
        games.set(id, g);
    }
    root.set("games", games);
    let text = json::serialize(&root, true);
    let dir = paths::parent(path);
    if !paths::ensure_dir(&dir) {
        return Err(format!("Cannot create {dir}"));
    }
    let tmp = format!("{path}.tmp");
    let done = std::fs::write(&tmp, text.as_bytes()).and_then(|_| std::fs::rename(&tmp, path));
    done.map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        e.to_string()
    })
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

    #[test]
    fn settings_round_trip_in_the_cpp_layout() {
        let dir = crate::launcher::tests::TempDir::new("settings-save");
        let path = dir.join("data\\settings.json");
        let mut d = UserData { selected_game: "gunmayhem2".into(), category: "cat:online".into(), width: 1200, maximized: true, ..Default::default() };
        let s = d.stats("gunmayhem2");
        s.favorite = true;
        s.plays = 3;
        s.seconds = 4000;
        s.last_played = 1700000000;
        s.player = "ruffle".into();
        s.swf.insert("host".into(), "C:\\Games\\\u{e9}t\u{e9}.swf".into());
        save_user_data(&d, &path).unwrap();
        let back = load_user_data(&path);
        assert_eq!(back.selected_game, "gunmayhem2");
        assert_eq!(back.category, "cat:online");
        assert!(back.width == 1200 && back.maximized);
        let b = back.find("gunmayhem2").unwrap();
        assert!(b.favorite && b.plays == 3 && b.seconds == 4000 && b.last_played == 1700000000);
        assert_eq!(b.player, "ruffle");
        assert_eq!(b.swf["host"], "C:\\Games\\\u{e9}t\u{e9}.swf");
        // The C++ writer's layout: two-space indents, its field order, a final newline.
        let text = std::fs::read_to_string(&path).unwrap();
        assert!(text.starts_with("{\n  \"selectedGame\": \"gunmayhem2\",\n  \"category\""), "{text}");
        assert!(text.contains("\"window\": {\n    \"x\": 0,") && text.ends_with("}\n"));
    }
}
