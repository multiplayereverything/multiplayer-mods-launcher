//! `${NAME}` variables in launcher.json strings (old/src/core/vars.cpp): the launcher's own (GAMES_DIR, SWF...),
//! `${env:X}` and `${folder:Documents}`. Names are case-insensitive; an unknown one stays as written.

use std::collections::BTreeMap;

use super::paths;
use super::text::{starts_with_i, to_upper};

#[derive(Clone, Debug, Default)]
pub struct Vars {
    values: BTreeMap<String, String>, // keys upper-case
}

impl Vars {
    pub fn set(&mut self, name: &str, value: &str) {
        self.values.insert(to_upper(name), value.to_string());
    }
    pub fn get(&self, name: &str) -> String {
        self.values.get(&to_upper(name)).cloned().unwrap_or_default()
    }
    pub fn has(&self, name: &str) -> bool {
        self.values.contains_key(&to_upper(name))
    }
    pub fn expand(&self, text: &str) -> String {
        self.expand_depth(text, 0, &mut None)
    }
    /// Expands and lists the names it couldn't (so a candidate path that needs them can be skipped).
    pub fn expand_unknown(&self, text: &str, unknown: &mut Vec<String>) -> String {
        let mut u = Some(std::mem::take(unknown));
        let out = self.expand_depth(text, 0, &mut u);
        *unknown = u.unwrap_or_default();
        out
    }

    fn expand_depth(&self, text: &str, depth: i32, unknown: &mut Option<Vec<String>>) -> String {
        let mut out = String::new();
        let mut i = 0;
        while i < text.len() {
            let Some(start) = text[i..].find("${").map(|p| p + i) else {
                out.push_str(&text[i..]);
                break;
            };
            let Some(end) = text[start + 2..].find('}').map(|p| p + start + 2) else {
                out.push_str(&text[i..]);
                break;
            };
            out.push_str(&text[i..start]);
            let name = &text[start + 2..end];
            let value: Option<String> = if starts_with_i(name, "env:") {
                std::env::var_os(name.chars().skip(4).collect::<String>()).map(|v| v.to_string_lossy().into_owned())
            } else if starts_with_i(name, "folder:") {
                Some(paths::known_folder(&name.chars().skip(7).collect::<String>())).filter(|v| !v.is_empty())
            } else {
                self.values.get(&to_upper(name)).map(|v| if depth < 8 { self.expand_depth(v, depth + 1, unknown) } else { v.clone() })
            };
            match value {
                Some(v) => out.push_str(&v),
                None => {
                    out.push_str(&text[start..=end]);
                    if let Some(u) = unknown.as_mut() {
                        u.push(name.to_string());
                    }
                }
            }
            i = end + 1;
        }
        out
    }
}

/// Every name a config string may use (without `${}`), for the unknown-variable warnings.
pub const KNOWN_NAMES: &[&str] = &[
    "LAUNCHER_DIR", "GAMES_DIR", "PLAYERS_DIR", "DATA_DIR", "FLASHPOINT", "GAME_ID", "GAME_NAME", "SWF", "SWF_DIR",
    "PORT", "TITLE", "SUBTITLE", "URL", "FILE", "FILE_DIR", "PLAYER_NAME", "CODE", "SERVER", "ADDRESS", "MOD_ID",
    "MOD_NAME", "MOD_VERSION", "MOD_PUBLISHER", "MOD_WEBSITE", "MOD_SUPPORT_URL", "MOD_DESCRIPTION", "GAME_EXE",
    "STEAM_APP_ID", "GAME_DIR", "INSTALL_DIR", "STATE_DIR", "INSTALLED_VERSION",
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn expands_nested_env_and_leaves_unknown() {
        let mut v = Vars::default();
        v.set("launcher_dir", "C:\\L");
        v.set("GAMES_DIR", "${LAUNCHER_DIR}\\games");
        let mut unknown = Vec::new();
        assert_eq!(v.expand_unknown("${games_dir}\\x ${NOPE} ${", &mut unknown), "C:\\L\\games\\x ${NOPE} ${");
        assert_eq!(unknown, vec!["NOPE".to_string()]);
        unsafe { std::env::set_var("FL_VARS_TEST", "yes") };
        assert_eq!(v.expand("${env:FL_VARS_TEST}"), "yes");
        assert!(!v.expand("${folder:Documents}").contains("${"));
    }
}
