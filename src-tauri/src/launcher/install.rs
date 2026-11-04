//! Players and game files (old/src/core/install.cpp, and main.cpp's `--install-player`, `--update-players [--check]`
//! and `--download-game`): a player's newest GitHub release asset or fixed URL, checked against its SHA-256 and
//! unpacked into the player's folder; the players the launcher installed kept on their newest release; the game
//! catalog's files. Every download lands under a temporary name and moves into place only once it checked out, so a
//! failed one leaves the previous copy as it was.

use std::sync::atomic::AtomicBool;

#[cfg(windows)]
use windows_sys::Win32::System::SystemInformation::GetSystemDirectoryW;
#[cfg(windows)]
use windows_sys::Win32::System::Threading::{GetExitCodeProcess, WaitForSingleObject};

use super::catalog::{CatalogFile, PLAYER_VERSION_FILE, parse_catalog};
use super::config::{Config, GameSpec, InstallSpec, PlayerSpec};
use super::http::{self, win32_error_message};
use super::json::{self, Value, serialize};
use super::library::Library;
use super::library_json::{load_launcher_config_issues, open_library};
use super::paths;
#[cfg(windows)]
use super::process::start_process;
use super::text::{compare_versions, format_bytes, glob_match, replace_all, to_lower, trim};
use super::{log, vars::Vars};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    Finding,
    Downloading,
    Verifying,
    Unpacking,
    Done,
    Failed,
}

#[derive(Clone, Copy, Debug)]
pub struct InstallStatus {
    pub stage: Stage,
    pub done: u64, // bytes, while downloading
    pub total: u64,
}

impl Default for InstallStatus {
    fn default() -> Self {
        InstallStatus { stage: Stage::Finding, done: 0, total: 0 }
    }
}

#[derive(Clone, Debug, Default)]
pub struct InstallResult {
    pub ok: bool,
    pub exe: String,     // the installed player
    pub version: String, // release tag, when known
    pub error: String,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct PlayerUpdate {
    pub checked: bool,     // the newest release was looked up
    pub available: bool,   // it is newer than the installed copy
    pub updated: bool,     // a newer release is now installed
    pub version: String,   // the newest release's tag
    pub installed: String, // the installed copy's tag (empty: not recorded)
    pub error: String,
}

pub type Progress<'a> = &'a mut dyn FnMut(&InstallStatus);

fn move_error(what: &str, code: u32) -> String {
    format!("{what}: {}", win32_error_message(code))
}

#[derive(Clone, Debug, Default)]
struct Asset {
    url: String,
    name: String,
    version: String,
    sha256: String,
    size: u64,
}

/// The file to download: the fixed URL, or the asset of the repository's latest release whose name matches. GitHub
/// publishes each asset's SHA-256 as its "digest".
fn find_asset(spec: &InstallSpec, cancel: Option<&AtomicBool>) -> Result<Asset, String> {
    if !spec.url.is_empty() {
        return Ok(Asset {
            url: spec.url.clone(),
            name: paths::file_name(&replace_all(&spec.url, "/", "\\")),
            sha256: to_lower(&spec.sha256),
            ..Default::default()
        });
    }
    let base = if spec.api.is_empty() { "https://api.github.com" } else { spec.api.as_str() };
    let api = format!("{base}/repos/{}/releases/latest", spec.github);
    let mut body = Vec::new();
    http::get_to(
        &api,
        &mut |d, _| {
            body.extend_from_slice(d);
            body.len() < 8 * 1024 * 1024
        },
        cancel,
    )?;
    let rel = json::parse(&body).unwrap_or_default();
    let Some(assets) = rel.find("assets").filter(|a| a.is_array()) else {
        return Err(format!("GitHub returned no release information for {}", spec.github));
    };
    let mut out = Asset::default();
    if let Some(tag) = rel.find("tag_name").and_then(Value::as_str) {
        out.version = tag.to_string();
    }
    for a in assets.items() {
        let (Some(name), Some(url)) = (a.find("name").and_then(Value::as_str), a.find("browser_download_url").and_then(Value::as_str)) else {
            continue;
        };
        if !glob_match(&spec.asset, name) {
            continue;
        }
        out.name = name.to_string();
        out.url = url.to_string();
        if let Some(n) = a.find("size").and_then(Value::as_f64) {
            out.size = n as u64;
        }
        if let Some(d) = a.find("digest").and_then(Value::as_str).and_then(|d| d.strip_prefix("sha256:")) {
            out.sha256 = to_lower(d);
        }
        return Ok(out);
    }
    Err(format!("The latest {} release has no file matching {}", spec.github, spec.asset))
}

/// `exe` in `dir` or one folder down (some archives wrap everything in a top folder): the folder that holds it.
fn folder_with(dir: &str, exe: &str) -> String {
    if paths::is_file(&paths::join(dir, exe)) {
        return dir.to_string();
    }
    let Ok(entries) = std::fs::read_dir(dir) else { return String::new() };
    for e in entries.flatten() {
        let sub = paths::join(dir, &e.file_name().to_string_lossy());
        if paths::is_dir(&sub) && paths::is_file(&paths::join(&sub, exe)) {
            return sub;
        }
    }
    String::new()
}

/// Downloads `url` to `dest`, adding to `st.done` as bytes arrive. Checks the size and SHA-256 when known; on any
/// failure `dest` is deleted.
fn download_verified(
    url: &str,
    dest: &str,
    sha256: &str,
    size: u64,
    st: &mut InstallStatus,
    progress: Progress,
    cancel: Option<&AtomicBool>,
) -> Result<(), String> {
    use sha2::{Digest, Sha256};
    use std::io::Write;
    let Ok(mut f) = std::fs::File::create(paths::long_path(dest)) else { return Err(format!("Cannot write {dest}")) };
    let mut hash = Sha256::new();
    let mut got = 0u64;
    let mut result = http::get_to(
        url,
        &mut |d, length| {
            hash.update(d);
            got += d.len() as u64;
            st.done += d.len() as u64;
            if st.total == 0 && length != 0 {
                st.total = length;
            }
            progress(st);
            f.write_all(d).is_ok()
        },
        cancel,
    );
    drop(f);
    if result.is_ok() && size != 0 && got != size {
        result = Err(format!(
            "The download is incomplete ({} of {}; {got} of {size} bytes)",
            format_bytes(got),
            format_bytes(size)
        ));
    }
    if result.is_ok() && !sha256.is_empty() {
        let hex: String = hash.finalize().iter().map(|b| format!("{b:02x}")).collect();
        if hex != to_lower(sha256) {
            result = Err("The download doesn't match its published checksum, so it was not installed.".into());
        }
    }
    if result.is_err() {
        paths::delete_file(dest);
    }
    result
}

/// Release tags are "v1.2.0"; the shared version compare wants "1.2.0".
fn bare_version(v: &str) -> &str {
    v.strip_prefix(['v', 'V']).unwrap_or(v)
}

#[cfg(windows)]
fn system_dir() -> String {
    let mut buf = [0u16; 260];
    let n = unsafe { GetSystemDirectoryW(buf.as_mut_ptr(), buf.len() as u32) } as usize;
    paths::from_wide(&buf[..n.min(buf.len())])
}

/// Downloads and unpacks `player`'s install entry into its folder. `vars` must hold PLAYERS_DIR and DATA_DIR.
pub fn install_player(player: &PlayerSpec, vars: &Vars, progress: Progress, cancel: Option<&AtomicBool>) -> InstallResult {
    let mut r = InstallResult::default();
    let mut st = InstallStatus::default();
    macro_rules! report {
        ($s:expr) => {{
            st.stage = $s;
            progress(&st);
        }};
    }
    macro_rules! fail {
        ($msg:expr) => {{
            let msg: String = $msg;
            log::error(&format!("Installing {} failed: {msg}", player.name));
            r.error = msg;
            report!(Stage::Failed);
            return r;
        }};
    }
    let spec = &player.install;
    if !spec.enabled() {
        fail!(format!("No download is configured for {}", player.name));
    }
    let target = paths::normalize(&vars.expand(&spec.dir));
    let exe = if spec.exe.is_empty() { "ruffle.exe".to_string() } else { spec.exe.clone() };
    if target.is_empty() || paths::is_dangerous_root(&target) {
        fail!(format!("Unsafe install folder: {target}"));
    }

    report!(Stage::Finding);
    let asset = match find_asset(spec, cancel) {
        Ok(a) => a,
        Err(e) => fail!(e),
    };
    r.version = asset.version.clone();
    log::info(&format!("Installing {} {} from {}", player.name, asset.version, asset.url));

    // Download next to the target so the final move is a rename on the same drive.
    let work = format!("{target}.installing");
    paths::remove_tree(&work);
    if !paths::ensure_dir(&work) {
        fail!(format!("Cannot create {work}"));
    }
    let archive = paths::join(&work, if asset.name.is_empty() { "download.zip" } else { &asset.name });
    st.total = asset.size;
    report!(Stage::Downloading);
    if let Err(e) = download_verified(&asset.url, &archive, &asset.sha256, asset.size, &mut st, progress, cancel) {
        paths::remove_tree(&work);
        fail!(e);
    }

    report!(Stage::Unpacking);
    let unpacked = paths::join(&work, "files");
    paths::ensure_dir(&unpacked);
    #[cfg(windows)]
    let code = {
        // tar.exe ships with Windows 10 1803+ and unpacks .zip archives; it refuses entries that climb out with "..".
        let tar = paths::join(&system_dir(), "tar.exe");
        let args = ["-xf".to_string(), archive.clone(), "-C".to_string(), unpacked.clone()];
        let p = match start_process(&tar, &args, &work, true, &[]) {
            Ok(p) => p,
            Err(e) => {
                paths::remove_tree(&work);
                fail!(format!("Cannot unpack the download: {e}"));
            }
        };
        unsafe { WaitForSingleObject(p.raw(), 120000) };
        let mut code: u32 = 1;
        if unsafe { GetExitCodeProcess(p.raw(), &mut code) } == 0 {
            code = 1;
        }
        drop(p);
        code
    };
    // Linux: a zip is read by the launcher itself, anything else by the system's tar (src/launcher/unzip.rs).
    #[cfg(unix)]
    let code: u32 = match super::unzip::unpack_archive(&archive, &unpacked) {
        Ok(()) => 0,
        Err(e) => {
            log::error(&format!("Unpacking {archive} failed: {e}"));
            1
        }
    };
    let root = if code == 0 { folder_with(&unpacked, &exe) } else { String::new() };
    if root.is_empty() {
        paths::remove_tree(&work);
        // tar still running after two minutes reads STILL_ACTIVE, so it counts as failed too.
        fail!(if code == 0 { format!("The download doesn't contain {exe}") } else { "Unpacking the download failed".to_string() });
    }

    // Swap folders: keep the old install until the new one is in place.
    let old = format!("{target}.old");
    paths::remove_tree(&old);
    if paths::exists(&target)
        && let Err(c) = paths::move_file(&target, &old, false)
    {
        paths::remove_tree(&work);
        fail!(move_error(&format!("Cannot replace {target}"), c));
    }
    paths::ensure_dir(&paths::parent(&target));
    if let Err(c) = paths::move_file(&root, &target, false) {
        let msg = move_error(&format!("Cannot move the player into {target}"), c);
        if paths::exists(&old) {
            let _ = paths::move_file(&old, &target, false);
        }
        paths::remove_tree(&work);
        fail!(msg);
    }
    paths::remove_tree(&old);
    paths::remove_tree(&work);
    r.ok = true;
    r.exe = paths::join(&target, &exe);
    // Remember which release this is, so a newer one can be told apart later.
    if !asset.version.is_empty() {
        paths::write_file(&paths::join(&target, PLAYER_VERSION_FILE), asset.version.as_bytes());
    }
    log::info(&format!("{} installed: {}", player.name, r.exe));
    report!(Stage::Done);
    r
}

/// When the launcher installed `player` from a GitHub release and a newer release exists, installs it over the old
/// one (`install` false: only says whether there is one, so the window can ask first). A player the launcher didn't
/// install (no exe in install.dir) is left alone. Needs the player not to be running.
pub fn update_installed_player(player: &PlayerSpec, vars: &Vars, cancel: Option<&AtomicBool>, install: bool, progress: Progress) -> PlayerUpdate {
    let mut u = PlayerUpdate::default();
    let spec = &player.install;
    if spec.github.is_empty() {
        return u; // only a player the launcher installed from a release is kept current
    }
    let target = paths::normalize(&vars.expand(&spec.dir));
    let exe = if spec.exe.is_empty() { "ruffle.exe" } else { spec.exe.as_str() };
    if target.is_empty() || !paths::is_file(&paths::join(&target, exe)) {
        return u;
    }
    let asset = match find_asset(spec, cancel) {
        Ok(a) => a,
        Err(e) => {
            u.error = e;
            return u;
        }
    };
    u.checked = true;
    u.version = asset.version.clone();
    // A copy installed before the version was recorded has no file: treat it as older.
    let installed = paths::read_file(&paths::join(&target, PLAYER_VERSION_FILE)).unwrap_or_default();
    let have = trim(&String::from_utf8_lossy(&installed)).to_string();
    u.installed = have.clone();
    if !have.is_empty() && compare_versions(bare_version(&asset.version), bare_version(&have)) <= 0 {
        return u;
    }
    u.available = true;
    if !install {
        return u;
    }
    log::info(&format!("{} {} -> {}", player.name, if have.is_empty() { "(unknown version)" } else { &have }, asset.version));
    let r = install_player(player, vars, progress, cancel);
    u.updated = r.ok;
    if !r.ok {
        u.error = r.error;
    }
    u
}

/// Downloads and parses the catalog; the JSON comes back too, for a cache.
pub fn fetch_catalog(url: &str, cancel: Option<&AtomicBool>) -> Result<(Vec<CatalogFile>, Vec<u8>), String> {
    let mut raw = Vec::new();
    http::get_to(
        url,
        &mut |d, _| {
            raw.extend_from_slice(d);
            raw.len() < 4 * 1024 * 1024
        },
        cancel,
    )?;
    let files = parse_catalog(&raw, url)?;
    Ok((files, raw))
}

/// Downloads `files` into `dir`, each checked against its SHA-256 and written under a temporary name first. Files
/// already there with the right checksum are kept.
pub fn download_game_files(files: &[CatalogFile], dir: &str, progress: Progress, cancel: Option<&AtomicBool>) -> InstallResult {
    let mut r = InstallResult::default();
    let mut st = InstallStatus::default();
    let mut todo = Vec::new();
    for f in files {
        let dest = paths::join(dir, &f.name);
        if paths::is_file(&dest) && !f.sha256.is_empty() && paths::sha256_file(&dest) == f.sha256 {
            continue;
        }
        todo.push(f);
        st.total += f.size;
    }
    if !paths::ensure_dir(dir) {
        r.error = format!("Cannot create {dir}");
        return r;
    }
    st.stage = Stage::Downloading;
    progress(&st);
    for f in todo {
        let dest = paths::join(dir, &f.name);
        let part = format!("{dest}.part");
        log::info(&format!("Downloading {} -> {dest}", f.url));
        if let Err(e) = download_verified(&f.url, &part, &f.sha256, f.size, &mut st, progress, cancel) {
            r.error = format!("{}: {e}", f.name);
            log::error(&format!("Download failed: {}", r.error));
            st.stage = Stage::Failed;
            progress(&st);
            return r;
        }
        // Renamed only after the checksum passed, so a half-written file never shows up as a playable game.
        if let Err(c) = paths::move_file(&part, &dest, true) {
            r.error = move_error(&format!("Cannot save {dest}"), c);
            paths::delete_file(&part);
            return r;
        }
    }
    r.ok = true;
    st.stage = Stage::Done;
    progress(&st);
    r
}

// ---- The window's commands, as the C++ launcher's command line answered them --------------------------------------

/// What every C++ command did first: launcher.json, launcher.log restarted in the data folder, the library (whose
/// player lookups are logged at each refresh) and settings.json. An error is the C++ launcher's exit 3.
fn open_command() -> Result<Config, String> {
    let (mut cfg, issues) = load_launcher_config_issues()?;
    // With mods-info's records, so a Flash game from team/ downloads its game files like one launcher.json has.
    super::library_json::add_community(&mut cfg);
    let mut base = Vars::default();
    base.set("LAUNCHER_DIR", &cfg.config_dir);
    log::open(&paths::join(&paths::normalize(&base.expand(&cfg.paths.data)), "launcher.log"));
    log::info(&format!("Config: {}", super::launch::config_source()));
    for i in &issues {
        log::warn(&format!("{}: {}", i.place, i.message));
    }
    Ok(cfg)
}

fn log_players(cfg: &Config, lib: &Library, refreshes: usize) {
    for _ in 0..refreshes {
        for p in lib.players() {
            log::info(&format!("{}: {}", cfg.players[p.index].name, if p.found() { p.exe.as_str() } else { "not found" }));
        }
    }
}

fn library_for<'c>(cfg: &'c Config) -> Library<'c> {
    let (lib, user) = open_library(cfg);
    log_players(cfg, &lib, if user.games_dir.is_empty() { 1 } else { 2 });
    lib
}

/// `--install-player <id>`: "N%" lines while it downloads, then the done text and the exe. Returns the exit code and,
/// when it failed, the message the C++ launcher printed on stderr.
pub fn install_player_command(id: &str, out: &mut dyn FnMut(&str)) -> (i32, String) {
    let cfg = match open_command() {
        Ok(c) => c,
        Err(e) => return (3, e),
    };
    let lib = library_for(&cfg);
    let fail = |code: i32, msg: String| {
        log::error(&msg);
        (code, msg)
    };
    let Some(p) = cfg.find_player(id).filter(|p| p.install.enabled()) else {
        return fail(3, format!("No installable player with id \"{id}\" (it needs an \"install\" entry)"));
    };
    let mut last_pct: i64 = -1;
    let r = install_player(
        p,
        lib.vars(),
        &mut |s| {
            if s.stage != Stage::Downloading || s.total == 0 {
                return;
            }
            let pct = (s.done * 100 / s.total) as i64;
            if pct / 10 != last_pct / 10 {
                out(&format!("{pct}%"));
            }
            last_pct = pct;
        },
        None,
    );
    if !r.ok {
        return fail(1, cfg.t("install.failed", &[("PLAYER", &p.name), ("ERROR", &r.error)]));
    }
    let who = if r.version.is_empty() { p.name.clone() } else { format!("{} {}", p.name, r.version) };
    out(&cfg.t("install.done", &[("PLAYER", &who)]));
    out(&r.exe);
    (0, String::new())
}

/// `--update-players [--check] [--json]`: each player the launcher installed from a release brought to the newest one
/// (or, with `check`, only looked up), because everyone in a co-op session has to run the same build. With `json`,
/// `{"progress":{id,done,total}}` lines (one per whole percent) while a release downloads, then `{"players":[...]}`.
/// Exit 1 when any lookup or install failed.
pub fn update_players_command(check: bool, as_json: bool, out: &mut dyn FnMut(&str)) -> (i32, String) {
    let cfg = match open_command() {
        Ok(c) => c,
        Err(e) => return (3, e),
    };
    let lib = library_for(&cfg);
    let mut list = Vec::new();
    let mut failed = 0;
    for p in &cfg.players {
        let mut last_pct: i64 = -1;
        let u = {
            let mut progress = |s: &InstallStatus| {
                if !as_json || s.stage != Stage::Downloading || s.total == 0 {
                    return;
                }
                let pct = (s.done * 100 / s.total) as i64;
                if pct == last_pct {
                    return;
                }
                last_pct = pct;
                let mut pr = Value::object();
                pr.set("id", p.id.as_str());
                pr.set("done", Value::Number(s.done as f64));
                pr.set("total", Value::Number(s.total as f64));
                let mut line = Value::object();
                line.set("progress", pr);
                out(&serialize(&line, false));
            };
            update_installed_player(p, lib.vars(), None, !check, &mut progress)
        };
        if !u.error.is_empty() {
            failed += 1;
        }
        if as_json {
            let mut o = Value::object();
            o.set("id", p.id.as_str());
            o.set("checked", u.checked);
            o.set("available", u.available);
            o.set("updated", u.updated);
            o.set("installed", u.installed.as_str());
            o.set("version", u.version.as_str());
            o.set("error", u.error.as_str());
            list.push(o);
        } else if u.updated {
            out(&format!("{} updated to {}", p.name, u.version));
        } else if u.available {
            out(&format!("{} {} is available (installed: {})", p.name, u.version, if u.installed.is_empty() { "unknown" } else { &u.installed }));
        } else if u.checked {
            out(&format!("{} is up to date ({})", p.name, u.version));
        } else if !u.error.is_empty() {
            out(&format!("{}: {}", p.name, u.error));
        }
    }
    if as_json {
        let mut root = Value::object();
        root.set("players", list);
        out(&serialize(&root, false));
    }
    (if failed > 0 { 1 } else { 0 }, String::new())
}

/// `--download-game <id | all>`: the game's files from the catalog into the games folder, then their paths. A game a
/// mods-info record pinned its own files for (a developer's Flash game) gets those instead, into their folder.
pub fn download_game_command(id: &str, out: &mut dyn FnMut(&str)) -> (i32, String) {
    let cfg = match open_command() {
        Ok(c) => c,
        Err(e) => return (3, e),
    };
    let lib = library_for(&cfg);
    let fail = |code: i32, msg: String| {
        log::error(&msg);
        (code, msg)
    };
    let games = lib.vars().get("GAMES_DIR");
    let pinned: Vec<&GameSpec> = cfg.games.iter().filter(|g| (id == "all" || g.id == id) && !g.downloads.is_empty()).collect();
    let mut wanted: Vec<(String, CatalogFile)> = Vec::new();
    for g in &pinned {
        for f in &g.downloads {
            let file = CatalogFile { game: g.id.clone(), name: f.name.clone(), url: f.url.clone(), sha256: f.sha256.clone(), description: String::new(), size: f.size };
            wanted.push((paths::join(&games, &f.dir), file));
        }
    }
    if id == "all" || pinned.is_empty() {
        if cfg.downloads.catalog.is_empty() {
            if pinned.is_empty() {
                return fail(3, "launcher.json has no downloads.catalog".into());
            }
        } else {
            let files = match fetch_catalog(&cfg.downloads.catalog, None) {
                Ok((f, _)) => f,
                Err(e) => return fail(1, format!("Cannot read the game catalog: {e}")),
            };
            wanted.extend(files.into_iter().filter(|f| id == "all" || f.game == id).map(|f| (games.clone(), f)));
        }
    }
    if wanted.is_empty() {
        return fail(3, format!("The catalog has no files for \"{id}\""));
    }
    // One folder at a time: the catalog's files go straight into the games folder, a record's into its own.
    let mut dirs: Vec<&String> = wanted.iter().map(|(d, _)| d).collect();
    dirs.dedup();
    for dir in dirs {
        let files: Vec<CatalogFile> = wanted.iter().filter(|(d, _)| d == dir).map(|(_, f)| f.clone()).collect();
        let r = download_game_files(&files, dir, &mut |_| {}, None);
        if !r.ok {
            return fail(1, format!("Download failed: {}", r.error));
        }
    }
    for (dir, f) in &wanted {
        out(&paths::join(dir, &f.name));
    }
    (0, String::new())
}
