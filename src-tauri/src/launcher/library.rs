//! Turns the configuration into something playable (old/src/core/library.cpp): finds each player's exe and each
//! action's game file, and builds the exact command line. Pure lookups; `refresh` again after files change.

use super::config::{ActionSpec, Config, GameSpec, PlayerOverride, PlayerSpec};
use super::paths;
use super::settings::UserData;
use super::text::starts_with_i;
use super::vars::Vars;

#[derive(Clone, Debug)]
pub struct PlayerStatus {
    pub index: usize, // into Config::players
    pub exe: String,  // empty when not found
}

impl PlayerStatus {
    pub fn found(&self) -> bool {
        !self.exe.is_empty()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ActionState {
    Ready,
    NoPlayer,
    NoSwf,
    NoRuntime,
}

impl ActionState {
    /// The word the window's JSON uses.
    pub fn id(self) -> &'static str {
        match self {
            ActionState::Ready => "ready",
            ActionState::NoPlayer => "noPlayer",
            ActionState::NoSwf => "noFile",
            ActionState::NoRuntime => "noRuntime",
        }
    }
}

#[derive(Clone, Debug)]
pub struct ResolvedAction<'c> {
    pub state: ActionState,
    pub player: Option<&'c PlayerSpec>, // the chosen player, or the first allowed one when none is found
    pub player_exe: String,
    pub swf: String,
    pub work_dir: String,
    pub args: Vec<String>, // the final argv after the exe
    pub swf_is_url: bool,
    pub user_swf: bool, // the file came from Locate, not the config
    pub web: bool,
    pub page: String,
    pub page_local: bool,
    pub url: String,
    pub server_exe: String,
    pub server_cwd: String,
    pub server_args: Vec<String>,
}

impl Default for ResolvedAction<'_> {
    fn default() -> Self {
        ResolvedAction {
            state: ActionState::NoSwf,
            player: None,
            player_exe: String::new(),
            swf: String::new(),
            work_dir: String::new(),
            args: Vec::new(),
            swf_is_url: false,
            user_swf: false,
            web: false,
            page: String::new(),
            page_local: false,
            url: String::new(),
            server_exe: String::new(),
            server_cwd: String::new(),
            server_args: Vec::new(),
        }
    }
}

pub struct Library<'c> {
    cfg: &'c Config,
    vars: Vars,
    players: Vec<PlayerStatus>,
    games_override: String,
}

pub fn is_url(s: &str) -> bool {
    starts_with_i(s, "http://") || starts_with_i(s, "https://")
}

/// Expands one candidate; empty when it uses a variable that is unknown or empty (${FLASHPOINT} on a PC without
/// Flashpoint), so that candidate is skipped rather than looked for in the wrong place.
fn expand_candidate(raw: &str, vars: &Vars) -> String {
    let mut unknown = Vec::new();
    let s = vars.expand_unknown(raw, &mut unknown);
    if !unknown.is_empty() {
        return String::new();
    }
    let mut i = 0;
    while let Some(start) = raw[i..].find("${").map(|p| p + i) {
        let Some(end) = raw[start..].find('}').map(|p| p + start) else { break };
        let name = &raw[start + 2..end];
        if !starts_with_i(name, "env:") && !starts_with_i(name, "folder:") && vars.get(name).is_empty() {
            return String::new();
        }
        i = end + 1;
    }
    s
}

/// Natural order: "flashplayer_32_sa.exe" sorts after "flashplayer_9_sa.exe".
fn natural_less(a: &str, b: &str) -> bool {
    let (a, b): (Vec<char>, Vec<char>) = (a.chars().collect(), b.chars().collect());
    let (mut i, mut j) = (0, 0);
    while i < a.len() && j < b.len() {
        if a[i].is_ascii_digit() && b[j].is_ascii_digit() {
            let (mut i2, mut j2) = (i, j);
            while i2 < a.len() && a[i2].is_ascii_digit() {
                i2 += 1;
            }
            while j2 < b.len() && b[j2].is_ascii_digit() {
                j2 += 1;
            }
            let num = |s: &[char]| s.iter().fold(0u64, |v, c| v.saturating_mul(10).saturating_add(c.to_digit(10).unwrap() as u64));
            let (x, y) = (num(&a[i..i2]), num(&b[j..j2]));
            if x != y {
                return x < y;
            }
            i = i2;
            j = j2;
            continue;
        }
        let (x, y) = (a[i].to_lowercase().next().unwrap_or(a[i]), b[j].to_lowercase().next().unwrap_or(b[j]));
        if x != y {
            return x < y;
        }
        i += 1;
        j += 1;
    }
    a.len() - i < b.len() - j
}

/// The highest-named file a wildcard matches.
fn glob_file(pattern: &str) -> String {
    let mut names = paths::find_files(pattern);
    if names.is_empty() {
        return String::new();
    }
    names.sort_by(|x, y| {
        if natural_less(x, y) {
            std::cmp::Ordering::Less
        } else if natural_less(y, x) {
            std::cmp::Ordering::Greater
        } else {
            std::cmp::Ordering::Equal
        }
    });
    paths::join(&paths::parent(pattern), names.last().unwrap())
}

impl<'c> Library<'c> {
    /// `base` holds LAUNCHER_DIR; the library adds GAMES_DIR, PLAYERS_DIR, DATA_DIR and FLASHPOINT.
    pub fn new(cfg: &'c Config, mut base: Vars) -> Self {
        base.set("TITLE", &cfg.branding.title);
        base.set("SUBTITLE", &cfg.branding.subtitle);
        let mut lib = Library { cfg, vars: base, players: Vec::new(), games_override: String::new() };
        lib.refresh();
        lib
    }

    pub fn config(&self) -> &'c Config {
        self.cfg
    }
    pub fn vars(&self) -> &Vars {
        &self.vars
    }
    pub fn players(&self) -> &[PlayerStatus] {
        &self.players
    }
    pub fn flashpoint_dir(&self) -> String {
        self.vars.get("FLASHPOINT")
    }
    pub fn data_dir(&self) -> String {
        self.vars.get("DATA_DIR")
    }
    /// A games folder chosen in Settings; empty = paths.games. Call `refresh` after.
    pub fn set_games_dir_override(&mut self, dir: &str) {
        self.games_override = dir.to_string();
    }

    /// First existing path among candidates: variables expanded, '*' allowed in the file name, a bare name
    /// searched on PATH, web addresses passed through. Empty when none exists.
    pub fn find_first(&self, candidates: &[String], vars: &Vars) -> String {
        for raw in candidates {
            let s = expand_candidate(raw, vars);
            if s.is_empty() {
                continue;
            }
            if is_url(&s) {
                return s;
            }
            if !s.contains('\\') && !s.contains('/') {
                let found = paths::search_path(&s);
                if !found.is_empty() {
                    return paths::normalize(&found);
                }
                continue;
            }
            let s = paths::normalize(&s);
            if paths::file_name(&s).contains(['*', '?']) {
                let found = glob_file(&s);
                if !found.is_empty() {
                    return found;
                }
            } else if paths::is_file(&s) {
                return s;
            }
        }
        String::new()
    }

    pub fn refresh(&mut self) {
        let cfg = self.cfg;
        let games = if self.games_override.is_empty() { self.vars.expand(&cfg.paths.games) } else { self.games_override.clone() };
        self.vars.set("GAMES_DIR", &paths::normalize(&games));
        let players = paths::normalize(&self.vars.expand(&cfg.paths.players));
        self.vars.set("PLAYERS_DIR", &players);
        let data = paths::normalize(&self.vars.expand(&cfg.paths.data));
        self.vars.set("DATA_DIR", &data);
        let mut flashpoint = String::new();
        for raw in &cfg.paths.flashpoint {
            let p = paths::normalize(&self.vars.expand(raw));
            if !p.is_empty() && paths::is_dir(&paths::join(&p, "FPSoftware")) {
                flashpoint = p;
                break;
            }
        }
        self.vars.set("FLASHPOINT", &flashpoint);
        self.players = cfg.players.iter().enumerate().map(|(index, p)| PlayerStatus { index, exe: self.find_first(&p.paths, &self.vars) }).collect();
    }

    pub fn player(&self, id: &str) -> Option<&PlayerStatus> {
        self.players.iter().find(|p| self.cfg.players[p.index].id == id)
    }

    /// Players this action may use, in the order they would be tried: Flash files go to Flash players, HTML5
    /// pages to browsers, and the user's choice goes first.
    pub fn allowed_players(&self, a: &ActionSpec, preferred: &str) -> Vec<&'c PlayerSpec> {
        let kind = if a.is_web() { "browser" } else { "flash" };
        let mut out: Vec<&'c PlayerSpec> = if a.players.is_empty() {
            self.cfg.players.iter().filter(|p| p.kind == kind).collect()
        } else {
            a.players.iter().filter_map(|id| self.cfg.find_player(id)).filter(|p| p.kind == kind).collect()
        };
        if let Some(pos) = out.iter().position(|p| p.id == preferred) {
            out[..=pos].rotate_right(1);
        }
        out
    }

    /// `joining`: a room code comes with this launch, so the action's joinArgs replace its args.
    pub fn resolve(&self, g: &GameSpec, a: &ActionSpec, user: &UserData, joining: bool) -> ResolvedAction<'c> {
        let r = ResolvedAction::default();
        let stats = user.find(&g.id);
        let mut v = self.vars.clone();
        v.set("GAME_ID", &g.id);
        v.set("GAME_NAME", &g.name);
        v.set("PORT", &a.relay.port.to_string());
        if a.is_link() {
            return ResolvedAction { state: ActionState::Ready, ..r };
        }
        let user_swf = stats.and_then(|s| s.swf.get(&a.id)).filter(|f| paths::is_file(f)).cloned().unwrap_or_default();
        let preferred = stats.map(|s| s.player.as_str()).filter(|p| !p.is_empty()).unwrap_or(&user.default_player).to_string();
        if a.is_web() {
            return self.resolve_web(r, a, v, &user_swf, &preferred);
        }

        let (mut any_player, mut any_swf) = (false, false);
        let mut fallback: ResolvedAction<'c> = ResolvedAction::default(); // the best partial match
        for p in self.allowed_players(a, &preferred) {
            let st = self.player(&p.id);
            let ov: Option<&PlayerOverride> = a.overrides.get(&p.id);
            let mut swf = user_swf.clone();
            if swf.is_empty() {
                swf = self.find_first(if let Some(o) = ov.filter(|o| o.has_swf) { &o.swf } else { &a.swf }, &v);
            }
            let has_exe = st.is_some_and(|s| s.found());
            any_player |= has_exe;
            any_swf |= !swf.is_empty();

            let mut c = r.clone();
            c.player = Some(p);
            c.player_exe = if has_exe { st.unwrap().exe.clone() } else { String::new() };
            c.swf = swf.clone();
            c.swf_is_url = is_url(&swf);
            c.user_swf = !user_swf.is_empty();
            if has_exe && !swf.is_empty() {
                let mut av = v.clone();
                av.set("SWF", &swf);
                av.set("SWF_DIR", &if c.swf_is_url { paths::parent(&c.player_exe) } else { paths::parent(&swf) });
                let mut mentions_swf = false;
                let mut add = |list: &[String], c: &mut ResolvedAction| {
                    for arg in list {
                        if arg.contains("${SWF}") {
                            mentions_swf = true;
                        }
                        c.args.push(av.expand(arg));
                    }
                };
                add(&p.args, &mut c);
                let extra = if joining && !a.join_args.is_empty() {
                    &a.join_args
                } else if let Some(o) = ov.filter(|o| o.has_args) {
                    &o.args
                } else {
                    &a.args
                };
                add(extra, &mut c);
                if !mentions_swf {
                    c.args.push(swf.clone());
                }
                // Flash loads a game's extra files relative to the working folder.
                c.work_dir = av.get("SWF_DIR");
                c.state = ActionState::Ready;
                return c;
            }
            // Prefer an installed player; among those (or with none installed), one that has the file.
            if fallback.player.is_none()
                || (has_exe && fallback.player_exe.is_empty())
                || (!swf.is_empty() && fallback.swf.is_empty() && has_exe == !fallback.player_exe.is_empty())
            {
                fallback = c;
            }
        }
        // An installed player without its file is a file problem (download or locate it), even when another
        // player's copy "exists", such as a web address for a player that isn't installed. Only with no player at
        // all is it a player problem.
        fallback.state = if !any_player && any_swf { ActionState::NoPlayer } else { ActionState::NoSwf };
        fallback
    }

    fn resolve_web(&self, mut r: ResolvedAction<'c>, a: &ActionSpec, mut v: Vars, user_file: &str, preferred: &str) -> ResolvedAction<'c> {
        r.web = true;
        // The file the game needs (a server script, a game folder's index.html...).
        let mut file = user_file.to_string();
        if file.is_empty() && !a.swf.is_empty() {
            file = self.find_first(&a.swf, &v);
        }
        if !file.is_empty() {
            for name in ["FILE", "SWF"] {
                v.set(name, &file);
            }
            for name in ["FILE_DIR", "SWF_DIR"] {
                v.set(name, &paths::parent(&file));
            }
        }
        r.swf = file.clone();
        r.user_swf = !user_file.is_empty();
        if !a.page.is_empty() {
            r.page = self.find_first(&a.page, &v);
            r.page_local = !r.page.is_empty() && !is_url(&r.page);
            if r.swf.is_empty() {
                r.swf = r.page.clone();
            }
            r.swf_is_url = is_url(&r.page);
        }
        // Browser: the first allowed one that is installed.
        for p in self.allowed_players(a, preferred) {
            let st = self.player(&p.id);
            let found = st.is_some_and(|s| s.found());
            if r.player.is_none() || (found && r.player_exe.is_empty()) {
                r.player = Some(p);
                r.player_exe = if found { st.unwrap().exe.clone() } else { String::new() };
            }
        }
        if (!a.swf.is_empty() && file.is_empty()) || (!a.page.is_empty() && r.page.is_empty()) {
            r.state = ActionState::NoSwf;
            return r;
        }
        if r.player_exe.is_empty() {
            r.state = ActionState::NoPlayer;
            return r;
        }
        if a.server.enabled() {
            r.server_exe = self.find_first(&a.server.exe, &v);
            if r.server_exe.is_empty() {
                r.state = ActionState::NoRuntime;
                return r;
            }
            r.server_args = a.server.args.iter().map(|arg| v.expand(arg)).collect();
            r.server_cwd = paths::normalize(&if a.server.cwd.is_empty() { v.get("FILE_DIR") } else { v.expand(&a.server.cwd) });
            r.url = v.expand(&a.server.url);
        } else if !r.page.is_empty() && !r.page_local {
            r.url = r.page.clone();
        }
        // ${URL} stays unexpanded here: a served page's port, a room code or a friend's link are only known when
        // the game starts.
        let player = r.player.unwrap();
        let mut mentions_url = false;
        for arg in &player.args {
            if arg.contains("${URL}") {
                mentions_url = true;
            }
            r.args.push(v.expand(arg));
        }
        for arg in &a.args {
            r.args.push(v.expand(arg));
        }
        if !mentions_url {
            r.args.push("${URL}".into());
        }
        r.work_dir = if r.page_local { paths::parent(&r.page) } else { paths::parent(&r.player_exe) };
        r.state = ActionState::Ready;
        r
    }

    /// The game's overall state: Ready when any action can start.
    pub fn game_state(&self, g: &GameSpec, user: &UserData) -> ActionState {
        if g.mod_spec.is_some() {
            return ActionState::Ready; // a PC mod's state is the mod manager's
        }
        let mut best = ActionState::NoSwf;
        for a in &g.actions {
            let s = self.resolve(g, a, user, false).state;
            if s == ActionState::Ready {
                return s;
            }
            if s == ActionState::NoPlayer || s == ActionState::NoRuntime {
                best = s;
            }
        }
        best
    }
}
