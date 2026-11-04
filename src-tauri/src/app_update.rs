//! The app updating itself (PORTING.md part 8): Tauri's updater reads `latest.json` from the newest GitHub release,
//! checks the installer's minisign signature against the public key in tauri.conf.json (the private key stays on the
//! release PC, outside the repository), and runs the installer, which replaces the app and opens it again.
//!
//! Once per start the window asks `check_app_update`; when a newer version exists it asks the player first, as it
//! does for Ruffle Together, and `install_app_update` downloads with `app-progress` events and then installs. The app
//! exits while the installer runs, so the window offers it only while no game runs or hosts.

use std::sync::Mutex;

use serde::Serialize;
use tauri::{AppHandle, Emitter, State};
use tauri_plugin_updater::{Update, UpdaterExt};

/// The release the last check found, kept for the install that follows.
#[derive(Default)]
pub struct Pending(Mutex<Option<Update>>);

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Available {
    version: String,
    current: String,
}

#[derive(Clone, Serialize)]
struct Progress {
    done: u64,
    total: u64,
}

/// The updater as tauri.conf.json sets it up. Development builds (the lab) may point it at a local feed signed with
/// a test key: `FL_UPDATE_ENDPOINT` and `FL_UPDATE_PUBKEY`. A release build ignores both, so nothing on the PC can
/// redirect a shipped app to another feed or key.
fn updater(app: &AppHandle) -> Result<tauri_plugin_updater::Updater, String> {
    let mut b = app.updater_builder();
    if cfg!(debug_assertions) {
        if let Ok(url) = std::env::var("FL_UPDATE_ENDPOINT") {
            let url = url.parse().map_err(|e| format!("FL_UPDATE_ENDPOINT: {e}"))?;
            b = b.endpoints(vec![url]).map_err(|e| e.to_string())?;
        }
        if let Ok(key) = std::env::var("FL_UPDATE_PUBKEY") {
            b = b.pubkey(key);
        }
    }
    b.build().map_err(|e| e.to_string())
}

/// A newer version of the app, or nothing. Nothing is downloaded yet.
#[tauri::command]
pub async fn check_app_update(app: AppHandle, pending: State<'_, Pending>) -> Result<Option<Available>, String> {
    let found = updater(&app)?.check().await.map_err(|e| e.to_string())?;
    let answer = found.as_ref().map(|u| Available { version: u.version.clone(), current: u.current_version.clone() });
    *pending.0.lock().unwrap() = found;
    Ok(answer)
}

/// Downloads the release the check found, checks its signature, and starts its installer; the app exits then and
/// the installer opens the new version. Returns only on a failure.
#[tauri::command]
pub async fn install_app_update(app: AppHandle, pending: State<'_, Pending>) -> Result<(), String> {
    let update = pending.0.lock().unwrap().take().ok_or("There is no update to install; check again")?;
    let mut done = 0u64;
    let progress = app.clone();
    update
        .download_and_install(
            move |chunk, total| {
                done += chunk as u64;
                let _ = progress.emit("app-progress", Progress { done, total: total.unwrap_or(0) });
            },
            || {},
        )
        .await
        .map_err(|e| format!("The update couldn't be installed: {e}"))
}
