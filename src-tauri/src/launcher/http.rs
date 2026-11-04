//! HTTP GET the way the C++ launcher does it (old/src/core/install.cpp `httpGet`): through WinHTTP, so the system's
//! proxy settings, TLS and redirects behave as they did, and a failure reads the same ("<host> answered HTTP 404",
//! "Downloading from <host>: <Windows' message> (code N)"). Text answers (`get`) and downloads streamed to disk
//! (`get_to`, with the Content-Length for progress and a cancel flag).

use std::sync::atomic::{AtomicBool, Ordering};

#[cfg(windows)]
use windows_sys::Win32::Foundation::GetLastError;
#[cfg(windows)]
use windows_sys::Win32::Networking::WinHttp::*;
#[cfg(windows)]
use windows_sys::Win32::System::Diagnostics::Debug::{FORMAT_MESSAGE_FROM_SYSTEM, FORMAT_MESSAGE_IGNORE_INSERTS, FormatMessageW};

#[cfg(windows)]
use super::paths::{from_wide, wide};
#[cfg(windows)]
use super::text::trim;

#[cfg(windows)]
/// Windows' text for an error code, as the C++ launcher's `win32ErrorMessage` words it.
pub fn win32_error_message(code: u32) -> String {
    let mut buf = [0u16; 2048];
    let n = unsafe {
        FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            std::ptr::null(),
            code,
            0,
            buf.as_mut_ptr(),
            buf.len() as u32,
            std::ptr::null(),
        )
    } as usize;
    let msg = if n > 0 { trim(&from_wide(&buf[..n.min(buf.len())])).to_string() } else { "Unknown error".to_string() };
    format!("{msg} (code {code})")
}

#[cfg(windows)]
fn last_error(what: &str) -> String {
    format!("{what}: {}", win32_error_message(unsafe { GetLastError() }))
}

/// GitHub's wording when its 60 unsigned requests an hour are used up.
pub fn rate_limit_message(seconds_left: i64) -> String {
    let minutes = (seconds_left.max(1) + 59) / 60;
    let wait = if minutes <= 1 {
        "about a minute".to_string()
    } else if minutes < 60 {
        format!("{minutes} minutes")
    } else {
        format!("{} h {} min", minutes / 60, minutes % 60)
    };
    format!("GitHub only allows 60 requests an hour and that limit is used up. It answers again in {wait}.")
}

/// The window tells a used-up rate limit apart from other failures by this wording.
pub fn is_rate_limit_message(text: &str) -> bool {
    text.starts_with("GitHub only allows 60 requests")
}

#[cfg(windows)]
/// A WinHTTP text buffer up to its NUL.
fn until_nul(b: &[u16]) -> String {
    from_wide(&b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())])
}

#[cfg(windows)]
struct Handle(*mut core::ffi::c_void);

#[cfg(windows)]
impl Drop for Handle {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { WinHttpCloseHandle(self.0) };
        }
    }
}

/// The body of `url`, at most `limit` bytes (more is an error, as the C++ sink refusing it was).
pub fn get(url: &str, limit: usize) -> Result<Vec<u8>, String> {
    let mut body = Vec::new();
    get_to(
        url,
        &mut |d, _| {
            body.extend_from_slice(d);
            body.len() < limit
        },
        None,
    )?;
    Ok(body)
}

#[cfg(windows)]
/// A request sent and its answer's headers read: the open handles (the request first, so it closes first) and the host.
struct Open {
    req: Handle,
    _conn: Handle,
    _session: Handle,
    host: String,
}

#[cfg(windows)]
/// Sends `verb` to `url` with `headers` and `body`, through the system's proxy, and reads the status code.
fn open(url: &str, verb: &str, headers: &str, body: &[u8]) -> Result<(Open, u32), String> {
    let mut host = [0u16; 256];
    let mut path = [0u16; 4096];
    let mut extra = [0u16; 2048];
    let mut uc = URL_COMPONENTS {
        dwStructSize: std::mem::size_of::<URL_COMPONENTS>() as u32,
        lpszHostName: host.as_mut_ptr(),
        dwHostNameLength: host.len() as u32,
        lpszUrlPath: path.as_mut_ptr(),
        dwUrlPathLength: path.len() as u32,
        lpszExtraInfo: extra.as_mut_ptr(),
        dwExtraInfoLength: extra.len() as u32,
        ..Default::default()
    };
    let wurl = wide(url);
    if unsafe { WinHttpCrackUrl(wurl.as_ptr(), 0, 0, &mut uc) } == 0 {
        return Err(format!("Bad download address: {url}"));
    }
    let host_s = until_nul(&host);
    let agent = wide("MultiplayerEverything-Launcher/1.0");
    let session = Handle(unsafe {
        WinHttpOpen(agent.as_ptr(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, std::ptr::null(), std::ptr::null(), 0)
    });
    if session.0.is_null() {
        return Err(last_error("WinHttpOpen"));
    }
    let whost = wide(&host_s);
    let conn = Handle(unsafe { WinHttpConnect(session.0, whost.as_ptr(), uc.nPort, 0) });
    let object = wide(&(until_nul(&path) + &until_nul(&extra)));
    let flags = if uc.nScheme == WINHTTP_INTERNET_SCHEME_HTTPS { WINHTTP_FLAG_SECURE } else { 0 };
    let verb = wide(verb);
    let req = Handle(if conn.0.is_null() {
        std::ptr::null_mut()
    } else {
        unsafe { WinHttpOpenRequest(conn.0, verb.as_ptr(), object.as_ptr(), std::ptr::null(), std::ptr::null(), std::ptr::null(), flags) }
    });
    if req.0.is_null() {
        return Err(last_error(&format!("Connecting to {host_s}")));
    }
    let headers = wide(headers);
    let len = body.len() as u32;
    if unsafe { WinHttpSendRequest(req.0, headers.as_ptr(), u32::MAX, body.as_ptr().cast(), len, len, 0) } == 0
        || unsafe { WinHttpReceiveResponse(req.0, std::ptr::null_mut()) } == 0
    {
        return Err(last_error(&format!("Downloading from {host_s}")));
    }
    let mut status: u32 = 0;
    let mut size = 4u32;
    unsafe {
        WinHttpQueryHeaders(
            req.0,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            std::ptr::null(),
            (&mut status as *mut u32).cast(),
            &mut size,
            std::ptr::null_mut(),
        )
    };
    Ok((Open { req, _conn: conn, _session: session, host: host_s }, status))
}

#[cfg(windows)]
/// The rest of an answer, at most `limit` bytes.
fn read_all(req: &Handle, limit: usize) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        let mut got = 0u32;
        if unsafe { WinHttpReadData(req.0, buf.as_mut_ptr().cast(), buf.len() as u32, &mut got) } == 0 {
            return Err(last_error("The answer was interrupted"));
        }
        if got == 0 || out.len() >= limit {
            out.truncate(limit);
            return Ok(out);
        }
        out.extend_from_slice(&buf[..got as usize]);
    }
}

#[cfg(windows)]
/// A JSON request to a web service of ours (the reports, src/launcher/reports.rs): the status code and the body, which
/// carries the service's own reason when it refuses. GET when `body` is None.
pub fn json_request(url: &str, body: Option<&str>) -> Result<(u32, Vec<u8>), String> {
    let (verb, data) = match body {
        Some(b) => ("POST", b.as_bytes()),
        None => ("GET", &[][..]),
    };
    let (r, status) = open(url, verb, "Accept: application/json\r\nContent-Type: application/json\r\n", data)?;
    Ok((status, read_all(&r.req, 1024 * 1024)?))
}

#[cfg(windows)]
/// GET `url`, following redirects (GitHub sends downloads to its CDN), handing the body to `sink` as it arrives; a
/// sink that answers false stops it ("Could not write the download to disk", the C++ wording whether the disk or a
/// size limit refused); it is also told the Content-Length header (0 without one). `cancel` is looked at before each
/// read.
pub fn get_to(url: &str, sink: &mut dyn FnMut(&[u8], u64) -> bool, cancel: Option<&AtomicBool>) -> Result<(), String> {
    // GitHub's API answers JSON only to clients that send an Accept header.
    let (r, status) = open(url, "GET", "Accept: application/vnd.github+json, */*\r\n", &[])?;
    let (req, host_s) = (&r.req, &r.host);
    if status != 200 {
        let mut error = format!("{host_s} answered HTTP {status}");
        // GitHub allows 60 requests an hour without signing in: say how long until it answers again.
        if status == 403 || status == 429 {
            let header = |name: &str| {
                let mut v = [0u16; 32];
                let mut vsize = std::mem::size_of_val(&v) as u32;
                let wname = wide(name);
                let ok = unsafe {
                    WinHttpQueryHeaders(req.0, WINHTTP_QUERY_CUSTOM, wname.as_ptr(), v.as_mut_ptr().cast(), &mut vsize, std::ptr::null_mut())
                };
                if ok != 0 { until_nul(&v) } else { String::new() }
            };
            let (remaining, reset, retry) = (header("X-RateLimit-Remaining"), header("X-RateLimit-Reset"), header("Retry-After"));
            let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
            let reset_at = if !reset.is_empty() {
                wcstoi64(&reset)
            } else if !retry.is_empty() {
                now + wcstoi64(&retry)
            } else {
                0
            };
            if reset_at > 0 && (remaining == "0" || !retry.is_empty()) {
                error = rate_limit_message(reset_at - now);
            }
        }
        return Err(error);
    }
    let length = {
        let mut v = [0u16; 32];
        let mut lsize = std::mem::size_of_val(&v) as u32;
        let ok = unsafe {
            WinHttpQueryHeaders(req.0, WINHTTP_QUERY_CONTENT_LENGTH, std::ptr::null(), v.as_mut_ptr().cast(), &mut lsize, std::ptr::null_mut())
        };
        if ok != 0 { wcstoui64(&until_nul(&v)) } else { 0 }
    };
    let mut buf = vec![0u8; 256 * 1024];
    loop {
        if cancel.is_some_and(|c| c.load(Ordering::Relaxed)) {
            return Err("Cancelled".into());
        }
        let mut got = 0u32;
        if unsafe { WinHttpReadData(req.0, buf.as_mut_ptr().cast(), buf.len() as u32, &mut got) } == 0 {
            return Err(last_error("Download interrupted"));
        }
        if got == 0 {
            return Ok(());
        }
        if !sink(&buf[..got as usize], length) {
            return Err("Could not write the download to disk".into());
        }
    }
}

/// `_wcstoui64`: leading white space, digits; a minus sign negates modulo 2^64 as the C runtime does.
fn wcstoui64(s: &str) -> u64 {
    let t = s.trim_start();
    let (neg, digits) = match t.as_bytes().first() {
        Some(b'-') => (true, &t[1..]),
        Some(b'+') => (false, &t[1..]),
        _ => (false, t),
    };
    let mut v: u64 = 0;
    for b in digits.bytes().take_while(|b| b.is_ascii_digit()) {
        match v.checked_mul(10).and_then(|x| x.checked_add((b - b'0') as u64)) {
            Some(x) => v = x,
            None => return u64::MAX,
        }
    }
    if neg { v.wrapping_neg() } else { v }
}

/// `_wcstoi64`: leading white space, a sign, digits.
fn wcstoi64(s: &str) -> i64 {
    let t = s.trim_start();
    let (neg, digits) = match t.as_bytes().first() {
        Some(b'-') => (true, &t[1..]),
        Some(b'+') => (false, &t[1..]),
        _ => (false, t),
    };
    let v = digits.bytes().take_while(|b| b.is_ascii_digit()).fold(0i64, |v, b| v.saturating_mul(10).saturating_add((b - b'0') as i64));
    if neg { -v } else { v }
}

/// What `httpGetText` read: a text file of at most 4 MB.
pub fn get_text(url: &str) -> Result<Vec<u8>, String> {
    get(url, 4 * 1024 * 1024)
}

// ---- Linux (old/src/core/posix/http.cpp and install.cpp's #else httpGet) ----------------------------------------
// The same requests through the system's curl: proxy settings and CA certificates come from the system, as WinHTTP's
// do on Windows, and a failure reads as the C++ launcher words it.

/// Windows' text for the codes the Linux layer reports, as the C++ launcher's `win32ErrorMessage` words it.
#[cfg(unix)]
pub fn win32_error_message(code: u32) -> String {
    format!("{} (code {code})", super::posix::win32_text(code).unwrap_or("Unknown error"))
}

/// One "HTTP/x 200 ..." block: the status and the headers, names lower-cased.
#[cfg(unix)]
struct Head {
    status: u32,
    headers: Vec<(String, String)>,
}

#[cfg(unix)]
impl Head {
    fn parse(block: &str) -> Option<Head> {
        let mut lines = block.split("\r\n");
        let first = lines.next()?;
        if !first.starts_with("HTTP/") {
            return None;
        }
        let status = first.split(' ').nth(1).map_or(0, |s| s.bytes().take_while(u8::is_ascii_digit).fold(0u32, |v, b| v * 10 + (b - b'0') as u32));
        let headers = lines
            .filter_map(|l| l.split_once(':'))
            .map(|(n, v)| (n.to_ascii_lowercase(), v.trim_start_matches([' ', '\t']).to_string()))
            .collect();
        Some(Head { status, headers })
    }

    fn header(&self, name: &str) -> String {
        self.headers.iter().rev().find(|(n, _)| n == name).map(|(_, v)| v.clone()).unwrap_or_default()
    }
}

/// curl's exit codes worth putting in words; the rest show curl's own message.
#[cfg(unix)]
fn curl_problem(code: i32, stderr: &str) -> String {
    let msg = stderr.trim_end_matches(['\r', '\n']);
    let msg = msg.strip_prefix("curl: ").unwrap_or(msg);
    match code {
        6 => "Couldn't find the server (no internet connection?)".into(),
        7 => "Couldn't connect to the server".into(),
        28 => "The server took too long to answer".into(),
        35 | 60 => format!("The secure connection failed: {msg}"),
        127 => "curl isn't installed; the launcher needs it to download (sudo apt install curl)".into(),
        _ if msg.is_empty() => format!("curl failed with code {code}"),
        _ => msg.to_string(),
    }
}

/// The host part of an address, for "Downloading from <host>".
#[cfg(unix)]
fn host_of(url: &str) -> &str {
    let rest = url.find("://").map_or(url, |i| &url[i + 3..]);
    &rest[..rest.find(['/', ':', '?', '#']).unwrap_or(rest.len())]
}

/// Sends `verb` to `url` with `headers` (and `body` on POST) through curl, following redirects. `on_head` gets the
/// final status and headers before any body bytes; answering false stops there (and is not an error). The body goes
/// to `sink`. Errors: "Cancelled", "Could not write the download to disk", "Bad download address: …" or what curl
/// reported.
#[cfg(unix)]
fn curl(
    url: &str,
    verb: &str,
    headers: &[&str],
    body: &[u8],
    on_head: &mut dyn FnMut(&Head) -> bool,
    sink: &mut dyn FnMut(&[u8]) -> bool,
    cancel: Option<&AtomicBool>,
) -> Result<(), String> {
    use std::io::{Read, Write};
    use std::os::fd::AsRawFd;
    use std::process::{Command, Stdio};
    if !url.starts_with("http://") && !url.starts_with("https://") {
        return Err(format!("Bad download address: {url}"));
    }
    let mut c = Command::new("curl");
    // Headers and body on one stream (-D -), so the headers are known before the first body byte.
    c.args(["-sS", "-L", "--max-redirs", "20", "--connect-timeout", "30", "--suppress-connect-headers"]);
    c.args(["-A", "MultiplayerEverything-Launcher/1.0"]);
    for h in headers {
        c.args(["-H", h]);
    }
    if verb != "GET" {
        c.args(["-X", verb, "--data-binary", "@-"]);
    }
    c.args(["-D", "-", "-o", "-", "--", url]);
    c.stdin(if verb != "GET" { Stdio::piped() } else { Stdio::null() }).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = match c.spawn() {
        Ok(ch) => ch,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Err(curl_problem(127, "")),
        Err(_) => return Err("Cannot start curl".into()),
    };
    if let Some(mut stdin) = child.stdin.take() {
        let data = body.to_vec();
        std::thread::spawn(move || {
            let _ = stdin.write_all(&data);
        });
    }
    let mut err_pipe = child.stderr.take().unwrap();
    let err_reader = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = err_pipe.read_to_string(&mut s);
        s
    });
    let mut out = child.stdout.take().unwrap();
    let (mut head, mut in_body, mut stopped) = (Vec::<u8>::new(), false, false);
    let mut result: Result<(), String> = Ok(());
    let mut buf = vec![0u8; 256 * 1024];
    'read: loop {
        if cancel.is_some_and(|c| c.load(Ordering::Relaxed)) {
            result = Err("Cancelled".into());
            stopped = true;
            break;
        }
        let mut pfd = libc::pollfd { fd: out.as_raw_fd(), events: libc::POLLIN, revents: 0 };
        if unsafe { libc::poll(&mut pfd, 1, 200) } <= 0 {
            continue;
        }
        let n = match out.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(n) => n,
        };
        let mut data = &buf[..n];
        if !in_body {
            head.extend_from_slice(data);
            data = &[];
            // Every block before the last one is a redirect or a 1xx; the body starts after the last.
            while !in_body && let Some(end) = head.windows(4).position(|w| w == b"\r\n\r\n") {
                let Some(h) = Head::parse(&String::from_utf8_lossy(&head[..end])) else {
                    result = Err("The server's answer couldn't be read".into());
                    stopped = true;
                    break 'read;
                };
                let rest = head.split_off(end + 4);
                let more = (100..200).contains(&h.status) || ((300..400).contains(&h.status) && !h.header("location").is_empty());
                head = rest;
                if more {
                    continue;
                }
                in_body = true;
                if !on_head(&h) {
                    stopped = true;
                    break 'read;
                }
                if !head.is_empty() && !sink(&head) {
                    result = Err("Could not write the download to disk".into());
                    stopped = true;
                    break 'read;
                }
                head.clear();
            }
        }
        if in_body && !data.is_empty() && !sink(data) {
            result = Err("Could not write the download to disk".into());
            stopped = true;
            break;
        }
    }
    if stopped {
        let _ = child.kill();
    }
    drop(out);
    let status = child.wait();
    let err_text = err_reader.join().unwrap_or_default();
    if stopped {
        return result;
    }
    let code = status.ok().and_then(|s| s.code()).unwrap_or(1);
    if code != 0 {
        return Err(curl_problem(code, &err_text));
    }
    if !in_body {
        // A body-less answer ends with the header block and nothing after it.
        let text = String::from_utf8_lossy(&head).into_owned();
        let Some(h) = Head::parse(text.split("\r\n\r\n").next().unwrap_or("")) else {
            return Err("The server's answer couldn't be read".into());
        };
        on_head(&h);
    }
    Ok(())
}

/// What a failure reads as, as httpGet words it.
#[cfg(unix)]
fn download_error(host: &str, problem: String) -> String {
    if problem == "Cancelled" || problem == "Could not write the download to disk" || problem.starts_with("Bad download address") {
        problem
    } else {
        format!("Downloading from {host}: {problem}")
    }
}

/// A JSON request to a web service of ours (the reports, src/launcher/reports.rs): the status code and the body, which
/// carries the service's own reason when it refuses. GET when `body` is None.
#[cfg(unix)]
pub fn json_request(url: &str, body: Option<&str>) -> Result<(u32, Vec<u8>), String> {
    let (verb, data) = match body {
        Some(b) => ("POST", b.as_bytes()),
        None => ("GET", &[][..]),
    };
    let (mut status, mut reply) = (0u32, Vec::new());
    let limit = 1024 * 1024;
    curl(
        url,
        verb,
        &["Accept: application/json", "Content-Type: application/json"],
        data,
        &mut |h| {
            status = h.status;
            true
        },
        &mut |d| {
            reply.extend_from_slice(&d[..d.len().min(limit - reply.len().min(limit))]);
            true
        },
        None,
    )
    .map_err(|e| download_error(host_of(url), e))?;
    Ok((status, reply))
}

/// GET `url`, following redirects (GitHub sends downloads to its CDN), handing the body to `sink` as it arrives; a
/// sink that answers false stops it ("Could not write the download to disk"); it is also told the Content-Length
/// header (0 without one). `cancel` is looked at before each read.
#[cfg(unix)]
pub fn get_to(url: &str, sink: &mut dyn FnMut(&[u8], u64) -> bool, cancel: Option<&AtomicBool>) -> Result<(), String> {
    let host = host_of(url);
    let length = std::cell::Cell::new(0u64);
    let mut refused: Option<String> = None;
    let r = curl(
        url,
        "GET",
        // GitHub's API answers JSON only to clients that send an Accept header.
        &["Accept: application/vnd.github+json, */*"],
        &[],
        &mut |h| {
            if h.status != 200 {
                let mut error = format!("{host} answered HTTP {}", h.status);
                // GitHub allows 60 requests an hour without signing in: say how long until it answers again.
                if h.status == 403 || h.status == 429 {
                    let (remaining, reset, retry) = (h.header("x-ratelimit-remaining"), h.header("x-ratelimit-reset"), h.header("retry-after"));
                    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
                    let reset_at = if !reset.is_empty() {
                        wcstoi64(&reset)
                    } else if !retry.is_empty() {
                        now + wcstoi64(&retry)
                    } else {
                        0
                    };
                    if reset_at > 0 && (remaining == "0" || !retry.is_empty()) {
                        error = rate_limit_message(reset_at - now);
                    }
                }
                refused = Some(error);
                return false;
            }
            length.set(wcstoui64(&h.header("content-length")));
            true
        },
        &mut |d| sink(d, length.get()),
        cancel,
    );
    if let Some(e) = refused {
        return Err(e);
    }
    r.map_err(|e| download_error(host, e))
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;

    #[test]
    fn heads_and_hosts_read_as_the_cpp_launcher_reads_them() {
        let h = Head::parse("HTTP/2 403 Forbidden\r\nX-RateLimit-Remaining: 0\r\ncontent-length:  12").unwrap();
        assert_eq!(h.status, 403);
        assert_eq!(h.header("x-ratelimit-remaining"), "0");
        assert_eq!(h.header("content-length"), "12");
        assert!(Head::parse("<html>").is_none());
        assert_eq!(host_of("https://api.github.com:443/repos/x"), "api.github.com");
        assert_eq!(download_error("h", "Couldn't connect to the server".into()), "Downloading from h: Couldn't connect to the server");
        assert_eq!(get("ftp://x/y", 10).unwrap_err(), "Bad download address: ftp://x/y");
    }
}
