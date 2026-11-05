//! mods-info as one download: its zip holds records/ and releases.json from one commit, so a refresh is one request and
//! can't drop a mod because one file failed (docs/COMMUNITY_MODS.md). The zip is read in memory, by exact names, and is
//! never unpacked to disk. The same copies as ever are saved from it: records.json, and mods/releases.json for the C++
//! mod engine. If the zip can't be had, the per-file route (community.rs) is the fallback, at most hourly, so a host
//! that refuses doesn't turn into a burst of requests to another.

use std::io::{Cursor, Read};

use super::community::{self, Cache, MAX_AGE_SECONDS, load_cache, parse_index, save_cache};
use super::http;
use super::json::{self, Value};
use super::log;
use super::paths;

const MAX_ZIP_BYTES: usize = 5 * 1024 * 1024;
const MAX_ENTRY_BYTES: u64 = 1024 * 1024;
const MAX_UNPACKED_BYTES: u64 = 32 * 1024 * 1024;
const MAX_ENTRIES: usize = 5000;
/// How old the saved copies may be before the zip is asked for again.
pub const ZIP_MAX_AGE_SECONDS: i64 = 15 * 60;
const FIRST_WAIT_SECONDS: i64 = 15 * 60;
const LONGEST_WAIT_SECONDS: i64 = 60 * 60;
const PER_FILE_EVERY_SECONDS: i64 = 60 * 60;

#[derive(Debug, PartialEq)]
pub struct Snapshot {
    pub records: Vec<String>,
    pub releases: Option<String>,
}

/// The number of entries the zip's end record states. The reader keeps one entry per name, so a stated number it didn't
/// keep means a name came twice, and which of the two is meant is anyone's guess.
fn stated_entries(bytes: &[u8]) -> Option<usize> {
    let from = bytes.len().saturating_sub(22 + 65535);
    let at = (from..=bytes.len().checked_sub(22)?).rev().find(|&i| bytes[i..i + 4] == [0x50, 0x4b, 0x05, 0x06])?;
    Some(u16::from_le_bytes([bytes[at + 10], bytes[at + 11]]) as usize)
}

/// The mods in a mods-info zip, or why the whole zip is refused. Nothing is half read: a damaged entry, a second top-level
/// folder or anything over the limits refuses all of it, and the caller keeps its old copies.
pub fn read_snapshot(bytes: &[u8]) -> Result<Snapshot, String> {
    let mut zip = zip::ZipArchive::new(Cursor::new(bytes)).map_err(|e| format!("mods-info's zip can't be read: {e}"))?;
    if zip.len() > MAX_ENTRIES {
        return Err("mods-info's zip has too many files".into());
    }
    if stated_entries(bytes).is_some_and(|n| n != zip.len() && n != 0xFFFF) {
        return Err("mods-info's zip has a name twice".into());
    }
    let (mut top, mut unpacked) = (None::<String>, 0u64);
    for i in 0..zip.len() {
        let e = zip.by_index_raw(i).map_err(|e| format!("mods-info's zip can't be read: {e}"))?;
        unpacked = unpacked.saturating_add(e.size());
        let name = e.name().map_err(|e| format!("mods-info's zip can't be read: {e}"))?;
        let first = name.split('/').next().unwrap_or("").to_string();
        match &top {
            None if !first.is_empty() => top = Some(first),
            Some(t) if *t == first => {}
            _ => return Err("mods-info's zip doesn't have one top-level folder".into()),
        }
    }
    if unpacked > MAX_UNPACKED_BYTES {
        return Err("mods-info's zip unpacks to too much".into());
    }
    let top = top.ok_or("mods-info's zip is empty")?;
    let mut text = |name: String| -> Result<Option<String>, String> {
        let mut e = match zip.by_name(&name) {
            Ok(e) => e,
            Err(zip::result::ZipError::FileNotFound) => return Ok(None),
            Err(e) => return Err(format!("{name}: {e}")),
        };
        if e.size() > MAX_ENTRY_BYTES {
            return Err(format!("{name} is too big"));
        }
        // Read to the end, past the stated size if it lied: that is what checks the entry's CRC.
        let mut out = Vec::new();
        (&mut e).take(MAX_ENTRY_BYTES + 1).read_to_end(&mut out).map_err(|e| format!("{name}: {e}"))?;
        if out.len() as u64 > MAX_ENTRY_BYTES {
            return Err(format!("{name} is too big"));
        }
        Ok(Some(String::from_utf8_lossy(&out).to_string()))
    };
    let mut records = Vec::new();
    if let Some(index) = text(format!("{top}/records/index.json"))? {
        let ids = parse_index(index.as_bytes()).ok_or("mods-info's records/index.json isn't a list of mods")?;
        for id in ids {
            // parse_index only lets through lowercase letters, digits and '-', so the name stays inside records/.
            if let Some(record) = text(format!("{top}/records/{id}.json"))? {
                records.push(record);
            }
        }
    }
    let releases = text(format!("{top}/releases.json"))?;
    Ok(Snapshot { records, releases })
}

/// When to ask again after a failure, kept in the data folder so a restart doesn't start over.
#[derive(Debug, Default, PartialEq)]
struct Waiting {
    failures: i64,
    next_try: i64,
    last_per_file: i64,
}

fn number(o: &Value, key: &str) -> i64 {
    o.find(key).and_then(Value::as_f64).unwrap_or(0.0) as i64
}

fn load_waiting(path: &str) -> Waiting {
    let Some(root) = paths::read_file(path).and_then(|t| json::parse(&t).ok()) else { return Waiting::default() };
    Waiting { failures: number(&root, "failures"), next_try: number(&root, "nextTry"), last_per_file: number(&root, "lastPerFile") }
}

fn save_text(path: &str, text: &str) -> bool {
    paths::ensure_dir(&paths::parent(path));
    let part = format!("{path}.part");
    paths::write_file(&part, text.as_bytes()) && paths::move_file(&part, path, true).is_ok()
}

fn save_waiting(path: &str, w: &Waiting) {
    let text = serde_json::json!({ "failures": w.failures, "nextTry": w.next_try, "lastPerFile": w.last_per_file }).to_string();
    save_text(path, &format!("{text}\n"));
}

/// How long to leave a host alone after `failures` failures in a row: 15 minutes, doubling up to an hour, or longer if
/// it said so.
fn wait_seconds(failures: i64, retry_after: i64) -> i64 {
    let doubled = FIRST_WAIT_SECONDS.saturating_mul(1i64 << failures.clamp(1, 16).saturating_sub(1));
    doubled.min(LONGEST_WAIT_SECONDS).max(retry_after)
}

fn save_releases(path: &str, releases: Option<&str>) -> bool {
    let before = paths::read_file(path).map(|t| String::from_utf8_lossy(&t).to_string());
    if before.as_deref() == releases {
        return false;
    }
    match releases {
        Some(text) => save_text(path, text),
        // Not in the zip: the engine asks GitHub, rather than trust an old copy.
        None => std::fs::remove_file(path).is_ok(),
    }
}

/// Brings the data folder's records.json and mods/releases.json up to date. `zip_url` empty: the per-file route alone, as
/// a config without the option has. Ok(true) when something changed. `force` (Ctrl+F5) asks whatever the age and the wait.
pub fn refresh(zip_url: &str, base: &str, data_dir: &str, force: bool) -> Result<bool, String> {
    let records_path = paths::join(data_dir, "records.json");
    if zip_url.is_empty() {
        return community::refresh_records_within(base, &records_path, if force { 0 } else { MAX_AGE_SECONDS });
    }
    let releases_path = paths::join(&paths::join(data_dir, "mods"), "releases.json");
    let waiting_path = paths::join(data_dir, "mods-info-wait.json");
    let now = community::now_unix();
    let saved = load_cache(&records_path);
    if !force && saved.as_ref().is_some_and(|c| now >= c.fetched_at && now - c.fetched_at < ZIP_MAX_AGE_SECONDS) {
        return Ok(false);
    }
    let mut waiting = load_waiting(&waiting_path);
    // A date a day or more ahead is a clock that was wrong when it was written, not a wait.
    if !force && now < waiting.next_try && now + LONGEST_WAIT_SECONDS * 24 > waiting.next_try {
        return Ok(false);
    }
    let answer = http::get_capped(zip_url, MAX_ZIP_BYTES).and_then(|bytes| read_snapshot(&bytes).map_err(|e| (e, 0)));
    let (error, retry_after) = match answer {
        Ok(snapshot) => {
            let changed = saved.as_ref().is_none_or(|c| c.raw != snapshot.records);
            save_cache(&records_path, &Cache { fetched_at: now, raw: snapshot.records });
            let releases_changed = save_releases(&releases_path, snapshot.releases.as_deref());
            let _ = std::fs::remove_file(&waiting_path);
            return Ok(changed || releases_changed);
        }
        Err(e) => e,
    };
    log::warn(&format!("mods-info's zip: {error}"));
    waiting.failures += 1;
    waiting.next_try = now + wait_seconds(waiting.failures, retry_after);
    // The per-file route, for a network that blocks one host and not the other. Never more than hourly: this is many
    // requests, and a zip that fails must not turn into them every time the window gets the focus.
    let mut result = Err(error);
    if saved.is_none() || now - waiting.last_per_file >= PER_FILE_EVERY_SECONDS {
        waiting.last_per_file = now;
        result = community::refresh_records_within(base, &records_path, MAX_AGE_SECONDS);
        if result.is_ok()
            && let Ok(text) = http::get_text(&format!("{base}releases.json"))
        {
            let text = String::from_utf8_lossy(&text).to_string();
            if save_releases(&releases_path, Some(&text)) {
                result = Ok(true);
            }
        }
    }
    save_waiting(&waiting_path, &waiting);
    result
}
