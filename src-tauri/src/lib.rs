mod app_update;
mod engine;
pub mod launcher;

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin};
use std::sync::{Arc, Mutex};

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, State};  // Manager: asset_protocol_scope()

/// What the window keeps between calls: where the launcher keeps settings.json, the games running or hosting (for
/// Stop hosting: a PC mod's C++ launcher, or a Rust launch's stop signal), and the lobby session's input.
#[derive(Default)]
struct Shared {
    data_dir: Mutex<Option<PathBuf>>,
    sessions: Arc<Mutex<HashMap<u32, Child>>>,
    stops: Arc<Mutex<HashMap<u32, Arc<launcher::launch::Stop>>>>,
    lobby: Mutex<Option<ChildStdin>>,
}

/// Runs blocking work (the launcher exe) off the window's thread.
async fn blocking<T: Send + 'static>(f: impl FnOnce() -> Result<T, String> + Send + 'static) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f).await.map_err(|e| e.to_string())?
}

/// Every game, its actions and their state, the UI texts, the user's stats and settings: what `--list --json` printed,
/// now built here (src/launcher). The covers are opened to the window's asset protocol so it can show them.
#[tauri::command]
async fn library(app: AppHandle, shared: State<'_, Shared>) -> Result<serde_json::Value, String> {
    let lib = blocking(launcher::library_json::library_json).await?;
    if let Some(dir) = lib.get("dataDir").and_then(|v| v.as_str()) {
        *shared.data_dir.lock().unwrap() = Some(PathBuf::from(dir));
    }
    let scope = app.asset_protocol_scope();
    if let Some(games) = lib.get("games").and_then(|v| v.as_array()) {
        for g in games {
            if let Some(img) = g.get("image").and_then(|v| v.as_str()).filter(|s| !s.is_empty()) {
                let _ = scope.allow_file(img);
            }
        }
    }
    Ok(lib)
}

/// The PC mods: game found, installed and newest version (`--mods --json`; asks mods-info, so it takes a moment).
#[tauri::command]
async fn mods() -> Result<serde_json::Value, String> {
    blocking(|| engine::run_json(&["--mods".into(), "--json".into()])).await
}

/// Downloads and installs a player that has an "install" entry, such as Ruffle Together (src/launcher/install.rs:
/// what `--install-player <id>` did): what the C++ launcher does on its first start, shown as a step of the first-run
/// setup here. A failure is the last line of the message, as the window read the C++ launcher's stderr.
#[tauri::command]
async fn install_player(player: String) -> Result<(), String> {
    let (code, message) = blocking(move || Ok(launcher::install::install_player_command(&player, &mut |_| {}))).await?;
    if code == 0 {
        return Ok(());
    }
    Err(message.trim().lines().last().unwrap_or("The player couldn't be installed").to_string())
}

/// A game's open rooms on the central server: what `--rooms <id> --json` printed, now asked here (src/launcher/rooms).
/// The window reads the answer whether or not the list came, as it read the C++ launcher's output.
#[tauri::command]
async fn rooms(game: String) -> Result<serde_json::Value, String> {
    blocking(move || launcher::rooms::game_rooms(&game).map(|(answer, _listed)| answer)).await
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LaunchEnded {
    session: u32,
    game: String,
    code: i32,
    message: String,
    /// How long the game ran, for "How did it run?" after a long session.
    secs: u64,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct HostStatus {
    session: u32,
    game: String,
    relay: serde_json::Value,
}

/// Starts an action and returns at once (src/launcher/launch.rs: what `--launch game:action --json --wait` did). A
/// host action keeps the relay up until the game closes and reports its state as it changes: those become
/// `host-status` events. `launch-ended` follows when the game exits, with the C++ launcher's exit code and message.
/// PC mods still start through the C++ launcher (PORTING.md part 6).
#[tauri::command]
fn launch(
    app: AppHandle,
    shared: State<'_, Shared>,
    game: String,
    action: String,
    code: Option<String>,
    address: Option<String>,
    coop_save: Option<String>,
    tv: Option<bool>,
) -> Result<u32, String> {
    static NEXT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(1);
    let session = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let is_mod = launcher::library_json::load_with_community().ok().and_then(|c| c.find_game(&game).map(|g| g.mod_spec.is_some()));
    if is_mod == Some(true) {
        return launch_mod(app, &shared, session, game, action, tv);
    }
    let req = launcher::launch::Request {
        game: game.clone(),
        action,
        join: code.unwrap_or_default(),
        address: address.unwrap_or_default(),
        coop_save: coop_save.unwrap_or_default(),
        tv: tv == Some(true),
        // The launch stays until the game exits, so the window knows it is running, its time is counted and Discord
        // shows it.
        wait: true,
    };
    let stop = Arc::new(launcher::launch::Stop::default());
    let stops = shared.stops.clone();
    stops.lock().unwrap().insert(session, stop.clone());
    std::thread::spawn(move || {
        let started = std::time::Instant::now();
        let (code, message) = launcher::launch::run(
            &req,
            &mut |line| {
                if let Some(relay) = serde_json::from_str::<serde_json::Value>(line).ok().and_then(|v| v.get("relay").cloned()) {
                    let _ = app.emit("host-status", HostStatus { session, game: game.clone(), relay });
                }
            },
            &stop,
        );
        // Stop hosting already took it out: that ends the session quietly, as killing the C++ launcher did.
        let stopped = stops.lock().unwrap().remove(&session).is_none();
        let (code, message) = if stopped { (0, String::new()) } else { (code, message) };
        let _ = app.emit("launch-ended", LaunchEnded { session, game, code, message, secs: started.elapsed().as_secs() });
    });
    Ok(session)
}

/// A PC mod's Launch: `--launch game:action --json --wait` on the C++ launcher, whose exit becomes `launch-ended`.
fn launch_mod(app: AppHandle, shared: &Shared, session: u32, game: String, action: String, tv: Option<bool>) -> Result<u32, String> {
    let mut args = vec!["--launch".to_string(), format!("{game}:{action}"), "--json".into(), "--wait".into()];
    if tv == Some(true) {
        args.push("--tv".into());
    }
    let mut child = engine::spawn(&args, false)?;
    let stdout = child.stdout.take();
    let stderr = child.stderr.take();
    let sessions = shared.sessions.clone();
    sessions.lock().unwrap().insert(session, child);
    std::thread::spawn(move || {
        let started = std::time::Instant::now();
        let err_reader = std::thread::spawn(move || {
            let mut text = String::new();
            if let Some(s) = stderr {
                for l in BufReader::new(s).lines().map_while(Result::ok) {
                    text.push_str(&l);
                    text.push('\n');
                }
            }
            text
        });
        let mut message = String::new();
        if let Some(out) = stdout {
            for line in BufReader::new(out).lines().map_while(Result::ok) {
                message = line;
            }
        }
        let err = err_reader.join().unwrap_or_default();
        let child = sessions.lock().unwrap().remove(&session);
        let code = match child {
            Some(mut c) => c.wait().ok().and_then(|s| s.code()).unwrap_or(-1),
            None => 0,
        };
        let message = if code == 0 { message } else { err.trim().to_string() };
        let _ = app.emit("launch-ended", LaunchEnded { session, game, code, message, secs: started.elapsed().as_secs() });
    });
    Ok(session)
}

/// Stop hosting: ends the relay (and an HTML5 game's servers); the game itself keeps running.
#[tauri::command]
fn stop_session(shared: State<'_, Shared>, session: u32) -> Result<(), String> {
    if let Some(stop) = shared.stops.lock().unwrap().remove(&session) {
        stop.set();
    }
    if let Some(mut c) = shared.sessions.lock().unwrap().remove(&session) {
        c.kill().map_err(|e| e.to_string())?;
        let _ = c.wait();
    }
    Ok(())
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct JobEvent {
    game: String,
    line: String,
    done: bool,
    ok: bool,
}

/// Installs, updates, repairs or removes a PC mod (`--install-mod` and the rest). Progress lines arrive as
/// `mod-progress` events, the last one with `done`.
#[tauri::command]
fn mod_job(app: AppHandle, op: String, game: String) -> Result<(), String> {
    let flag = match op.as_str() {
        "install" => "--install-mod",
        "repair" => "--repair-mod",
        "uninstall" => "--uninstall-mod",
        _ => return Err(format!("Unknown mod operation {op}")),
    };
    let args = vec![flag.to_string(), game.clone()];
    engine::find_exe()?;
    std::thread::spawn(move || {
        let progress = |line: &str| {
            let _ = app.emit("mod-progress", JobEvent { game: game.clone(), line: line.to_string(), done: false, ok: true });
        };
        let (ok, line) = match engine::stream(&args, progress) {
            Ok((0, _)) => (true, String::new()),
            Ok((_, err)) => (false, err),
            Err(e) => (false, e),
        };
        let _ = app.emit("mod-progress", JobEvent { game, line, done: true, ok });
    });
    Ok(())
}

/// Downloads a game's files from the catalog (src/launcher/install.rs: what `--download-game <id>` did);
/// `download-ended` says how it went.
#[tauri::command]
fn download_game(app: AppHandle, game: String) -> Result<(), String> {
    std::thread::spawn(move || {
        let (code, message) = launcher::install::download_game_command(&game, &mut |_| {});
        let (ok, line) = if code == 0 { (true, String::new()) } else { (false, message.trim().to_string()) };
        let _ = app.emit("download-ended", JobEvent { game, line, done: true, ok });
    });
    Ok(())
}

/// Reads settings.json, lets `change` edit it, and writes it back beside-and-move, so a crash never leaves half a
/// file. The C++ launcher reads the same file: both windows share favorites, folders and settings.
fn edit_settings(shared: &Shared, change: impl FnOnce(&mut serde_json::Map<String, serde_json::Value>) -> Result<(), String>) -> Result<(), String> {
    let dir = shared.data_dir.lock().unwrap().clone().ok_or("The library hasn't been read yet")?;
    let path = dir.join("settings.json");
    let mut root: serde_json::Value = std::fs::read_to_string(&path)
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_else(|| serde_json::json!({}));
    change(root.as_object_mut().ok_or("settings.json isn't an object")?)?;
    let text = serde_json::to_string_pretty(&root).map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, text).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &path).map_err(|e| e.to_string())
}

fn game_entry<'a>(root: &'a mut serde_json::Map<String, serde_json::Value>, game: &str) -> Result<&'a mut serde_json::Map<String, serde_json::Value>, String> {
    root.entry("games")
        .or_insert_with(|| serde_json::json!({}))
        .as_object_mut()
        .ok_or("settings.json games isn't an object")?
        .entry(game.to_string())
        .or_insert_with(|| serde_json::json!({}))
        .as_object_mut()
        .ok_or_else(|| "bad game entry".to_string())
}

/// Marks a game a favorite.
#[tauri::command]
fn set_favorite(shared: State<'_, Shared>, game: String, on: bool) -> Result<(), String> {
    edit_settings(&shared, |root| {
        game_entry(root, &game)?.insert("favorite".into(), serde_json::Value::Bool(on));
        Ok(())
    })
}

/// One of the Settings window's choices, by its settings.json key.
#[tauri::command]
fn set_setting(shared: State<'_, Shared>, key: String, value: serde_json::Value) -> Result<(), String> {
    const KEYS: [&str; 9] = ["minimize", "online", "defaultPlayer", "gamesDir", "playerName", "showOffline", "showOriginals", "autoUpdate", "compactCards"];
    if !KEYS.contains(&key.as_str()) {
        return Err(format!("{key} isn't a setting"));
    }
    // The C++ launcher keeps "minimize" as a number.
    let value = if key == "minimize" { serde_json::json!(if value.as_bool().unwrap_or(false) { 1 } else { 0 }) } else { value };
    edit_settings(&shared, |root| {
        root.insert(key, value);
        Ok(())
    })
}

/// A PC mod's game folder the player chose (the mod commands use it), or empty to detect it again.
#[tauri::command]
fn set_game_dir(shared: State<'_, Shared>, game: String, dir: String) -> Result<(), String> {
    edit_settings(&shared, |root| {
        let e = game_entry(root, &game)?;
        if dir.is_empty() {
            e.remove("gameDir");
        } else {
            e.insert("gameDir".into(), serde_json::Value::String(dir));
        }
        Ok(())
    })
}

/// One of a game's own settings (a checkbox "1"/"0" or a choice id), as the C++ panel saves it.
#[tauri::command]
fn set_game_setting(shared: State<'_, Shared>, game: String, id: String, value: String) -> Result<(), String> {
    edit_settings(&shared, |root| {
        let e = game_entry(root, &game)?;
        let sets = e.entry("settings").or_insert_with(|| serde_json::json!({}));
        sets.as_object_mut().ok_or("settings isn't an object")?.insert(id, serde_json::Value::String(value));
        Ok(())
    })
}

/// The player that runs a game; empty goes back to automatic (the config's order).
#[tauri::command]
fn set_game_player(shared: State<'_, Shared>, game: String, player: String) -> Result<(), String> {
    edit_settings(&shared, |root| {
        let e = game_entry(root, &game)?;
        if player.is_empty() {
            e.remove("player");
        } else {
            e.insert("player".into(), serde_json::Value::String(player));
        }
        Ok(())
    })
}

/// Every online game's open rooms in one call (what `--rooms all --json` printed), for the cards' room counts.
#[tauri::command]
async fn all_rooms() -> Result<serde_json::Value, String> {
    blocking(launcher::rooms::all_rooms).await
}

/// Whether a player the launcher installed has a newer release, without installing it (what
/// `--update-players --check --json` printed), so the window can ask first.
#[tauri::command]
async fn check_players() -> Result<serde_json::Value, String> {
    blocking(|| {
        let mut answer = String::new();
        let (code, message) = launcher::install::update_players_command(true, true, &mut |line| answer = line.to_string());
        if answer.is_empty() {
            return Err(if message.is_empty() { format!("The launcher printed nothing (exit {code})") } else { message });
        }
        serde_json::from_str(&answer).map_err(|e| format!("The launcher's answer isn't JSON: {e}"))
    })
    .await
}

/// Brings the players the launcher installed up to their newest release (what `--update-players --json` printed).
/// While a release downloads, its `{"progress":...}` lines become `player-progress` events; the last line is the
/// answer.
#[tauri::command]
async fn update_players(app: AppHandle) -> Result<serde_json::Value, String> {
    blocking(move || {
        let mut last = String::new();
        let (code, err) = launcher::install::update_players_command(false, true, &mut |line| {
            match serde_json::from_str::<serde_json::Value>(line) {
                Ok(v) if v.get("progress").is_some() => {
                    let _ = app.emit("player-progress", v["progress"].clone());
                }
                _ => last = line.to_string(),
            }
        });
        serde_json::from_str(last.trim()).map_err(|_| if err.trim().is_empty() { format!("The update stopped (exit {code})") } else { err.trim().to_string() })
    })
    .await
}

/// How long a connection to the central server takes, in milliseconds (the status bar's "Server online (N ms)").
#[tauri::command]
async fn ping(server: String) -> Result<u32, String> {
    blocking(move || {
        use std::net::{TcpStream, ToSocketAddrs};
        let addr = server.to_socket_addrs().map_err(|e| e.to_string())?.next().ok_or("no address")?;
        let start = std::time::Instant::now();
        TcpStream::connect_timeout(&addr, std::time::Duration::from_secs(4)).map_err(|e| e.to_string())?;
        Ok(start.elapsed().as_millis() as u32)
    })
    .await
}

/// A game file found with Locate, for one action.
#[tauri::command]
fn set_game_file(shared: State<'_, Shared>, game: String, action: String, file: String) -> Result<(), String> {
    edit_settings(&shared, |root| {
        let e = game_entry(root, &game)?;
        let swf = e.entry("swf").or_insert_with(|| serde_json::json!({}));
        swf.as_object_mut().ok_or("bad swf entry")?.insert(action, serde_json::Value::String(file));
        Ok(())
    })
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LobbyEvent {
    game: String,
    message: serde_json::Value,
}

/// Opens a lobby room: runs `--lobby-session <game>`, hands it the host or join request, and passes each line it
/// prints to the window as a `lobby` event (snapshots, the game starting and exiting, the room closing).
#[tauri::command]
fn lobby_start(app: AppHandle, shared: State<'_, Shared>, game: String, request: serde_json::Value, tv: Option<bool>) -> Result<(), String> {
    if shared.lobby.lock().unwrap().is_some() {
        return Err("A room is already open".into());
    }
    let mut args = vec!["--lobby-session".to_string(), game.clone()];
    if tv == Some(true) {
        args.push("--tv".into());
    }
    let mut child = engine::spawn(&args, true)?;
    let mut input = child.stdin.take().ok_or("No input to the lobby")?;
    writeln!(input, "{request}").map_err(|e| e.to_string())?;
    let stdout = child.stdout.take();
    *shared.lobby.lock().unwrap() = Some(input);
    std::thread::spawn(move || {
        if let Some(out) = stdout {
            for line in BufReader::new(out).lines().map_while(Result::ok) {
                if let Ok(message) = serde_json::from_str::<serde_json::Value>(&line) {
                    let _ = app.emit("lobby", LobbyEvent { game: game.clone(), message });
                }
            }
        }
        let _ = child.wait();
        if let Some(s) = app.try_state::<Shared>() {
            *s.lobby.lock().unwrap() = None;
        }
        let _ = app.emit("lobby", LobbyEvent { game, message: serde_json::json!({ "t": "ended" }) });
    });
    Ok(())
}

/// A lobby command: ready, start, chat, kick, leave.
#[tauri::command]
fn lobby_send(shared: State<'_, Shared>, command: serde_json::Value) -> Result<(), String> {
    let mut guard = shared.lobby.lock().unwrap();
    let input = guard.as_mut().ok_or("No room is open")?;
    writeln!(input, "{command}").map_err(|e| e.to_string())?;
    input.flush().map_err(|e| e.to_string())
}

/// mods-info's community mods and the website's games.json with its covers, brought up to date (each at most once an
/// hour, or now with `force`): true when the library should be read again, for a mod or game that was added or a status that changed.
#[tauri::command]
async fn refresh_site(force: Option<bool>) -> Result<bool, String> {
    let force = force.unwrap_or(false);
    blocking(move || launcher::library_json::refresh_online_lists(force)).await
}

/// "How did it run?": the player's answer, sent to the central server with this PC's parts (src/launcher/reports.rs).
#[tauri::command]
async fn send_report(shared: State<'_, Shared>, game: String, verdict: String, version: String, mins: u64, note: String) -> Result<(), String> {
    let dir = shared.data_dir.lock().unwrap().clone().ok_or("The library hasn't been read yet")?;
    blocking(move || launcher::reports::send(&dir, &game, &verdict, &version, mins, &note)).await
}

/// What players reported about a game: counts and the newest reports.
#[tauri::command]
async fn reports(game: String) -> Result<serde_json::Value, String> {
    blocking(move || launcher::reports::game_reports(&game)).await
}

/// Opens a folder or file's folder in Explorer, or a link in the browser.
#[tauri::command]
fn open(target: String) -> Result<(), String> {
    if !(target.starts_with("https://") || target.starts_with("http://") || std::path::Path::new(&target).exists()) {
        return Err(format!("Not a folder or web link: {target}"));
    }
    std::process::Command::new("explorer").arg(&target).spawn().map(|_| ()).map_err(|e| e.to_string())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        // A second start brings the open window forward instead of opening another one.
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.set_focus();
            }
        }))
        // Where the window was, how big, and whether it was maximized (the C++ window keeps the last).
        .plugin(tauri_plugin_window_state::Builder::default().build())
        .manage(Shared::default())
        .manage(app_update::Pending::default())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_clipboard_manager::init())
        .plugin(tauri_plugin_notification::init())
        .setup(|app| {
            if cfg!(debug_assertions) {
                app.handle().plugin(tauri_plugin_log::Builder::default().level(log::LevelFilter::Info).build())?;
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            library, mods, rooms, launch, stop_session, mod_job, download_game, set_favorite, set_setting, set_game_dir,
            set_game_file, lobby_start, lobby_send, open, install_player, set_game_setting, set_game_player,
            check_players, update_players, ping, all_rooms, send_report, reports, refresh_site,
            app_update::check_app_update, app_update::install_app_update
        ])
        .run(tauri::generate_context!())
        .expect("error while building tauri application");
}
