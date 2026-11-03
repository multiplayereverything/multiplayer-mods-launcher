//! launcher.json (old/src/core/config.cpp): every game, player, path, color and visible string, read into these
//! structs with the C++ launcher's defaults, its type errors and its unknown-key warnings, then cross-checked.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet, HashSet};

use super::json::{self, Type, Value};
use super::paths;
use super::text::{iequals, starts_with_i, to_lower, trim};
use super::texts::DEFAULT_TEXTS;
use super::vars::KNOWN_NAMES;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl Color {
    pub const fn rgb(v: u32) -> Color {
        Color { r: (v >> 16) as u8, g: (v >> 8) as u8, b: v as u8, a: 255 }
    }
    pub fn hex(&self) -> String {
        format!("#{:02X}{:02X}{:02X}", self.r, self.g, self.b)
    }
}

/// "#4F8BFF", "#48F" or "#4F8BFFCC".
pub fn parse_color(text: &str) -> Option<Color> {
    let s = trim(text).strip_prefix('#')?;
    if !s.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let hex = |pos: usize, len: usize| {
        let v = u32::from_str_radix(&s[pos..pos + len], 16).unwrap_or(0) * if len == 1 { 17 } else { 1 };
        v as u8
    };
    match s.len() {
        3 => Some(Color { r: hex(0, 1), g: hex(1, 1), b: hex(2, 1), a: 255 }),
        6 | 8 => Some(Color { r: hex(0, 2), g: hex(2, 2), b: hex(4, 2), a: if s.len() == 8 { hex(6, 2) } else { 255 } }),
        _ => None,
    }
}

/// The window palette names, in the C++ order; launcher.json's branding.colors may set any of them.
pub const COLOR_FIELDS: [&str; 17] = [
    "background", "surface", "surfaceHover", "sidebar", "sidebarText", "sidebarTextMuted", "primary", "primaryHover",
    "onPrimary", "text", "textMuted", "border", "focus", "success", "warning", "error", "online",
];

pub fn dark_palette() -> [Color; 17] {
    [
        0x0B1119, 0x141B26, 0x1F2835, 0x070C13, 0xF2F4F7, 0x8E9BAE, 0xC8313A, 0xB42A33, 0xFFFFFF, 0xE6EBF2, 0x93A0B3,
        0x253041, 0x38BDF8, 0x3CCB7F, 0xF2B544, 0xFF6464, 0x38BDF8,
    ]
    .map(Color::rgb)
}

pub fn light_palette() -> [Color; 17] {
    [
        0xFFFFFF, 0xF4F6F9, 0xE9EDF3, 0x1B2030, 0xF2F4F7, 0xA3ABBA, 0xB42A33, 0x9C232B, 0xFFFFFF, 0x1C2128, 0x5B6472,
        0xD5DAE1, 0x2F6FEB, 0x1F9D57, 0xB7791F, 0xD93636, 0x0E9C90,
    ]
    .map(Color::rgb)
}

#[derive(Clone, Debug)]
pub struct WindowSpec {
    pub width: i32,
    pub height: i32,
    pub min_width: i32,
    pub min_height: i32,
}

#[derive(Clone, Debug)]
pub struct Branding {
    pub title: String,
    pub subtitle: String,
    pub website: String,
    pub window_title: String,
    pub theme: String,
    pub colors: [Color; 17],
    pub font: String,
    pub display_font: String,
    pub font_size: f32,
    pub player_colors: Vec<Color>,
    pub window: WindowSpec,
}

impl Default for Branding {
    fn default() -> Self {
        Branding {
            title: "Multiplayer Everything".into(),
            subtitle: "Multiplayer Everything Launcher".into(),
            website: "https://github.com/multiplayereverything/multiplayer-mods-launcher".into(),
            window_title: "${TITLE} ${SUBTITLE}".into(),
            theme: "dark".into(),
            colors: dark_palette(),
            font: "Segoe UI".into(),
            display_font: "Bahnschrift".into(),
            font_size: 10.0,
            player_colors: [0xE5484D, 0x3E7BFA, 0x2EBD6B, 0xF5B83D].map(Color::rgb).to_vec(),
            window: WindowSpec { width: 1180, height: 720, min_width: 720, min_height: 520 },
        }
    }
}

#[derive(Clone, Debug)]
pub struct Paths {
    pub games: String,
    pub players: String,
    pub data: String,
    pub flashpoint: Vec<String>,
}

impl Default for Paths {
    fn default() -> Self {
        Paths {
            games: "${LAUNCHER_DIR}\\games".into(),
            players: "${LAUNCHER_DIR}\\players".into(),
            data: "${env:LOCALAPPDATA}\\MultiplayerEverything\\MultiplayerEverythingLauncher".into(),
            flashpoint: vec!["C:\\Flashpoint".into(), "D:\\Flashpoint".into(), "${folder:Profile}\\Flashpoint".into()],
        }
    }
}

#[derive(Clone, Debug)]
pub struct Rooms {
    pub servers: String,
    pub server: String,
    pub refresh_seconds: i32,
}

#[derive(Clone, Debug)]
pub struct Downloads {
    pub catalog: String,
    pub mods_info: String,
    pub site: String,
}

#[derive(Clone, Debug)]
pub struct Behavior {
    pub minimize_on_launch: bool,
    pub stop_relay_with_game: bool,
    pub quick_exit_seconds: i32,
}

/// Where a missing player comes from: a GitHub release asset, or a fixed URL.
#[derive(Clone, Debug, Default)]
pub struct InstallSpec {
    pub github: String,
    pub api: String,
    pub asset: String,
    pub url: String,
    pub sha256: String,
    pub dir: String,
    pub exe: String,
}

impl InstallSpec {
    pub fn enabled(&self) -> bool {
        !self.github.is_empty() || !self.url.is_empty()
    }
}

#[derive(Clone, Debug)]
pub struct PlayerSpec {
    pub id: String,
    pub kind: String, // "flash" | "browser"
    pub name: String,
    pub website: String,
    pub install: InstallSpec,
    pub paths: Vec<String>,
    pub args: Vec<String>,
    pub flashvar_arg: String,
    pub coop_save_arg: String,
    pub coop_save_since: String,
    pub tv_args: Vec<String>,
    pub tv_env: Vec<(String, String)>,
}

#[derive(Clone, Debug, Default)]
pub struct PlayerOverride {
    pub swf: Vec<String>,
    pub args: Vec<String>,
    pub has_swf: bool,
    pub has_args: bool,
}

#[derive(Clone, Debug)]
pub struct RelaySpec {
    pub enabled: bool,
    pub port: i32,
}

#[derive(Clone, Debug)]
pub struct ServerSpec {
    pub name: String,
    pub website: String,
    pub exe: Vec<String>,
    pub args: Vec<String>,
    pub cwd: String,
    pub url: String,
    pub share: String,
    pub lan: String,
}

impl ServerSpec {
    pub fn enabled(&self) -> bool {
        !self.exe.is_empty()
    }
}

#[derive(Clone, Debug)]
pub struct ActionSpec {
    pub id: String,
    pub label: String,
    pub description: String,
    pub hint: String,
    pub swf: Vec<String>,
    pub page: Vec<String>,
    pub server: ServerSpec,
    pub join_link: bool,
    pub args: Vec<String>,
    pub join_args: Vec<String>,
    pub players: Vec<String>,
    pub overrides: BTreeMap<String, PlayerOverride>,
    pub relay: RelaySpec,
    pub flashvars: Vec<(String, String)>,
    pub mod_op: String,
    pub more: bool,
    pub offline: bool,
    pub icon: String,
    pub original: bool,
    pub link: String,
}

impl Default for ActionSpec {
    fn default() -> Self {
        ActionSpec {
            id: String::new(),
            label: String::new(),
            description: String::new(),
            hint: String::new(),
            swf: Vec::new(),
            page: Vec::new(),
            server: ServerSpec {
                name: "Server".into(),
                website: String::new(),
                exe: Vec::new(),
                args: Vec::new(),
                cwd: String::new(),
                url: String::new(),
                share: String::new(),
                lan: String::new(),
            },
            join_link: false,
            args: Vec::new(),
            join_args: Vec::new(),
            players: Vec::new(),
            overrides: BTreeMap::new(),
            relay: RelaySpec { enabled: false, port: 7777 },
            flashvars: Vec::new(),
            mod_op: String::new(),
            more: false,
            offline: false,
            icon: String::new(),
            original: false,
            link: String::new(),
        }
    }
}

impl ActionSpec {
    pub fn is_web(&self) -> bool {
        !self.page.is_empty() || self.server.enabled() || self.join_link
    }
    pub fn is_mod(&self) -> bool {
        !self.mod_op.is_empty()
    }
    pub fn is_link(&self) -> bool {
        !self.link.is_empty()
    }
}

#[derive(Clone, Debug)]
pub struct LobbySpec {
    pub enabled: bool,
    pub version: String,
    pub port: i32,
}

#[derive(Clone, Debug)]
pub struct GuideLink {
    pub label: String,
    pub url: String,
}

#[derive(Clone, Debug, Default)]
pub struct GuideSpec {
    pub needs: Vec<String>,
    pub steps: Vec<String>,
    pub tips: Vec<String>,
    pub keys: Vec<(String, String)>,
    pub links: Vec<GuideLink>,
}

#[derive(Clone, Debug)]
pub struct SettingChoice {
    pub id: String,
    pub label: String,
    pub args: Vec<String>,
}

/// A game setting (a checkbox or a choice) that adds options to the command line.
#[derive(Clone, Debug)]
pub struct Setting {
    pub id: String,
    pub kind: String,  // "checkbox" | "choice"
    pub scope: String, // "host" | "join" | "all"
    pub label: String,
    pub since: String,
    pub args: Vec<String>,
    pub choices: Vec<SettingChoice>,
    pub def: String,
}

pub const MAX_SETTINGS: usize = 4;
pub const GUIDE_MAX_LINKS: usize = 4;

/// A PC game mod (old/src/core/mod/spec.h); the library needs its names, the rest is read so the checks match.
#[derive(Clone, Debug, Default)]
pub struct ModInfo {
    pub id: String,
    pub name: String,
    pub version: String,
    pub publisher: String,
    pub website: String,
    pub support_url: String,
    pub description: String,
}

#[derive(Clone, Debug, Default)]
pub struct DetectionRule {
    pub kind: String,
    pub app_id: String,
    pub install_dir: String,
    pub app_name: String,
    pub key: String,
    pub value: String,
    pub sub_path: String,
    pub path: String,
}

#[derive(Clone, Debug, Default)]
pub struct ModGame {
    pub name: String,
    pub executable: String,
    pub steam_app_id: String,
    pub validation_files: Vec<String>,
    pub process_names: Vec<String>,
    pub detection: Vec<DetectionRule>,
    pub allow_unvalidated: bool,
}

#[derive(Clone, Debug)]
pub struct Component {
    pub id: String,
    pub name: String,
    pub description: String,
    pub source: String,
    pub target: String,
    pub target_name: String,
    pub source_game: Vec<String>,
    pub skip_if_present: Vec<String>,
    pub required: bool,
    pub selected: bool,
}

#[derive(Clone, Debug)]
pub struct LaunchSpec {
    pub executable: String,
    pub arguments: String,
    pub working_dir: String,
    pub uri: String,
    pub hint: String,
}

#[derive(Clone, Debug)]
pub struct Requirement {
    pub path: String,
    pub message: String,
}

#[derive(Clone, Debug)]
pub struct Layout {
    pub target_dir: String,
    pub state_dir: String,
    pub uninstall_key: String,
    pub allowed_roots: Vec<String>,
    pub preserve: Vec<String>,
    pub uninstall_cleanup: Vec<String>,
    pub uninstall_cleanup_with: String,
    pub requires: Vec<Requirement>,
    pub backup_overwritten: bool,
    pub locked_file_retries: i32,
    pub extra_space_mb: i32,
}

#[derive(Clone, Debug, Default)]
pub struct ExtraPackage {
    pub dir: String,
    pub github: String,
    pub tag: String,
    pub asset: String,
    pub url: String,
    pub sha256: String,
}

#[derive(Clone, Debug, Default)]
pub struct Package {
    pub mods_info: String,
    pub url: String,
    pub sha256: String,
    pub version: String,
    pub github: String,
    pub asset: String,
    pub extras: Vec<ExtraPackage>,
}

#[derive(Clone, Debug)]
pub struct ModSpec {
    pub info: ModInfo,
    pub game: ModGame,
    pub install: Layout,
    pub components: Vec<Component>,
    pub launch: LaunchSpec,
    pub package: Package,
}

#[derive(Clone, Debug)]
pub struct GameSpec {
    pub id: String,
    pub name: String,
    pub description: String,
    pub player_count: String,
    pub controls: String,
    pub image: String,
    pub image_focus: f32,
    pub max_players: i32,
    pub online_id: String,
    pub join_action: String,
    pub site_id: String,
    pub lobby: LobbySpec,
    pub guide: GuideSpec,
    pub categories: Vec<String>,
    pub coming_soon: bool,
    pub settings: Vec<Setting>,
    pub mod_spec: Option<ModSpec>,
    pub color: Color,
    pub color_end: Color,
    pub actions: Vec<ActionSpec>,
}

impl Default for GameSpec {
    fn default() -> Self {
        GameSpec {
            id: String::new(),
            name: String::new(),
            description: String::new(),
            player_count: String::new(),
            controls: String::new(),
            image: String::new(),
            image_focus: 0.5,
            max_players: 0,
            online_id: String::new(),
            join_action: String::new(),
            site_id: String::new(),
            lobby: LobbySpec { enabled: false, version: "1".into(), port: 7790 },
            guide: GuideSpec::default(),
            categories: Vec::new(),
            coming_soon: false,
            settings: Vec::new(),
            mod_spec: None,
            color: Color::rgb(0x4F8BFF),
            color_end: Color::rgb(0x9C6BFF),
            actions: Vec::new(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct Category {
    pub id: String,
    pub name: String,
    pub icon: String,
    pub group: String,
}

#[derive(Clone, Debug)]
pub struct Config {
    pub schema_version: i32,
    pub branding: Branding,
    pub paths: Paths,
    pub behavior: Behavior,
    pub downloads: Downloads,
    pub rooms: Rooms,
    /// Where "How did it run?" reports go and come from (reports-api/, src/launcher/reports.rs); empty = no reports.
    pub reports_url: String,
    pub discord_client_id: String,
    pub updates: InstallSpec,
    pub players: Vec<PlayerSpec>,
    pub categories: Vec<Category>,
    pub games: Vec<GameSpec>,
    pub text: BTreeMap<String, String>,
    pub config_dir: String,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            schema_version: 1,
            branding: Branding::default(),
            paths: Paths::default(),
            behavior: Behavior { minimize_on_launch: false, stop_relay_with_game: true, quick_exit_seconds: 4 },
            downloads: Downloads {
                catalog: String::new(),
                mods_info: String::new(),
                site: "https://www.multiplayereverything.com/games.json".into(),
            },
            rooms: Rooms { servers: String::new(), server: String::new(), refresh_seconds: 30 },
            reports_url: String::new(),
            discord_client_id: String::new(),
            updates: InstallSpec::default(),
            players: Vec::new(),
            categories: Vec::new(),
            games: Vec::new(),
            text: BTreeMap::new(),
            config_dir: String::new(),
        }
    }
}

impl Config {
    /// A UI text with `${NAME}` filled in from `vars`; a key with no text is shown as the key.
    pub fn t(&self, key: &str, vars: &[(&str, &str)]) -> String {
        let mut s = self.text.get(key).cloned().unwrap_or_else(|| key.to_string());
        for (name, value) in vars {
            s = s.replace(&format!("${{{name}}}"), value);
        }
        s
    }
    pub fn updates_enabled(&self) -> bool {
        !self.updates.github.is_empty() && !self.updates.asset.is_empty()
    }
    pub fn find_game(&self, id: &str) -> Option<&GameSpec> {
        self.games.iter().find(|g| g.id == id)
    }
    pub fn find_player(&self, id: &str) -> Option<&PlayerSpec> {
        self.players.iter().find(|p| p.id == id)
    }
    pub fn find_category(&self, id: &str) -> Option<&Category> {
        self.categories.iter().find(|c| c.id == id)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Severity {
    Warning,
    Error,
}

#[derive(Clone, Debug)]
pub struct ConfigIssue {
    pub severity: Severity,
    pub place: String,
    pub message: String,
}

pub struct LoadResult {
    pub config: Config,
    pub issues: Vec<ConfigIssue>,
}

impl LoadResult {
    pub fn ok(&self) -> bool {
        self.issues.iter().all(|i| i.severity != Severity::Error)
    }
}

type Issues = RefCell<Vec<ConfigIssue>>;

fn type_name(t: Type) -> &'static str {
    match t {
        Type::Null => "null",
        Type::Bool => "true/false",
        Type::Number => "a number",
        Type::String => "a string",
        Type::Array => "a list",
        Type::Object => "an object",
    }
}

/// Reads one JSON object into fields, reporting type errors and, when it goes out of scope, the keys nobody asked
/// for (usually typos) with their path, as the C++ `Section` does.
struct Section<'a> {
    v: Option<&'a Value>,
    path: String,
    issues: &'a Issues,
    seen: RefCell<HashSet<String>>,
}

impl<'a> Section<'a> {
    fn new(v: Option<&'a Value>, path: impl Into<String>, issues: &'a Issues) -> Self {
        let s = Section { v: v.filter(|v| v.is_object()), path: path.into(), issues, seen: RefCell::new(HashSet::new()) };
        if let Some(v) = v
            && !v.is_object()
            && !v.is_null()
        {
            s.error("", "must be an object");
        }
        s
    }
    fn get(&self, key: &str, t: Type) -> Option<&'a Value> {
        let v = self.v?;
        self.seen.borrow_mut().insert(key.to_string());
        let f = v.find(key).filter(|f| !f.is_null())?;
        if f.kind() != t {
            self.error(key, &format!("must be {}, found {}", type_name(t), type_name(f.kind())));
            return None;
        }
        Some(f)
    }
    fn str(&self, key: &str, out: &mut String) {
        if let Some(f) = self.get(key, Type::String) {
            *out = f.as_str().unwrap_or_default().to_string();
        }
    }
    fn boolean(&self, key: &str, out: &mut bool) {
        if let Some(f) = self.get(key, Type::Bool) {
            *out = f.as_bool().unwrap_or(false);
        }
    }
    fn integer(&self, key: &str, out: &mut i32, lo: i32, hi: i32) {
        if let Some(f) = self.get(key, Type::Number) {
            let d = f.as_f64().unwrap_or(0.0);
            if d < lo as f64 || d > hi as f64 {
                self.error(key, &format!("must be between {lo} and {hi}"));
            } else {
                *out = d as i32;
            }
        }
    }
    fn real(&self, key: &str, out: &mut f32, lo: f32, hi: f32) {
        if let Some(f) = self.get(key, Type::Number) {
            let d = f.as_f64().unwrap_or(0.0);
            if d < lo as f64 || d > hi as f64 {
                self.error(key, "is out of range");
            } else {
                *out = d as f32;
            }
        }
    }
    fn color(&self, key: &str, out: &mut Color) {
        let mut text = String::new();
        self.str(key, &mut text);
        if !text.is_empty() {
            match parse_color(&text) {
                Some(c) => *out = c,
                None => self.error(key, "must be a color like \"#4F8BFF\""),
            }
        }
    }
    /// A single string counts as a one-item list ("swf": "x.swf").
    fn str_list(&self, key: &str, out: &mut Vec<String>, present: Option<&mut bool>) {
        let Some(v) = self.v else { return };
        self.seen.borrow_mut().insert(key.to_string());
        let Some(f) = v.find(key).filter(|f| !f.is_null()) else { return };
        if let Some(p) = present {
            *p = true;
        }
        out.clear();
        if let Some(s) = f.as_str() {
            out.push(s.to_string());
            return;
        }
        if !f.is_array() {
            self.error(key, "must be a string or a list of strings");
            return;
        }
        for item in f.items() {
            match item.as_str() {
                Some(s) => out.push(s.to_string()),
                None => self.error(key, "must be a list of strings"),
            }
        }
    }
    fn id_list(&self, key: &str, out: &mut Vec<String>) {
        let mut w = Vec::new();
        self.str_list(key, &mut w, None);
        out.extend(w);
    }
    fn child(&self, key: &str) -> String {
        if self.path.is_empty() { key.to_string() } else { format!("{}.{key}", self.path) }
    }
    fn error(&self, key: &str, msg: &str) {
        let place = if key.is_empty() { self.path.clone() } else { self.child(key) };
        self.issues.borrow_mut().push(ConfigIssue { severity: Severity::Error, place, message: msg.to_string() });
    }
}

impl Drop for Section<'_> {
    fn drop(&mut self) {
        let Some(v) = self.v else { return };
        let seen = self.seen.borrow();
        for (k, _) in v.entries() {
            if k.is_empty() || k.starts_with('_') || k.starts_with('$') {
                continue; // "_comment" keys are allowed
            }
            if !seen.contains(k) {
                self.issues.borrow_mut().push(ConfigIssue {
                    severity: Severity::Warning,
                    place: self.child(k),
                    message: "unknown setting (typo?)".into(),
                });
            }
        }
    }
}

/// wcstoul(text, 16) over the whole string, as readIcon reads it: spaces, a sign and "0x" are allowed in front.
fn parse_hex_ulong(text: &str) -> Option<u64> {
    let t = text.trim_start_matches(|c: char| c == ' ' || ('\t'..='\r').contains(&c));
    let (neg, t) = match t.as_bytes().first() {
        Some(b'-') => (true, &t[1..]),
        Some(b'+') => (false, &t[1..]),
        _ => (false, t),
    };
    let t = match t.get(..2) {
        Some("0x" | "0X") if t[2..].starts_with(|c: char| c.is_ascii_hexdigit()) => &t[2..],
        _ => t,
    };
    if t.is_empty() || !t.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let v = t.chars().fold(0u64, |v, c| v.saturating_mul(16).saturating_add(c.to_digit(16).unwrap() as u64).min(u32::MAX as u64 + 1));
    let v = if v > u32::MAX as u64 { u32::MAX as u64 } else { v };
    Some(if neg { (v as u32).wrapping_neg() as u64 } else { v })
}

/// "icon": a Segoe Fluent Icons code point written as hex ("E774" is the globe).
fn read_icon(s: &Section, out: &mut String) {
    let mut icon = String::new();
    s.str("icon", &mut icon);
    if icon.is_empty() {
        return;
    }
    match parse_hex_ulong(&icon).filter(|cp| (0xE000..=0xF8FF).contains(cp)) {
        Some(cp) => *out = char::from_u32(cp as u32).map(String::from).unwrap_or_default(),
        None => s.error("icon", "must be a Segoe Fluent Icons code like \"E774\""),
    }
}

fn index_path(list: &str, i: usize) -> String {
    format!("{list}[{i}]")
}

/// A "settings" list (launcher.json or the catalog); bad entries are reported and left out.
pub fn read_settings(list: &Value, place: &str, issues: &RefCell<Vec<ConfigIssue>>) -> Vec<Setting> {
    let mut out = Vec::new();
    for (k, item) in list.items().iter().enumerate() {
        let s = Section::new(Some(item), format!("{place}[{k}]"), issues);
        let mut st = Setting {
            id: String::new(),
            kind: "checkbox".into(),
            scope: "host".into(),
            label: String::new(),
            since: String::new(),
            args: Vec::new(),
            choices: Vec::new(),
            def: String::new(),
        };
        s.str("id", &mut st.id);
        s.str("type", &mut st.kind);
        s.str("scope", &mut st.scope);
        s.str("label", &mut st.label);
        s.str("since", &mut st.since);
        s.str("default", &mut st.def);
        s.str_list("args", &mut st.args, None);
        if let Some(choices) = s.get("choices", Type::Array) {
            for (c, item) in choices.items().iter().enumerate() {
                let cs = Section::new(Some(item), format!("{}[{c}]", s.child("choices")), issues);
                let mut ch = SettingChoice { id: String::new(), label: String::new(), args: Vec::new() };
                cs.str("id", &mut ch.id);
                cs.str("label", &mut ch.label);
                cs.str_list("args", &mut ch.args, None);
                if ch.id.is_empty() || ch.label.is_empty() {
                    cs.error("id", "give \"id\" and \"label\"");
                } else {
                    st.choices.push(ch);
                }
            }
        }
        if st.kind != "checkbox" && st.kind != "choice" {
            s.error("type", "must be \"checkbox\" or \"choice\"");
        } else if st.scope != "host" && st.scope != "join" && st.scope != "all" {
            s.error("scope", "must be \"host\", \"join\" or \"all\"");
        } else if st.id.is_empty() || st.label.is_empty() {
            s.error("id", "give \"id\" and \"label\"");
        } else if st.kind == "checkbox" && st.args.is_empty() {
            s.error("args", "a checkbox needs \"args\"");
        } else if st.kind == "choice" && st.choices.len() < 2 {
            s.error("choices", "a choice needs at least two \"choices\"");
        } else if out.len() >= MAX_SETTINGS {
            s.error("id", "at most four settings");
        } else {
            out.push(st);
        }
    }
    out
}

fn read_action(v: &Value, place: &str, issues: &Issues) -> ActionSpec {
    let mut a = ActionSpec::default();
    let s = Section::new(Some(v), place, issues);
    s.str("id", &mut a.id);
    s.str("label", &mut a.label);
    s.str("description", &mut a.description);
    s.str("hint", &mut a.hint);
    s.str_list("swf", &mut a.swf, None);
    s.str_list("file", &mut a.swf, None); // the same list, named for HTML5 games
    s.str_list("page", &mut a.page, None);
    s.boolean("joinLink", &mut a.join_link);
    if let Some(sv) = s.get("server", Type::Object) {
        let ss = Section::new(Some(sv), s.child("server"), issues);
        ss.str("name", &mut a.server.name);
        ss.str("website", &mut a.server.website);
        ss.str_list("exe", &mut a.server.exe, None);
        ss.str_list("args", &mut a.server.args, None);
        ss.str("cwd", &mut a.server.cwd);
        ss.str("url", &mut a.server.url);
        ss.str("share", &mut a.server.share);
        ss.str("lan", &mut a.server.lan);
    }
    s.str_list("args", &mut a.args, None);
    s.str_list("joinArgs", &mut a.join_args, None);
    s.str("link", &mut a.link);
    s.id_list("players", &mut a.players);
    if let Some(o) = s.get("overrides", Type::Object) {
        for (key, item) in o.entries() {
            let mut po = PlayerOverride::default();
            let os = Section::new(Some(item), format!("{}.{key}", s.child("overrides")), issues);
            os.str_list("swf", &mut po.swf, Some(&mut po.has_swf));
            os.str_list("args", &mut po.args, Some(&mut po.has_args));
            drop(os);
            a.overrides.insert(key.clone(), po);
        }
    }
    if let Some(fv) = s.get("flashvars", Type::Object) {
        for (key, item) in fv.entries() {
            match item.as_str() {
                Some(val) => a.flashvars.push((key.clone(), val.to_string())),
                None => s.error("flashvars", "values must be strings"),
            }
        }
    }
    s.boolean("more", &mut a.more);
    s.boolean("offline", &mut a.offline);
    read_icon(&s, &mut a.icon);
    let mut edition = String::new();
    s.str("edition", &mut edition);
    if !edition.is_empty() && edition != "original" && edition != "modded" {
        s.error("edition", "must be \"original\" or \"modded\"");
    }
    a.original = edition == "original";
    if let Some(r) = s.get("relay", Type::Object) {
        let rs = Section::new(Some(r), s.child("relay"), issues);
        a.relay.enabled = true;
        rs.boolean("enabled", &mut a.relay.enabled);
        rs.integer("port", &mut a.relay.port, 1, 65535);
    }
    if a.label.is_empty() {
        a.label = a.id.clone();
    }
    a
}

fn read_mod(v: &Value, game: &GameSpec, place: &str, issues: &Issues) -> ModSpec {
    let mut m = ModSpec {
        info: ModInfo { id: game.id.clone(), name: game.name.clone(), description: game.description.clone(), ..Default::default() },
        game: ModGame::default(),
        install: Layout {
            target_dir: "${GAME_DIR}\\Mods\\${MOD_ID}".into(),
            state_dir: "${INSTALL_DIR}\\.installer".into(),
            uninstall_key: "${MOD_ID}".into(),
            allowed_roots: Vec::new(),
            preserve: Vec::new(),
            uninstall_cleanup: Vec::new(),
            uninstall_cleanup_with: String::new(),
            requires: Vec::new(),
            backup_overwritten: true,
            locked_file_retries: 5,
            extra_space_mb: 20,
        },
        components: Vec::new(),
        launch: LaunchSpec {
            executable: "${GAME_DIR}\\${GAME_EXE}".into(),
            arguments: String::new(),
            working_dir: "${GAME_DIR}".into(),
            uri: String::new(),
            hint: String::new(),
        },
        package: Package::default(),
    };
    let s = Section::new(Some(v), place, issues);
    s.str("id", &mut m.info.id); // keep a stand-alone installer's id to adopt its installs
    s.str("publisher", &mut m.info.publisher);
    s.str("website", &mut m.info.website);
    s.str("supportUrl", &mut m.info.support_url);
    if let Some(g) = s.get("game", Type::Object) {
        let gs = Section::new(Some(g), s.child("game"), issues);
        gs.str("name", &mut m.game.name);
        gs.str("executable", &mut m.game.executable);
        gs.str("steamAppId", &mut m.game.steam_app_id);
        gs.str_list("validationFiles", &mut m.game.validation_files, None);
        gs.str_list("processNames", &mut m.game.process_names, None);
        gs.boolean("allowUnvalidated", &mut m.game.allow_unvalidated);
        if let Some(list) = gs.get("detection", Type::Array) {
            for (i, item) in list.items().iter().enumerate() {
                let mut r = DetectionRule::default();
                let rs = Section::new(Some(item), format!("{}[{i}]", gs.child("detection")), issues);
                rs.str("type", &mut r.kind);
                rs.str("appId", &mut r.app_id);
                rs.str("installDir", &mut r.install_dir);
                rs.str("appName", &mut r.app_name);
                rs.str("key", &mut r.key);
                rs.str("value", &mut r.value);
                rs.str("subPath", &mut r.sub_path);
                rs.str("path", &mut r.path);
                if !["steam", "epic", "registry", "path"].contains(&r.kind.as_str()) {
                    rs.error("type", "must be steam, epic, registry or path");
                }
                drop(rs);
                m.game.detection.push(r);
            }
        }
    }
    if m.game.name.is_empty() {
        m.game.name = game.name.clone();
    }
    if let Some(inv) = s.get("install", Type::Object) {
        let is = Section::new(Some(inv), s.child("install"), issues);
        is.str("targetDir", &mut m.install.target_dir);
        is.str("stateDir", &mut m.install.state_dir);
        is.str("uninstallKey", &mut m.install.uninstall_key);
        is.str_list("allowedRoots", &mut m.install.allowed_roots, None);
        is.str_list("preserve", &mut m.install.preserve, None);
        is.str_list("uninstallCleanup", &mut m.install.uninstall_cleanup, None);
        is.str("uninstallCleanupWith", &mut m.install.uninstall_cleanup_with);
        if let Some(rq) = is.get("requires", Type::Array) {
            for (i, item) in rq.items().iter().enumerate() {
                let mut r = Requirement { path: String::new(), message: String::new() };
                let rs = Section::new(Some(item), format!("{}[{i}]", is.child("requires")), issues);
                rs.str("path", &mut r.path);
                rs.str("message", &mut r.message);
                drop(rs);
                m.install.requires.push(r);
            }
        }
        is.boolean("backupOverwritten", &mut m.install.backup_overwritten);
        is.integer("lockedFileRetries", &mut m.install.locked_file_retries, 0, 30);
        is.integer("extraSpaceMB", &mut m.install.extra_space_mb, 0, 1 << 20);
    }
    if let Some(list) = s.get("components", Type::Array) {
        for (i, item) in list.items().iter().enumerate() {
            let mut c = Component {
                id: String::new(),
                name: String::new(),
                description: String::new(),
                source: String::new(),
                target: "${INSTALL_DIR}".into(),
                target_name: String::new(),
                source_game: Vec::new(),
                skip_if_present: Vec::new(),
                required: false,
                selected: true,
            };
            let cs = Section::new(Some(item), format!("{}[{i}]", s.child("components")), issues);
            cs.str("id", &mut c.id);
            cs.str("name", &mut c.name);
            cs.str("description", &mut c.description);
            cs.str("source", &mut c.source);
            cs.str("target", &mut c.target);
            cs.str("targetName", &mut c.target_name);
            cs.str_list("sourceGame", &mut c.source_game, None);
            cs.str_list("skipIfPresent", &mut c.skip_if_present, None);
            cs.boolean("required", &mut c.required);
            cs.boolean("selected", &mut c.selected);
            drop(cs);
            if c.name.is_empty() {
                c.name = c.id.clone();
            }
            m.components.push(c);
        }
    }
    if let Some(l) = s.get("launch", Type::Object) {
        let ls = Section::new(Some(l), s.child("launch"), issues);
        ls.str("executable", &mut m.launch.executable);
        ls.str("arguments", &mut m.launch.arguments);
        ls.str("workingDir", &mut m.launch.working_dir);
        ls.str("uri", &mut m.launch.uri);
        ls.str("hint", &mut m.launch.hint);
    }
    if let Some(p) = s.get("package", Type::Object) {
        let ps = Section::new(Some(p), s.child("package"), issues);
        ps.str("modsInfo", &mut m.package.mods_info);
        ps.str("url", &mut m.package.url);
        ps.str("sha256", &mut m.package.sha256);
        ps.str("version", &mut m.package.version);
        ps.str("github", &mut m.package.github);
        ps.str("asset", &mut m.package.asset);
        if let Some(ex) = ps.get("extras", Type::Array) {
            for (i, item) in ex.items().iter().enumerate() {
                let mut e = ExtraPackage::default();
                let es = Section::new(Some(item), format!("{}[{i}]", ps.child("extras")), issues);
                es.str("dir", &mut e.dir);
                es.str("github", &mut e.github);
                es.str("tag", &mut e.tag);
                es.str("asset", &mut e.asset);
                es.str("url", &mut e.url);
                es.str("sha256", &mut e.sha256);
                drop(es);
                m.package.extras.push(e);
            }
        }
    }
    m
}

/// The buttons of a mod game; the window shows the ones that fit its state. The top one is the first shown.
fn add_mod_actions(g: &mut GameSpec) {
    for op in ["update", "install", "launch", "repair", "uninstall"] {
        g.actions.push(ActionSpec { id: op.into(), mod_op: op.into(), label: op.into(), ..Default::default() });
    }
}

fn is_https_or_local(u: &str) -> bool {
    starts_with_i(u, "https://") || starts_with_i(u, "http://127.0.0.1")
}

pub fn load_config(text: &[u8], config_dir: &str) -> LoadResult {
    let issues: Issues = RefCell::new(Vec::new());
    let mut c = Config { config_dir: config_dir.to_string(), ..Default::default() };
    for (k, v) in DEFAULT_TEXTS {
        c.text.insert(k.to_string(), v.to_string());
    }
    let root = match json::parse(text) {
        Ok(v) => v,
        Err(e) => {
            issues.borrow_mut().push(ConfigIssue { severity: Severity::Error, place: "launcher.json".into(), message: e });
            return LoadResult { config: c, issues: issues.into_inner() };
        }
    };
    if !root.is_object() {
        issues.borrow_mut().push(ConfigIssue {
            severity: Severity::Error,
            place: "launcher.json".into(),
            message: "the top level must be an object".into(),
        });
        return LoadResult { config: c, issues: issues.into_inner() };
    }
    {
        let top = Section::new(Some(&root), "", &issues);
        top.integer("schemaVersion", &mut c.schema_version, 1, 1);
        {
            let s = Section::new(top.get("branding", Type::Object), "branding", &issues);
            let b = &mut c.branding;
            s.str("title", &mut b.title);
            s.str("subtitle", &mut b.subtitle);
            s.str("website", &mut b.website);
            s.str("windowTitle", &mut b.window_title);
            s.str("theme", &mut b.theme);
            if !iequals(&b.theme, "dark") && !iequals(&b.theme, "light") {
                s.error("theme", "must be \"dark\" or \"light\"");
            }
            b.colors = if iequals(&b.theme, "light") { light_palette() } else { dark_palette() };
            if let Some(colors) = s.get("colors", Type::Object) {
                let cs = Section::new(Some(colors), "branding.colors", &issues);
                for (k, name) in COLOR_FIELDS.iter().enumerate() {
                    cs.color(name, &mut b.colors[k]);
                }
            }
            s.str("font", &mut b.font);
            s.str("displayFont", &mut b.display_font);
            if let Some(pc) = s.get("playerColors", Type::Array) {
                b.player_colors.clear();
                for item in pc.items() {
                    match item.as_str().and_then(parse_color) {
                        Some(col) => b.player_colors.push(col),
                        None => s.error("playerColors", "must be a list of colors like \"#E5484D\""),
                    }
                }
                if b.player_colors.is_empty() {
                    s.error("playerColors", "needs at least one color");
                }
            }
            s.real("fontSize", &mut b.font_size, 7.0, 20.0);
            let ws = Section::new(s.get("window", Type::Object), "branding.window", &issues);
            ws.integer("width", &mut b.window.width, 400, 8000);
            ws.integer("height", &mut b.window.height, 300, 8000);
            ws.integer("minWidth", &mut b.window.min_width, 400, 8000);
            ws.integer("minHeight", &mut b.window.min_height, 300, 8000);
        }
        {
            let s = Section::new(top.get("paths", Type::Object), "paths", &issues);
            s.str("games", &mut c.paths.games);
            s.str("players", &mut c.paths.players);
            s.str("data", &mut c.paths.data);
            s.str_list("flashpoint", &mut c.paths.flashpoint, None);
        }
        {
            let s = Section::new(top.get("behavior", Type::Object), "behavior", &issues);
            s.boolean("minimizeOnLaunch", &mut c.behavior.minimize_on_launch);
            s.boolean("stopRelayWithGame", &mut c.behavior.stop_relay_with_game);
            s.integer("quickExitSeconds", &mut c.behavior.quick_exit_seconds, 0, 60);
        }
        {
            let rs = Section::new(top.get("rooms", Type::Object), "rooms", &issues);
            rs.str("servers", &mut c.rooms.servers);
            rs.str("server", &mut c.rooms.server);
            rs.integer("refreshSeconds", &mut c.rooms.refresh_seconds, 10, 3600);
            if !c.rooms.servers.is_empty() && !is_https_or_local(&c.rooms.servers) {
                rs.error("servers", "must be an https:// address");
            }
        }
        {
            let s = Section::new(top.get("reports", Type::Object), "reports", &issues);
            s.str("url", &mut c.reports_url);
            if !c.reports_url.is_empty() && !is_https_or_local(&c.reports_url) && !starts_with_i(&c.reports_url, "http://localhost") {
                s.error("url", "must be an https:// address");
            }
        }
        {
            let s = Section::new(top.get("updates", Type::Object), "updates", &issues);
            s.str("github", &mut c.updates.github);
            s.str("asset", &mut c.updates.asset);
            s.str("api", &mut c.updates.api);
            if !c.updates.github.is_empty() && !c.updates.github.contains('/') {
                s.error("github", "must look like \"owner/repo\"");
            }
            if !c.updates.github.is_empty() && c.updates.asset.is_empty() {
                s.error("asset", "give the release file name, e.g. \"MultiplayerEverythingLauncher.exe\"");
            }
        }
        {
            let s = Section::new(top.get("discord", Type::Object), "discord", &issues);
            s.str("clientId", &mut c.discord_client_id);
            if c.discord_client_id.chars().any(|ch| !ch.is_ascii_digit()) {
                s.error("clientId", "must be the application id, digits only");
            }
        }
        {
            let s = Section::new(top.get("downloads", Type::Object), "downloads", &issues);
            s.str("catalog", &mut c.downloads.catalog);
            s.str("modsInfo", &mut c.downloads.mods_info);
            s.str("site", &mut c.downloads.site);
            if !c.downloads.site.is_empty() && !is_https_or_local(&c.downloads.site) {
                s.error("site", "must be an https:// address");
            }
            if !c.downloads.catalog.is_empty() && !is_https_or_local(&c.downloads.catalog) {
                s.error("catalog", "must be an https:// address");
            }
            if !c.downloads.mods_info.is_empty() && !is_https_or_local(&c.downloads.mods_info) {
                s.error("modsInfo", "must be an https:// address");
            }
        }
        if let Some(list) = top.get("players", Type::Array) {
            for (i, item) in list.items().iter().enumerate() {
                let mut p = PlayerSpec {
                    id: String::new(),
                    kind: "flash".into(),
                    name: String::new(),
                    website: String::new(),
                    install: InstallSpec::default(),
                    paths: Vec::new(),
                    args: Vec::new(),
                    flashvar_arg: String::new(),
                    coop_save_arg: String::new(),
                    coop_save_since: String::new(),
                    tv_args: Vec::new(),
                    tv_env: Vec::new(),
                };
                let s = Section::new(Some(item), index_path("players", i), &issues);
                s.str("id", &mut p.id);
                s.str("kind", &mut p.kind);
                if p.kind != "flash" && p.kind != "browser" {
                    s.error("kind", "must be \"flash\" or \"browser\"");
                }
                s.str("name", &mut p.name);
                s.str("website", &mut p.website);
                s.str_list("paths", &mut p.paths, None);
                s.str_list("args", &mut p.args, None);
                s.str("flashvarArg", &mut p.flashvar_arg);
                s.str("coopSaveArg", &mut p.coop_save_arg);
                s.str("coopSaveSince", &mut p.coop_save_since);
                if let Some(tv) = s.get("tv", Type::Object) {
                    let ts = Section::new(Some(tv), s.child("tv"), &issues);
                    ts.str_list("args", &mut p.tv_args, None);
                    if let Some(env) = ts.get("env", Type::Object) {
                        for (k, v) in env.entries() {
                            match v.as_str() {
                                Some(val) => p.tv_env.push((k.clone(), val.to_string())),
                                None => ts.error("env", "values must be strings"),
                            }
                        }
                    }
                }
                if let Some(inst) = s.get("install", Type::Object) {
                    let is = Section::new(Some(inst), s.child("install"), &issues);
                    is.str("github", &mut p.install.github);
                    is.str("asset", &mut p.install.asset);
                    is.str("url", &mut p.install.url);
                    is.str("sha256", &mut p.install.sha256);
                    is.str("dir", &mut p.install.dir);
                    is.str("exe", &mut p.install.exe);
                    is.str("api", &mut p.install.api);
                }
                if p.name.is_empty() {
                    p.name = p.id.clone();
                }
                drop(s);
                c.players.push(p);
            }
        }
        if let Some(list) = top.get("categories", Type::Array) {
            for (i, item) in list.items().iter().enumerate() {
                let mut cat = Category { id: String::new(), name: String::new(), icon: String::new(), group: String::new() };
                let s = Section::new(Some(item), index_path("categories", i), &issues);
                s.str("id", &mut cat.id);
                s.str("name", &mut cat.name);
                s.str("group", &mut cat.group);
                read_icon(&s, &mut cat.icon);
                if cat.name.is_empty() {
                    cat.name = cat.id.clone();
                }
                drop(s);
                c.categories.push(cat);
            }
        }
        if let Some(list) = top.get("games", Type::Array) {
            for (i, item) in list.items().iter().enumerate() {
                let mut g = GameSpec::default();
                let place = index_path("games", i);
                let s = Section::new(Some(item), place.clone(), &issues);
                s.str("id", &mut g.id);
                s.str("name", &mut g.name);
                s.str("description", &mut g.description);
                s.str("players", &mut g.player_count);
                s.integer("maxPlayers", &mut g.max_players, 0, 16);
                if let Some(on) = s.get("online", Type::Object) {
                    let os = Section::new(Some(on), s.child("online"), &issues);
                    os.str("id", &mut g.online_id);
                    os.str("join", &mut g.join_action);
                    if let Some(lv) = os.get("lobby", Type::Object) {
                        let ls = Section::new(Some(lv), os.child("lobby"), &issues);
                        g.lobby.enabled = true;
                        ls.boolean("enabled", &mut g.lobby.enabled);
                        ls.str("version", &mut g.lobby.version);
                        ls.integer("port", &mut g.lobby.port, 1, 65535);
                    }
                }
                if g.max_players == 0 {
                    // "1-4 players" -> 4: the highest number in the text.
                    let units: Vec<char> = g.player_count.chars().collect();
                    for k in 0..units.len() {
                        if units[k].is_ascii_digit() {
                            let rest: String = units[k..].iter().collect();
                            g.max_players = g.max_players.max(super::text::atoi(&rest));
                        }
                    }
                }
                s.str("controls", &mut g.controls);
                s.str("image", &mut g.image);
                s.str("site", &mut g.site_id);
                if let Some(gv) = s.get("guide", Type::Object) {
                    let gs = Section::new(Some(gv), s.child("guide"), &issues);
                    gs.str_list("needs", &mut g.guide.needs, None);
                    gs.str_list("steps", &mut g.guide.steps, None);
                    gs.str_list("tips", &mut g.guide.tips, None);
                    if let Some(keys) = gs.get("keys", Type::Array) {
                        for (k, item) in keys.items().iter().enumerate() {
                            let ks = Section::new(Some(item), format!("{}[{k}]", gs.child("keys")), &issues);
                            let (mut key, mut does) = (String::new(), String::new());
                            ks.str("key", &mut key);
                            ks.str("does", &mut does);
                            if key.is_empty() || does.is_empty() {
                                ks.error("key", "give both \"key\" and \"does\"");
                            } else {
                                g.guide.keys.push((key, does));
                            }
                        }
                    }
                    if let Some(links) = gs.get("links", Type::Array) {
                        for (k, item) in links.items().iter().enumerate() {
                            let ls = Section::new(Some(item), format!("{}[{k}]", gs.child("links")), &issues);
                            let mut link = GuideLink { label: String::new(), url: String::new() };
                            ls.str("label", &mut link.label);
                            ls.str("url", &mut link.url);
                            if link.label.is_empty() || !starts_with_i(&link.url, "https://") {
                                ls.error("url", "give a \"label\" and an https:// \"url\"");
                            } else if g.guide.links.len() >= GUIDE_MAX_LINKS {
                                ls.error("url", "at most four links");
                            } else {
                                g.guide.links.push(link);
                            }
                        }
                    }
                }
                s.real("imageFocus", &mut g.image_focus, 0.0, 1.0);
                s.boolean("comingSoon", &mut g.coming_soon);
                if let Some(st) = s.get("settings", Type::Array) {
                    g.settings = read_settings(st, &s.child("settings"), &issues);
                }
                s.id_list("categories", &mut g.categories);
                s.color("color", &mut g.color);
                s.color("colorEnd", &mut g.color_end);
                if let Some(actions) = s.get("actions", Type::Array) {
                    for (j, item) in actions.items().iter().enumerate() {
                        g.actions.push(read_action(item, &format!("{place}.{}", index_path("actions", j)), &issues));
                    }
                }
                if let Some(mv) = s.get("mod", Type::Object) {
                    // The launcher makes the install / launch buttons; the only buttons a mod game may add are
                    // links (the mod's own page), which come after them.
                    let mut links = Vec::new();
                    for (j, a) in std::mem::take(&mut g.actions).into_iter().enumerate() {
                        if a.is_link() {
                            links.push(a);
                        } else {
                            issues.borrow_mut().push(ConfigIssue {
                                severity: Severity::Error,
                                place: format!("{place}.{}", index_path("actions", j)),
                                message: "a mod game gets its buttons from the launcher; only a \"link\" action can be added".into(),
                            });
                        }
                    }
                    g.mod_spec = Some(read_mod(mv, &g, &s.child("mod"), &issues));
                    add_mod_actions(&mut g);
                    g.actions.extend(links);
                }
                if g.name.is_empty() {
                    g.name = g.id.clone();
                }
                drop(s);
                c.games.push(g);
            }
        }
        if let Some(texts) = top.get("text", Type::Object) {
            for (k, v) in texts.entries() {
                if k.starts_with('_') {
                    continue;
                }
                let Some(val) = v.as_str() else {
                    issues.borrow_mut().push(ConfigIssue {
                        severity: Severity::Error,
                        place: format!("text.{k}"),
                        message: "must be a string".into(),
                    });
                    continue;
                };
                if !c.text.contains_key(k) {
                    issues.borrow_mut().push(ConfigIssue {
                        severity: Severity::Warning,
                        place: format!("text.{k}"),
                        message: "unknown text key (typo?)".into(),
                    });
                }
                c.text.insert(k.clone(), val.to_string());
            }
        }
    }
    let mut r = LoadResult { config: c, issues: issues.into_inner() };
    if r.ok() {
        let mut extra = Vec::new();
        validate_config(&r.config, &mut extra);
        r.issues.extend(extra);
    }
    r
}

pub fn load_config_file(path: &str) -> LoadResult {
    match paths::read_file(path) {
        Some(text) => load_config(&text, &paths::parent(path)),
        None => LoadResult {
            config: Config::default(),
            issues: vec![ConfigIssue { severity: Severity::Error, place: path.to_string(), message: "cannot read the file".into() }],
        },
    }
}

fn check_vars(text: &str, place: &str, issues: &mut Vec<ConfigIssue>) {
    let mut i = 0;
    while let Some(start) = text[i..].find("${").map(|p| p + i) {
        let Some(end) = text[start..].find('}').map(|p| p + start) else { break };
        let name = &text[start + 2..end];
        let ok = starts_with_i(name, "env:") || starts_with_i(name, "folder:") || KNOWN_NAMES.iter().any(|k| iequals(k, name));
        if !ok {
            issues.push(ConfigIssue { severity: Severity::Warning, place: place.to_string(), message: format!("unknown variable ${{{name}}}") });
        }
        i = end + 1;
    }
}

fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_' || c == b'.')
}

/// A path inside a folder, '/'-separated with "." parts dropped; empty when it could leave the folder.
pub fn norm_rel(rel: &str) -> String {
    let s = rel.replace('\\', "/");
    if s.contains(':') || s.starts_with("//") {
        return String::new();
    }
    let mut out = Vec::new();
    for part in s.split('/') {
        if part.is_empty() || part == "." {
            continue;
        }
        if part == ".." {
            return String::new();
        }
        out.push(part);
    }
    out.join("/")
}

/// launcher.json's server-link patterns are C++ (ECMAScript) regexes; Rust's regex lacks look-around and
/// back-references, so a pattern using them is taken as written rather than refused.
fn pattern_ok(re: &str) -> bool {
    if regex::Regex::new(re).is_ok() {
        return true;
    }
    ["(?=", "(?!", "(?<=", "(?<!"].iter().any(|p| re.contains(p)) || re.as_bytes().windows(2).any(|w| w[0] == b'\\' && w[1].is_ascii_digit())
}

/// Cross-references: unique ids, known players and categories, variables.
pub fn validate_config(cfg: &Config, issues: &mut Vec<ConfigIssue>) {
    let err = |issues: &mut Vec<ConfigIssue>, place: String, msg: String| {
        issues.push(ConfigIssue { severity: Severity::Error, place, message: msg });
    };
    if cfg.players.is_empty() {
        err(issues, "players".into(), "at least one player (Ruffle, Flash Player...) is required".into());
    }
    if cfg.games.is_empty() {
        err(issues, "games".into(), "the launcher needs at least one game".into());
    }
    let mut ids: BTreeSet<String> = BTreeSet::new();
    for (i, p) in cfg.players.iter().enumerate() {
        let w = index_path("players", i);
        if !valid_id(&p.id) {
            err(issues, format!("{w}.id"), "must be letters, digits, '-', '_' or '.'".into());
        } else if !ids.insert(p.id.clone()) {
            err(issues, format!("{w}.id"), format!("duplicate player id \"{}\"", p.id));
        }
        if p.paths.is_empty() {
            err(issues, format!("{w}.paths"), "list at least one place to look for the player".into());
        }
        for s in &p.paths {
            check_vars(s, &format!("{w}.paths"), issues);
        }
        for s in &p.args {
            check_vars(s, &format!("{w}.args"), issues);
        }
        let inst = &p.install;
        if inst.enabled() || !inst.dir.is_empty() {
            if inst.dir.is_empty() {
                err(issues, format!("{w}.install.dir"), "say which folder the player is unpacked into".into());
            }
            if !inst.github.is_empty() && inst.asset.is_empty() {
                err(issues, format!("{w}.install.asset"), "give the release file name, e.g. \"*windows-x86_64.zip\"".into());
            }
            if !inst.github.is_empty() && !inst.github.contains('/') {
                err(issues, format!("{w}.install.github"), "must look like \"owner/repo\"".into());
            }
            if !inst.url.is_empty() && !is_https_or_local(&inst.url) {
                err(issues, format!("{w}.install.url"), "must be an https:// address".into());
            }
            if !inst.enabled() {
                err(issues, format!("{w}.install"), "needs \"github\" or \"url\"".into());
            }
            check_vars(&inst.dir, &format!("{w}.install.dir"), issues);
        }
    }
    ids.clear();
    for (i, c) in cfg.categories.iter().enumerate() {
        let w = index_path("categories", i);
        if !valid_id(&c.id) {
            err(issues, format!("{w}.id"), "must be letters, digits, '-', '_' or '.'".into());
        } else if !ids.insert(c.id.clone()) {
            err(issues, format!("{w}.id"), "duplicate category id".into());
        }
    }
    ids.clear();
    for (i, g) in cfg.games.iter().enumerate() {
        let w = index_path("games", i);
        if !valid_id(&g.id) {
            err(issues, format!("{w}.id"), "must be letters, digits, '-', '_' or '.'".into());
        } else if !ids.insert(g.id.clone()) {
            err(issues, format!("{w}.id"), format!("duplicate game id \"{}\"", g.id));
        }
        for c in &g.categories {
            if cfg.find_category(c).is_none() {
                err(issues, format!("{w}.categories"), format!("unknown category \"{c}\""));
            }
        }
        if g.lobby.enabled && g.online_id.is_empty() {
            err(issues, format!("{w}.online.lobby"), "needs the game's online id".into());
        }
        if !g.join_action.is_empty() && !g.actions.iter().any(|a| a.id == g.join_action) {
            err(issues, format!("{w}.online.join"), format!("no action with id \"{}\"", g.join_action));
        }
        check_vars(&g.image, &format!("{w}.image"), issues);
        if let Some(m) = &g.mod_spec {
            let mw = format!("{w}.mod");
            if m.game.executable.is_empty() && m.game.validation_files.is_empty() {
                err(issues, format!("{mw}.game"), "give the game's \"executable\" or \"validationFiles\" so its folder can be recognised".into());
            }
            if m.game.detection.is_empty() {
                err(issues, format!("{mw}.game.detection"), "say where to look for the game (steam, epic, registry or path)".into());
            }
            for r in &m.game.detection {
                if r.kind == "steam" && r.app_id.is_empty() && m.game.steam_app_id.is_empty() && r.install_dir.is_empty() {
                    err(issues, format!("{mw}.game.detection"), "a steam rule needs an appId (or game.steamAppId) or an installDir".into());
                }
            }
            if m.components.is_empty() {
                err(issues, format!("{mw}.components"), "add at least one component".into());
            }
            let mut cids = BTreeSet::new();
            for c in &m.components {
                if !valid_id(&c.id) {
                    err(issues, format!("{mw}.components"), "component ids must be letters, digits, '-', '_' or '.'".into());
                } else if !cids.insert(c.id.clone()) {
                    err(issues, format!("{mw}.components"), format!("duplicate component id \"{}\"", c.id));
                }
                if c.source.is_empty() == c.source_game.is_empty() {
                    err(
                        issues,
                        format!("{mw}.components.{}", c.id),
                        "give either \"source\" (a path inside the package) or \"sourceGame\" (files in the game folder)".into(),
                    );
                }
                if !c.target_name.is_empty() && (c.target_name.contains(['/', '\\']) || c.target_name == "." || c.target_name == "..") {
                    err(issues, format!("{mw}.components.{}.targetName", c.id), "a file name, not a path".into());
                }
                for sg in &c.source_game {
                    if norm_rel(sg).is_empty() || sg.contains("..") {
                        err(issues, format!("{mw}.components.{}.sourceGame", c.id), "paths inside the game folder".into());
                    }
                }
                check_vars(&c.source, &format!("{mw}.components"), issues);
                check_vars(&c.target, &format!("{mw}.components"), issues);
            }
            if m.package.mods_info.is_empty() && m.package.url.is_empty() && m.package.github.is_empty() {
                err(issues, format!("{mw}.package"), "give \"github\" (owner/repo), \"modsInfo\" (the mod's id in mods-info) or a \"url\"".into());
            }
            if !m.package.url.is_empty() && !starts_with_i(&m.package.url, "https://") {
                err(issues, format!("{mw}.package.url"), "must be an https:// address".into());
            }
            if !m.package.github.is_empty() && !m.package.github.contains('/') {
                err(issues, format!("{mw}.package.github"), "must look like \"owner/repo\"".into());
            }
            let mut extra_dirs = BTreeSet::new();
            for (k, e) in m.package.extras.iter().enumerate() {
                let ew = format!("{mw}.package.extras[{k}]");
                if e.dir.is_empty() || norm_rel(&e.dir).is_empty() || e.dir.contains(['/', '\\']) {
                    err(issues, format!("{ew}.dir"), "a single folder name, e.g. \"loader\"".into());
                } else if !extra_dirs.insert(to_lower(&e.dir)) {
                    err(issues, format!("{ew}.dir"), "used twice".into());
                }
                if e.url.is_empty() && (e.github.is_empty() || e.asset.is_empty()) {
                    err(issues, ew.clone(), "give a \"url\", or \"github\" and \"asset\"".into());
                }
                if !e.url.is_empty() && !starts_with_i(&e.url, "https://") {
                    err(issues, format!("{ew}.url"), "must be an https:// address".into());
                }
            }
            for (k, r) in m.install.requires.iter().enumerate() {
                if r.path.is_empty() || norm_rel(&r.path).is_empty() {
                    err(issues, format!("{mw}.install.requires[{k}].path"), "a path inside the game folder".into());
                }
            }
            for (k, pat) in m.install.uninstall_cleanup.iter().enumerate() {
                let prefix = match pat.find(['*', '?']) {
                    Some(p) => &pat[..p],
                    None => pat.as_str(),
                };
                if norm_rel(pat).is_empty() || (!prefix.is_empty() && norm_rel(prefix).is_empty()) || pat.starts_with('*') {
                    err(
                        issues,
                        format!("{mw}.install.uninstallCleanup[{k}]"),
                        "must be a path inside the game folder that starts with a folder name, e.g. \"Loader/cache/**\"".into(),
                    );
                }
            }
            if iequals(&m.install.state_dir, &m.install.target_dir) || iequals(&m.install.state_dir, "${GAME_DIR}") {
                err(issues, format!("{mw}.install.stateDir"), "must be a sub-folder, not the install or game folder itself".into());
            }
            check_vars(&m.install.target_dir, &format!("{mw}.install"), issues);
            check_vars(&m.install.state_dir, &format!("{mw}.install"), issues);
            for d in &m.game.detection {
                check_vars(&d.path, &format!("{mw}.game.detection"), issues);
                check_vars(&d.install_dir, &format!("{mw}.game.detection"), issues);
            }
        } else if g.actions.is_empty() && !g.coming_soon {
            err(issues, format!("{w}.actions"), "add at least one action (e.g. \"Play\")".into());
        }
        let mut action_ids = BTreeSet::new();
        for (j, a) in g.actions.iter().enumerate() {
            let aw = format!("{w}.{}", index_path("actions", j));
            if !valid_id(&a.id) {
                err(issues, format!("{aw}.id"), "must be letters, digits, '-', '_' or '.'".into());
            } else if !action_ids.insert(a.id.clone()) {
                err(issues, format!("{aw}.id"), format!("duplicate action id \"{}\"", a.id));
            }
            if a.is_mod() {
                continue; // made by the launcher: no file to check
            }
            if a.is_link() {
                if !starts_with_i(&a.link, "https://") {
                    err(issues, format!("{aw}.link"), "must be an https:// address".into());
                }
                continue;
            }
            if a.is_web() {
                if a.server.enabled() && a.server.url.is_empty() {
                    err(issues, format!("{aw}.server.url"), "give the address the game opens at, e.g. \"http://localhost:2053/\"".into());
                }
                for pid in &a.players {
                    if let Some(p) = cfg.find_player(pid)
                        && p.kind != "browser"
                    {
                        err(issues, format!("{aw}.players"), format!("\"{pid}\" is not a browser; HTML5 games need one"));
                    }
                }
                if !cfg.players.iter().any(|p| p.kind == "browser") {
                    err(issues, aw.clone(), "HTML5 games need a player with \"kind\": \"browser\"".into());
                }
                for re in [&a.server.share, &a.server.lan] {
                    if !re.is_empty() && !pattern_ok(re) {
                        err(issues, format!("{aw}.server"), format!("not a valid pattern: {re}"));
                    }
                }
            } else if a.swf.is_empty() {
                let mut all = !a.overrides.is_empty();
                for p in &cfg.players {
                    if a.players.is_empty() || a.players.contains(&p.id) {
                        all = all && a.overrides.get(&p.id).is_some_and(|o| o.has_swf);
                    }
                }
                if !all {
                    err(issues, format!("{aw}.swf"), "list where the game's .swf file is".into());
                }
            }
            for s in &a.swf {
                check_vars(s, &format!("{aw}.swf"), issues);
            }
            for s in &a.args {
                check_vars(s, &format!("{aw}.args"), issues);
            }
            for (_, v) in &a.flashvars {
                check_vars(v, &format!("{aw}.flashvars"), issues);
            }
            for p in &a.players {
                if cfg.find_player(p).is_none() {
                    err(issues, format!("{aw}.players"), format!("unknown player \"{p}\""));
                }
            }
            for (pid, o) in &a.overrides {
                if cfg.find_player(pid).is_none() {
                    err(issues, format!("{aw}.overrides"), format!("unknown player \"{pid}\""));
                }
                for s in &o.swf {
                    check_vars(s, &format!("{aw}.overrides"), issues);
                }
                for s in &o.args {
                    check_vars(s, &format!("{aw}.overrides"), issues);
                }
            }
        }
    }
    check_vars(&cfg.paths.games, "paths.games", issues);
    check_vars(&cfg.paths.players, "paths.players", issues);
    check_vars(&cfg.paths.data, "paths.data", issues);
}

/// The issues as the C++ launcher prints them ("error   games[0].id: ...").
pub fn format_issues(issues: &[ConfigIssue]) -> String {
    let mut out = String::new();
    for i in issues {
        out.push_str(if i.severity == Severity::Error { "error   " } else { "warning " });
        out.push_str(if i.place.is_empty() { "(top level)" } else { &i.place });
        out.push_str(": ");
        out.push_str(&i.message);
        out.push('\n');
    }
    out
}
