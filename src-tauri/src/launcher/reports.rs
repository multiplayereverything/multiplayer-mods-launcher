//! "How did it run?": a player's report on a game, and what others reported. New in the app (the C++ launcher had
//! none). The reports Worker keeps them (reports-api/, at launcher.json's `reports.url`):
//!   POST <url>/v1/reports {game, install, verdict, version, mins, note, os, gpu, cpu, ram_gb} -> {ok} | {error}
//!   GET  <url>/v1/reports?game=<id> -> {total, counts, versions, recent}
//! A report carries the verdict, the version played, minutes, an optional note, and the PC's Windows version, graphics
//! card, processor and memory, so others know what to expect on a PC like theirs. No name or account: a random
//! install id (install.id in the data folder) lets the Worker keep one report per PC, game and version.

use std::path::Path;

use serde_json::{Value, json};

use super::http::json_request;
use super::library_json::load_launcher_config;

pub const VERDICTS: [&str; 3] = ["runs", "issues", "broken"];

/// This install's id: 32 hex characters, made once and kept in the data folder.
pub fn install_id(data_dir: &Path) -> String {
    let path = data_dir.join("install.id");
    if let Ok(id) = std::fs::read_to_string(&path) {
        let id = id.trim();
        if id.len() == 32 && id.chars().all(|c| c.is_ascii_hexdigit()) {
            return id.to_string();
        }
    }
    let id = random_hex();
    let _ = std::fs::create_dir_all(data_dir);
    let _ = std::fs::write(&path, &id);
    id
}

/// 128 random bits from the OS: each RandomState is seeded by it, and the clock tells two in a row apart.
fn random_hex() -> String {
    use std::hash::{BuildHasher, Hasher};
    let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0);
    let half = |salt: u64| {
        let mut h = std::collections::hash_map::RandomState::new().build_hasher();
        h.write_u128(nanos);
        h.write_u64(salt);
        h.write_u32(std::process::id());
        h.finish()
    };
    format!("{:016x}{:016x}", half(1), half(2))
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Pc {
    pub os: String,
    pub gpu: String,
    pub cpu: String,
    pub ram_gb: u64,
}

#[cfg(windows)]
fn reg_string(key: &str, value: &str) -> Option<String> {
    use windows_sys::Win32::System::Registry::{HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ, RegGetValueW};
    let w = |s: &str| s.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();
    let (k, v) = (w(key), w(value));
    let mut buf = vec![0u16; 512];
    let mut size = (buf.len() * 2) as u32;
    let r = unsafe { RegGetValueW(HKEY_LOCAL_MACHINE, k.as_ptr(), v.as_ptr(), RRF_RT_REG_SZ, std::ptr::null_mut(), buf.as_mut_ptr().cast(), &mut size) };
    if r != 0 {
        return None;
    }
    let n = (size as usize / 2).saturating_sub(1);
    Some(String::from_utf16_lossy(&buf[..n.min(buf.len())]).trim().to_string()).filter(|s| !s.is_empty())
}

/// "Windows 11 26200": the registry's ProductName still says Windows 10 on 11, so the build number decides.
#[cfg(windows)]
fn windows_version() -> String {
    const KEY: &str = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
    let build = reg_string(KEY, "CurrentBuildNumber").unwrap_or_default();
    let n: u32 = build.parse().unwrap_or(0);
    let name = if n >= 22000 { "Windows 11" } else if n > 0 { "Windows 10" } else { "Windows" };
    if build.is_empty() { name.to_string() } else { format!("{name} {build}") }
}

/// The display adapters' names, the separate card first: a laptop lists its built-in graphics too.
#[cfg(windows)]
fn graphics_card() -> String {
    const CLASS: &str = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}";
    let names: Vec<String> = (0..10)
        .filter_map(|i| reg_string(&format!(r"{CLASS}\{i:04}"), "DriverDesc"))
        .filter(|n| !n.contains("Microsoft Basic") && !n.contains("Remote Display") && !n.contains("Virtual"))
        .collect();
    pick_gpu(&names)
}

pub fn pick_gpu(names: &[String]) -> String {
    let separate = |n: &String| ["NVIDIA", "GeForce", "Radeon RX", "Radeon Pro", "Arc A", "Arc B"].iter().any(|k| n.contains(k));
    names.iter().find(|n| separate(n)).or(names.first()).cloned().unwrap_or_default()
}

#[cfg(windows)]
fn memory_gb() -> u64 {
    use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
    let mut m: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
    m.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
    if unsafe { GlobalMemoryStatusEx(&mut m) } == 0 {
        return 0;
    }
    // Some memory is always kept by the hardware: 15.9 GB is a 16 GB PC.
    (m.ullTotalPhys as f64 / (1u64 << 30) as f64).round() as u64
}

#[cfg(windows)]
pub fn this_pc() -> Pc {
    let cpu = reg_string(r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString").unwrap_or_default();
    Pc { os: windows_version(), gpu: graphics_card(), cpu: cpu.split_whitespace().collect::<Vec<_>>().join(" "), ram_gb: memory_gb() }
}

/// Linux: "Ubuntu 24.04.3 LTS" from os-release, the graphics cards' names from their PCI ids, /proc for the rest.
#[cfg(unix)]
pub fn this_pc() -> Pc {
    let read = |p: &str| std::fs::read_to_string(p).unwrap_or_default();
    let os_release = [read("/etc/os-release"), read("/usr/lib/os-release")].concat();
    let os = os_release
        .lines()
        .find_map(|l| l.strip_prefix("PRETTY_NAME="))
        .map(|v| v.trim_matches('"').to_string())
        .unwrap_or_else(|| "Linux".into());
    let cpu = read("/proc/cpuinfo")
        .lines()
        .find_map(|l| l.strip_prefix("model name").and_then(|r| r.split_once(':')).map(|(_, v)| v.split_whitespace().collect::<Vec<_>>().join(" ")))
        .unwrap_or_default();
    let kb: u64 = read("/proc/meminfo")
        .lines()
        .find_map(|l| l.strip_prefix("MemTotal:"))
        .and_then(|v| v.split_whitespace().next()?.parse().ok())
        .unwrap_or(0);
    let ram_gb = (kb as f64 / (1u64 << 20) as f64).round() as u64;
    Pc { os, gpu: pick_gpu(&linux_gpus()), cpu, ram_gb }
}

/// Every display card in /sys, named from the system's pci.ids: "NVIDIA GeForce RTX 3070", "AMD Radeon RX 6700 XT".
#[cfg(unix)]
fn linux_gpus() -> Vec<String> {
    let ids = ["/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids"]
        .iter()
        .find_map(|p| std::fs::read_to_string(p).ok())
        .unwrap_or_default();
    let mut out = Vec::new();
    let Ok(cards) = std::fs::read_dir("/sys/bus/pci/devices") else { return out };
    for c in cards.flatten() {
        let read = |f: &str| std::fs::read_to_string(c.path().join(f)).unwrap_or_default().trim().trim_start_matches("0x").to_lowercase();
        if !read("class").starts_with("03") {
            continue; // not a display controller
        }
        let (vendor, device) = (read("vendor"), read("device"));
        let name = pci_name(&ids, &vendor, &device);
        // As on Windows: a virtual machine's or remote session's adapter isn't the player's card.
        if !name.contains("Basic Render") && !name.contains("Basic Display") && !name.contains("Virtual") {
            out.push(name);
        }
    }
    out
}

/// A vendor's short name and a device's marketing name (the part in brackets) from pci.ids.
#[cfg(unix)]
pub fn pci_name(ids: &str, vendor: &str, device: &str) -> String {
    let short = match vendor {
        "10de" => "NVIDIA",
        "1002" => "AMD",
        "8086" => "Intel",
        _ => "",
    };
    let mut lines = ids.lines().skip_while(|l| !l.starts_with(&format!("{vendor}  ")));
    let vendor_name = lines.next().map(|l| l[vendor.len()..].trim().to_string()).unwrap_or_default();
    let device_name = lines
        .take_while(|l| l.starts_with('\t') || l.starts_with('#') || l.is_empty())
        .find_map(|l| l.strip_prefix('\t').and_then(|r| r.strip_prefix(&format!("{device}  "))))
        .map(|n| {
            let n = n.trim();
            match (n.find('['), n.rfind(']')) {
                (Some(a), Some(b)) if b > a => n[a + 1..b].to_string(),
                _ => n.to_string(),
            }
        })
        .unwrap_or_else(|| format!("graphics ({vendor}:{device})"));
    let vendor_part = if short.is_empty() { vendor_name } else { short.to_string() };
    if vendor_part.is_empty() || device_name.starts_with(&vendor_part) { device_name } else { format!("{vendor_part} {device_name}") }
}

/// The report the Worker reads.
pub fn report_body(game: &str, install: &str, verdict: &str, version: &str, mins: u64, note: &str, pc: &Pc) -> String {
    json!({
        "game": game, "install": install, "verdict": verdict, "version": version, "mins": mins, "note": note.trim(),
        "os": pc.os, "gpu": pc.gpu, "cpu": pc.cpu, "ram_gb": pc.ram_gb,
    })
    .to_string()
}

/// The Worker's own reason when it refuses ({"error": ...}), else what the answer was.
fn refusal(status: u32, body: &[u8]) -> String {
    serde_json::from_slice::<Value>(body)
        .ok()
        .and_then(|v| v.get("error").and_then(|e| e.as_str()).map(str::to_string))
        .unwrap_or_else(|| format!("The reports service answered HTTP {status}"))
}

fn endpoint(base: &str) -> String {
    format!("{}/v1/reports", base.trim_end_matches('/'))
}

pub fn send_to(base: &str, body: &str) -> Result<(), String> {
    let (status, reply) = json_request(&endpoint(base), Some(body))?;
    if status == 200 { Ok(()) } else { Err(refusal(status, &reply)) }
}

pub fn reports_from(base: &str, game: &str) -> Result<Value, String> {
    // Game ids are [a-z0-9_-], so they need no escaping in the query.
    let (status, reply) = json_request(&format!("{}?game={game}", endpoint(base)), None)?;
    if status != 200 {
        return Err(refusal(status, &reply));
    }
    serde_json::from_slice(&reply).map_err(|_| "The reports couldn't be read".to_string())
}

/// launcher.json's reports.url; an error when reports aren't set up, so the window shows none.
fn base_url() -> Result<String, String> {
    let url = load_launcher_config()?.reports_url;
    if url.is_empty() { Err("Reports aren't set up in launcher.json".into()) } else { Ok(url) }
}

/// The window's "How did it run?" answer.
pub fn send(data_dir: &Path, game: &str, verdict: &str, version: &str, mins: u64, note: &str) -> Result<(), String> {
    if !VERDICTS.contains(&verdict) {
        return Err(format!("Unknown verdict {verdict}"));
    }
    let base = base_url()?;
    send_to(&base, &report_body(game, &install_id(data_dir), verdict, version, mins, note, &this_pc()))
}

/// What players say about a game, for its page.
pub fn game_reports(game: &str) -> Result<Value, String> {
    reports_from(&base_url()?, game)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;

    #[test]
    fn install_id_is_made_once_and_kept() {
        let dir = std::env::temp_dir().join(format!("fl-reports-{}", random_hex()));
        let a = install_id(&dir);
        assert!(a.len() == 32 && a.chars().all(|c| c.is_ascii_hexdigit()));
        assert_eq!(install_id(&dir), a);
        // A damaged file is replaced by a new id.
        std::fs::write(dir.join("install.id"), "nope").unwrap();
        let b = install_id(&dir);
        assert!(b != a && b.len() == 32);
        assert_ne!(random_hex(), random_hex());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_separate_graphics_card_is_named() {
        let s = |v: &[&str]| v.iter().map(|x| x.to_string()).collect::<Vec<_>>();
        assert_eq!(pick_gpu(&s(&["Intel(R) UHD Graphics 630", "NVIDIA GeForce RTX 3060 Laptop GPU"])), "NVIDIA GeForce RTX 3060 Laptop GPU");
        assert_eq!(pick_gpu(&s(&["AMD Radeon(TM) Graphics", "AMD Radeon RX 6700 XT"])), "AMD Radeon RX 6700 XT");
        assert_eq!(pick_gpu(&s(&["Intel(R) Iris(R) Xe Graphics"])), "Intel(R) Iris(R) Xe Graphics");
        assert_eq!(pick_gpu(&[]), "");
    }

    #[test]
    fn this_pc_is_described() {
        let pc = this_pc();
        assert!(if cfg!(windows) { pc.os.starts_with("Windows") } else { !pc.os.is_empty() }, "{pc:?}");
        assert!(pc.ram_gb > 0, "{pc:?}");
    }

    #[test]
    fn report_body_is_json() {
        let pc = Pc { os: "Windows 11 26200".into(), gpu: "RTX".into(), cpu: "Ryzen".into(), ram_gb: 16 };
        let v: Value = serde_json::from_str(&report_body("gm2", "ab", "runs", "1.0.5", 14, "  line one\nline two ", &pc)).unwrap();
        assert_eq!(v["game"], "gm2");
        assert_eq!(v["note"], "line one\nline two");
        assert_eq!(v["ram_gb"], 16);
    }

    /// One HTTP exchange on loopback: the request it got (head and body), and the status and body it answers.
    fn serve_once(status: u32, body: &'static str) -> (String, std::thread::JoinHandle<String>) {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = format!("http://127.0.0.1:{}", l.local_addr().unwrap().port());
        let t = std::thread::spawn(move || {
            let (mut c, _) = l.accept().unwrap();
            let mut got = Vec::new();
            let mut buf = [0u8; 4096];
            loop {
                let n = c.read(&mut buf).unwrap();
                got.extend_from_slice(&buf[..n]);
                let text = String::from_utf8_lossy(&got).to_string();
                if let Some(end) = text.find("\r\n\r\n") {
                    let length = text.lines().find_map(|l| l.to_ascii_lowercase().strip_prefix("content-length:").map(|v| v.trim().parse::<usize>().unwrap_or(0))).unwrap_or(0);
                    if got.len() >= end + 4 + length {
                        break;
                    }
                }
            }
            let reply = format!("HTTP/1.1 {status} X\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len());
            c.write_all(reply.as_bytes()).unwrap();
            String::from_utf8_lossy(&got).to_string()
        });
        (addr, t)
    }

    #[test]
    fn a_report_is_posted() {
        let (addr, t) = serve_once(200, r#"{"ok":true}"#);
        send_to(&addr, r#"{"game":"gm2","verdict":"runs"}"#).unwrap();
        let req = t.join().unwrap();
        assert!(req.starts_with("POST /v1/reports HTTP/1.1"), "{req}");
        assert!(req.to_ascii_lowercase().contains("content-type: application/json"), "{req}");
        assert!(req.ends_with(r#"{"game":"gm2","verdict":"runs"}"#), "{req}");
        // The Worker's reason reaches the player; an answer without one says what came back.
        let (addr, t) = serve_once(429, r#"{"error":"Too many reports from here; try again later"}"#);
        assert_eq!(send_to(&format!("{addr}/"), "{}").unwrap_err(), "Too many reports from here; try again later");
        t.join().unwrap();
        let (addr, t) = serve_once(502, "<html>bad gateway</html>");
        assert_eq!(send_to(&addr, "{}").unwrap_err(), "The reports service answered HTTP 502");
        t.join().unwrap();
    }

    #[test]
    fn reports_are_read() {
        let (addr, t) = serve_once(200, r#"{"total":2,"counts":{"runs":1,"issues":0,"broken":1},"versions":{},"recent":[]}"#);
        assert_eq!(reports_from(&addr, "gunmayhem2").unwrap()["counts"]["broken"], 1);
        assert!(t.join().unwrap().starts_with("GET /v1/reports?game=gunmayhem2 HTTP/1.1"));
        let (addr, t) = serve_once(400, r#"{"error":"Bad game id"}"#);
        assert_eq!(reports_from(&addr, "x").unwrap_err(), "Bad game id");
        t.join().unwrap();
        let (addr, t) = serve_once(200, "{broken");
        assert_eq!(reports_from(&addr, "x").unwrap_err(), "The reports couldn't be read");
        t.join().unwrap();
    }

    /// Against the real Worker running locally (reports-api: npm run dev): FL_TEST_REPORTS_URL=http://localhost:8790.
    #[test]
    fn the_local_worker_keeps_the_report() {
        let Ok(base) = std::env::var("FL_TEST_REPORTS_URL") else { return };
        let pc = this_pc();
        let game = format!("lab-{}", &random_hex()[..8]);
        send_to(&base, &report_body(&game, &random_hex(), "issues", "1.0.5", 12, "Lag spikes with 4 players.", &pc)).unwrap();
        let v = reports_from(&base, &game).unwrap();
        assert_eq!(v["counts"]["issues"], 1, "{v}");
        assert_eq!(v["recent"][0]["note"], "Lag spikes with 4 players.");
        assert_eq!(v["recent"][0]["gpu"], pc.gpu.as_str());
        println!("the Worker answered: {v}");
    }
}
