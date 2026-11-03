//! HTTP GET the way the C++ launcher does it (old/src/core/install.cpp `httpGet`): through WinHTTP, so the system's
//! proxy settings, TLS and redirects behave as they did, and a failure reads the same ("<host> answered HTTP 404",
//! "Downloading from <host>: <Windows' message> (code N)"). Text answers (`get`) and downloads streamed to disk
//! (`get_to`, with the Content-Length for progress and a cancel flag).

use std::sync::atomic::{AtomicBool, Ordering};

use windows_sys::Win32::Foundation::GetLastError;
use windows_sys::Win32::Networking::WinHttp::*;
use windows_sys::Win32::System::Diagnostics::Debug::{FORMAT_MESSAGE_FROM_SYSTEM, FORMAT_MESSAGE_IGNORE_INSERTS, FormatMessageW};

use super::paths::{from_wide, wide};
use super::text::trim;

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

struct Handle(*mut core::ffi::c_void);

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

/// GET `url`, following redirects (GitHub sends downloads to its CDN), handing the body to `sink` as it arrives; a
/// sink that answers false stops it ("Could not write the download to disk", the C++ wording whether the disk or a
/// size limit refused); it is also told the Content-Length header (0 without one). `cancel` is looked at before each
/// read.
pub fn get_to(url: &str, sink: &mut dyn FnMut(&[u8], u64) -> bool, cancel: Option<&AtomicBool>) -> Result<(), String> {
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
    let until_nul = |b: &[u16]| from_wide(&b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())]);
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
    let verb = wide("GET");
    let req = Handle(if conn.0.is_null() {
        std::ptr::null_mut()
    } else {
        unsafe { WinHttpOpenRequest(conn.0, verb.as_ptr(), object.as_ptr(), std::ptr::null(), std::ptr::null(), std::ptr::null(), flags) }
    });
    if req.0.is_null() {
        return Err(last_error(&format!("Connecting to {host_s}")));
    }
    // GitHub's API answers JSON only to clients that send an Accept header.
    let headers = wide("Accept: application/vnd.github+json, */*\r\n");
    if unsafe { WinHttpSendRequest(req.0, headers.as_ptr(), u32::MAX, std::ptr::null(), 0, 0, 0) } == 0
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
