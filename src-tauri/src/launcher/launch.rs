//! Starting a game (old/src/app/main.cpp's `--launch` and old/src/ui/launcher_headless.cpp): the checks, the player's
//! exact command line and environment, the host's options, the built-in relay and its reports, the HTML5 web server
//! and game server, Discord, and the play count and time in settings.json. What `--launch <game>:<action> --json
//! --wait [--tv] [--coop-save X] [--join CODE] [--address A]` did, without the C++ launcher; its exit codes and
//! messages are kept, as the window shows them. PC mods still start through the C++ launcher (PORTING.md part 6).

use windows_sys::Win32::Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0};
use windows_sys::Win32::System::Threading::{CreateEventW, SetEvent, WaitForMultipleObjects, WaitForSingleObject};

use super::catalog::{GameOptions, cached_catalog_settings};
use super::config::{ActionSpec, Config};
use super::discord::{DiscordPresence, Presence};
use super::json::{Value, serialize};
use super::library::{ActionState, ResolvedAction};
use super::library_json::{load_with_community, open_library};
use super::paths;
use super::process::{BackgroundProcess, EnvVars, Process, local_addresses, start_process, wait_for_server};
use super::relay::Relay;
use super::rooms::resolve_room_server;
use super::settings::{load_user_data, save_user_data};
use super::text::replace_all;
use super::webserver::WebServer;
use super::{log, vars::Vars};

/// One launch, as the window asks for it.
#[derive(Clone, Debug, Default)]
pub struct Request {
    pub game: String,
    pub action: String,    // empty: the game's first action
    pub join: String,      // a room code as typed
    pub address: String,   // a friend's "host:port"
    pub coop_save: String, // "", "continue", "solo" or "new"
    pub tv: bool,
    pub wait: bool, // stay until the game exits: its time is counted, the relay and Discord last as long
}

/// Ends a running launch from another thread (Stop hosting), as killing the C++ launcher did: the relay, the web
/// server and the game's server stop, the player keeps running, and its time isn't added.
pub struct Stop(HANDLE);

unsafe impl Send for Stop {}
unsafe impl Sync for Stop {}

impl Default for Stop {
    fn default() -> Self {
        Stop(unsafe { CreateEventW(std::ptr::null(), 1, 0, std::ptr::null()) })
    }
}

impl Stop {
    pub fn set(&self) {
        unsafe { SetEvent(self.0) };
    }
    pub fn is_set(&self) -> bool {
        unsafe { WaitForSingleObject(self.0, 0) == WAIT_OBJECT_0 }
    }
}

impl Drop for Stop {
    fn drop(&mut self) {
        unsafe { CloseHandle(self.0) };
    }
}

enum Waited {
    Exited,
    Stopped,
    Timeout,
}

fn wait(p: &Process, stop: &Stop, ms: u32) -> Waited {
    let handles = [p.raw(), stop.0];
    match unsafe { WaitForMultipleObjects(2, handles.as_ptr(), 0, ms) } {
        WAIT_OBJECT_0 => Waited::Exited,
        x if x == WAIT_OBJECT_0 + 1 => Waited::Stopped,
        _ => Waited::Timeout,
    }
}

/// A fixed port per game keeps HTML5 saves: browsers file storage by address.
pub fn stable_port(game_id: &str) -> i32 {
    let mut h: u32 = 2166136261;
    for b in game_id.bytes() {
        h = (h ^ b as u32).wrapping_mul(16777619);
    }
    47000 + (h % 2000) as i32
}

/// The action's own values first (online mode, your name), then this launch's (a room code). Empty values are left
/// out. An action that takes its room on the command line (joinArgs) must give the game the same settings on every
/// PC, so the room code is not passed as a value too.
pub fn game_values(a: &ActionSpec, player_name: &str, given: &[(String, String)]) -> Vec<(String, String)> {
    let mut out: Vec<(String, String)> = Vec::new();
    for (key, value) in &a.flashvars {
        let v = replace_all(value, "${PLAYER_NAME}", player_name);
        if !v.is_empty() {
            out.push((key.clone(), v));
        }
    }
    if a.join_args.is_empty() {
        out.extend(given.iter().cloned());
    }
    out
}

/// Values for the game go in before the game file, the way the player expects options; players without
/// flashvarArg can't take them.
pub fn add_flash_vars(r: &mut ResolvedAction, vars: &[(String, String)]) {
    let Some(player) = r.player else { return };
    if vars.is_empty() || player.flashvar_arg.is_empty() {
        return;
    }
    let pos = r.args.iter().rposition(|a| *a == r.swf).unwrap_or(r.args.len());
    let extra: Vec<String> = vars.iter().map(|(k, v)| replace_all(&replace_all(&player.flashvar_arg, "${KEY}", k), "${VALUE}", v)).collect();
    r.args.splice(pos..pos, extra);
}

/// Fills ${PLAYER_NAME}, ${CODE}, ${SERVER} and ${ADDRESS} in the final arguments (the game file and flashvars never
/// contain them). Returns the name of a placeholder that has no value.
pub fn fill_placeholders(args: &mut [String], name: &str, code: &str, server: &str, address: &str) -> Option<&'static str> {
    let values = [("PLAYER_NAME", name), ("CODE", code), ("SERVER", server), ("ADDRESS", address)];
    for arg in args.iter_mut() {
        for (key, value) in values {
            let token = format!("${{{key}}}");
            if !arg.contains(&token) {
                continue;
            }
            if value.is_empty() && key != "PLAYER_NAME" {
                return Some(key);
            }
            *arg = replace_all(arg, &token, value);
        }
    }
    None
}

/// A friend's address typed without a port gets the one the game's direct hosts listen on, since most people copy only
/// the IP. A bare IPv6 address is left alone: its colons can't be told from a port.
pub fn with_default_port(address: &str, port: i32) -> String {
    let a = address.trim();
    if a.is_empty() {
        return String::new();
    }
    if let Some(host) = a.strip_suffix(':') {
        if !host.contains(':') || host.ends_with(']') {
            return format!("{host}:{port}");
        }
    }
    if a.ends_with(']') || !a.contains(':') {
        return format!("{a}:{port}");
    }
    a.to_string()
}

pub fn state_reason(cfg: &Config, a: &ActionSpec, state: ActionState) -> String {
    match state {
        ActionState::NoRuntime => cfg.t("status.noRuntime", &[("RUNTIME", &a.server.name)]),
        ActionState::NoSwf => cfg.t("status.noSwf", &[]),
        ActionState::NoPlayer => cfg.t("status.noPlayer", &[]),
        ActionState::Ready => cfg.t("status.ready", &[]),
    }
}

/// --join as the C++ launcher normalised it: letters and digits only, upper case.
pub fn normalize_code(join: &str) -> String {
    join.chars().filter(|c| c.is_alphanumeric()).map(|c| c.to_ascii_uppercase()).collect()
}

fn unix_now() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

/// The hosting box's view: where friends connect, and who has.
fn relay_line(relay: &Relay) -> String {
    let mut rel = Value::object();
    rel.set("port", relay.port());
    let addrs: Vec<Value> = local_addresses()
        .into_iter()
        .map(|a| {
            let mut o = Value::object();
            o.set("address", a.address);
            o.set("adapter", a.adapter);
            o
        })
        .collect();
    rel.set("addresses", addrs);
    let peers: Vec<Value> = relay
        .snapshot()
        .peers
        .into_iter()
        .map(|p| {
            let mut o = Value::object();
            o.set("name", p.name);
            o.set("address", p.address);
            o.set("host", p.host);
            o
        })
        .collect();
    rel.set("peers", peers);
    let mut o = Value::object();
    o.set("relay", rel);
    serialize(&o, false)
}

/// The config file the app reads, for the log's first line.
pub(crate) fn config_source() -> String {
    match std::env::var("FL_LAUNCHER_CONFIG") {
        Ok(c) if !c.is_empty() => paths::full_path(&c),
        _ => {
            let beside = paths::join(&paths::self_exe_dir(), "launcher.json");
            if paths::is_file(&beside) { beside } else { "built into the exe".into() }
        }
    }
}

/// Starts the action and, with `wait` (or a relay or an HTML5 server that must outlive the game), stays until the
/// game exits. `report` gets the relay's state as a JSON line whenever it changes. Returns the C++ launcher's exit
/// code and the message it printed: the launched text on success, the reason otherwise. A stop returns (0, "").
pub fn run(req: &Request, report: &mut dyn FnMut(&str), stop: &Stop) -> (i32, String) {
    // With mods-info's records: a Flash game from a team record starts like one launcher.json has.
    let cfg = match load_with_community() {
        Ok(c) => c,
        Err(e) => return (3, e),
    };
    let mut base = Vars::default();
    base.set("LAUNCHER_DIR", &cfg.config_dir);
    log::open(&paths::join(&paths::normalize(&base.expand(&cfg.paths.data)), "launcher.log"));
    log::info(&format!("Config: {}", config_source()));
    let (lib, mut user) = open_library(&cfg);
    for p in lib.players() {
        let spec = &cfg.players[p.index];
        log::info(&format!("{}: {}", spec.name, if p.found() { p.exe.as_str() } else { "not found" }));
    }
    let user_path = paths::join(&lib.data_dir(), "settings.json");

    let fail = |code: i32, msg: String| {
        log::error(&msg);
        (code, msg)
    };
    let Some(game) = cfg.find_game(&req.game) else {
        return fail(3, format!("No game with id \"{}\" (see --list)", req.game));
    };
    if game.coming_soon {
        return fail(4, format!("{} is coming soon: its online mod isn't ready yet.", game.name));
    }
    let action = if req.action.is_empty() { game.actions.first() } else { game.actions.iter().rev().find(|a| a.id == req.action) };
    let Some(action) = action else {
        return fail(3, format!("\"{}\" has no action \"{}\"", game.name, req.action));
    };
    let join_code = normalize_code(&req.join);
    let address = with_default_port(&req.address, game.lobby.port);
    let save_choice = match req.coop_save.as_str() {
        "" | "continue" => 0,
        "solo" => 1,
        "new" => 2,
        _ => return fail(3, "--coop-save takes continue, solo or new".into()),
    };
    if game.mod_spec.is_some() {
        return fail(3, format!("{} is a PC mod: it starts through the C++ launcher.", game.name));
    }
    if action.is_link() {
        return fail(3, format!("\"{}\" opens {} in your browser and needs the launcher window.", action.label, action.link));
    }
    let mut r = lib.resolve(game, action, &user, !join_code.is_empty());
    if r.state != ActionState::Ready {
        let reason = state_reason(&cfg, action, r.state);
        return fail(4, cfg.t("notice.notReady", &[("GAME", &game.name), ("REASON", &reason)]));
    }
    let player_name = r.player.map(|p| p.name.clone()).unwrap_or_default();
    let mut relay = Relay::new();
    if action.relay.enabled {
        if let Err((err, in_use)) = relay.start(action.relay.port) {
            // Another relay on the port (likely the window's own, hosting already) carries the game.
            if !in_use {
                return fail(1, cfg.t("host.failed", &[("PORT", &action.relay.port.to_string()), ("ERROR", &err)]));
            }
        }
    }
    // HTML5: the local web server or the game's own server comes first.
    let mut server = BackgroundProcess::default();
    let mut web = WebServer::new();
    if r.web {
        let mut url = r.url.clone();
        if action.join_link {
            return fail(3, format!("\"{}\" opens a friend's link and needs the launcher window.", action.label));
        }
        if r.page_local {
            let root = paths::parent(&r.page);
            if let Err(err) = web.start(&root, stable_port(&game.id)).or_else(|_| web.start(&root, 0)) {
                return fail(1, cfg.t("notice.launchFailed", &[("PLAYER", &player_name), ("ERROR", &err)]));
            }
            url = format!("http://127.0.0.1:{}/{}", web.port(), paths::file_name(&r.page));
        }
        if action.server.enabled() {
            if let Err(err) = server.start(&r.server_exe, &r.server_args, &r.server_cwd, |line| log::info(&format!("[server] {line}"))) {
                return fail(1, cfg.t("notice.launchFailed", &[("PLAYER", &action.server.name), ("ERROR", &err)]));
            }
            if !wait_for_server(&url, 30000, || server.running() && !stop.is_set()) {
                if stop.is_set() {
                    return (0, String::new());
                }
                return fail(1, cfg.t("web.serverTimeout", &[("GAME", &game.name)]));
            }
        }
        for arg in &mut r.args {
            *arg = replace_all(arg, "${URL}", &url);
        }
    } else {
        // No window: the room code and address come from --join and --address; the server is the central one.
        let (mut room_server, _) = resolve_room_server(&cfg.rooms.servers, &cfg.rooms.server);
        if room_server.is_empty() {
            room_server = cfg.rooms.server.clone();
        }
        if let Some(missing) = fill_placeholders(&mut r.args, &user.player_name, &join_code, &room_server, &address) {
            let what = if missing == "CODE" { "room code".to_string() } else { format!("value for ${{{missing}}}") };
            return fail(3, format!("\"{}\" needs a {what} and needs the launcher window.", action.label));
        }
        // The host's save choice and the game's settings, as the window adds them.
        let catalog = cached_catalog_settings(&lib.data_dir());
        GameOptions { cfg: &cfg, lib: &lib, user: &user, catalog: &catalog }.add_host_options(game, &mut r.args, save_choice);
        let mut given = Vec::new();
        if !join_code.is_empty() {
            given.push(("join".to_string(), join_code.clone()));
        }
        let values = game_values(action, &user.player_name, &given);
        add_flash_vars(&mut r, &values);
    }
    let mut env: EnvVars = Vec::new();
    if req.tv && !r.web {
        if let Some(p) = r.player {
            r.args.extend(p.tv_args.iter().cloned());
            env = p.tv_env.clone();
        }
    }
    // Hidden: under the C++ launcher a console player shared its hidden console, so it never opened a window of its
    // own that would take the focus from the window (and minimize it in TV mode). Windows ignores this for a
    // windowed player such as Ruffle.
    let process = match start_process(&r.player_exe, &r.args, &r.work_dir, true, &env) {
        Ok(p) => p,
        Err(err) => return fail(1, cfg.t("notice.launchFailed", &[("PLAYER", &player_name), ("ERROR", &err)])),
    };
    let st = user.stats(&game.id);
    st.plays += 1;
    st.last_played = unix_now();
    let _ = save_user_data(&user, &user_path);
    let message = if action.hint.is_empty() { cfg.t("notice.launched", &[("GAME", &game.name), ("PLAYER", &player_name)]) } else { action.hint.clone() };
    if !(relay.running() || r.web || req.wait) {
        return (0, message);
    }
    // The relay or the game's server must outlive the game.
    let start = std::time::Instant::now();
    // "Playing <game>" on the player's Discord profile while it runs, as the window shows it.
    let _presence = (!cfg.discord_client_id.is_empty() && std::env::var_os("FL_NO_DISCORD").is_none()).then(|| {
        let d = DiscordPresence::new(&cfg.discord_client_id);
        let state = if join_code.is_empty() { action.label.clone() } else { cfg.t("discord.room", &[("CODE", &join_code)]) };
        d.set(&Presence { details: cfg.t("discord.playing", &[("GAME", &game.name)]), state, start: unix_now() });
        d
    });
    if relay.running() {
        let mut last = String::new();
        loop {
            let line = relay_line(&relay);
            if line != last {
                report(&line);
            }
            last = line;
            match wait(&process, stop, 1000) {
                Waited::Exited => break,
                Waited::Stopped => return (0, String::new()),
                Waited::Timeout => {}
            }
        }
    } else if let Waited::Stopped = wait(&process, stop, u32::MAX) {
        return (0, String::new());
    }
    // The window writes settings while the game runs: the time is added to what is on disk now instead of writing
    // back the copy read at launch.
    let played = start.elapsed().as_secs() as i64;
    let mut now = load_user_data(&user_path);
    now.stats(&game.id).seconds += played;
    let _ = save_user_data(&now, &user_path);
    (0, message)
}
