//! launcher.log in the data folder (old/src/core/log.cpp): what started, with which command line, the relay's
//! comings and goings, a game server's output. Settings' "Open log folder" shows it to someone helping a player.
//! Each launch starts the file again, as each C++ `--launch` did. A line is appended and the file closed again, so
//! the C++ launcher (still run for mods and the lobby) can open it too; a line written while it holds the file is
//! lost, as its own were while another copy held it.

use std::io::Write;
use std::sync::Mutex;

#[cfg(windows)]
use windows_sys::Win32::System::SystemInformation::GetLocalTime;

static PATH: Mutex<Option<String>> = Mutex::new(None);

/// Starts the log at `path` (empty, as the C++ launcher opened it with "wb").
pub fn open(path: &str) {
    super::paths::ensure_dir(&super::paths::parent(path));
    let ok = std::fs::File::create(path).is_ok();
    *PATH.lock().unwrap() = ok.then(|| path.to_string());
}

fn line(level: &str, msg: &str) {
    let Some(path) = PATH.lock().unwrap().clone() else { return };
    let text = format!("{} {level} {msg}\r\n", local_time());
    if let Ok(mut f) = std::fs::OpenOptions::new().append(true).open(&path) {
        let _ = f.write_all(text.as_bytes());
    }
}

/// "2026-11-04 13:05:09.042", the local time.
#[cfg(windows)]
fn local_time() -> String {
    let t = unsafe {
        let mut t = std::mem::zeroed();
        GetLocalTime(&mut t);
        t
    };
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds)
}

#[cfg(unix)]
fn local_time() -> String {
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
    let secs = now.as_secs() as libc::time_t;
    let mut t: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&secs, &mut t) };
    format!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
        t.tm_year + 1900,
        t.tm_mon + 1,
        t.tm_mday,
        t.tm_hour,
        t.tm_min,
        t.tm_sec,
        now.subsec_millis()
    )
}

pub fn info(msg: &str) {
    line("INFO ", msg);
}
pub fn warn(msg: &str) {
    line("WARN ", msg);
}
pub fn error(msg: &str) {
    line("ERROR", msg);
}
