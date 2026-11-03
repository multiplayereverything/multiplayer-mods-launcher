//! Starting players and a game's server program, and this PC's network addresses (old/src/core/process.cpp). The
//! player gets exactly the command line the C++ launcher built (std's Command quotes in its own way) and its
//! environment block, through CreateProcess as there.

use std::net::TcpStream;
use std::thread::JoinHandle;

use windows_sys::Win32::Foundation::{
    CloseHandle, GENERIC_READ, GetLastError, HANDLE, HANDLE_FLAG_INHERIT, INVALID_HANDLE_VALUE, SetHandleInformation,
    WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows_sys::Win32::Security::SECURITY_ATTRIBUTES;
use windows_sys::Win32::Storage::FileSystem::{CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING, ReadFile};
use windows_sys::Win32::System::Environment::{FreeEnvironmentStringsW, GetEnvironmentStringsW};
use windows_sys::Win32::System::JobObjects::{
    AssignProcessToJobObject, CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
    JobObjectExtendedLimitInformation, SetInformationJobObject, TerminateJobObject,
};
use windows_sys::Win32::System::Pipes::CreatePipe;
use windows_sys::Win32::System::Threading::{
    CREATE_NO_WINDOW, CREATE_SUSPENDED, CREATE_UNICODE_ENVIRONMENT, CreateProcessW, PROCESS_INFORMATION, ResumeThread,
    STARTF_USESTDHANDLES, STARTUPINFOW, TerminateProcess, WaitForMultipleObjects, WaitForSingleObject,
};

use super::http::win32_error_message;
use super::log;
use super::paths::{from_wide, wide};
use super::text::{iequals, starts_with_i, to_lower};

/// One argument as CommandLineToArgvW reads it back: quoted only when it has to be, backslashes doubled only before
/// a quote.
pub fn quote_arg(arg: &str) -> String {
    if !arg.is_empty() && !arg.contains([' ', '\t', '"']) {
        return arg.to_string();
    }
    let mut out = String::from("\"");
    let mut slashes = 0;
    for c in arg.chars() {
        if c == '\\' {
            slashes += 1;
            continue;
        }
        let n = if c == '"' { slashes * 2 + 1 } else { slashes };
        out.push_str(&"\\".repeat(n));
        slashes = 0;
        out.push(c);
    }
    out.push_str(&"\\".repeat(slashes * 2));
    out.push('"');
    out
}

/// argv[0] is always quoted: CreateProcess otherwise tries "C:\Program.exe" first.
pub fn build_command_line(exe: &str, args: &[String]) -> String {
    let mut cmd = format!("\"{exe}\"");
    for a in args {
        cmd.push(' ');
        cmd.push_str(&quote_arg(a));
    }
    cmd
}

pub type EnvVars = Vec<(String, String)>;

/// `current` (NUL-separated entries ending in an empty one, as GetEnvironmentStrings gives them) with `extra` set on
/// top: a name already there is replaced, ignoring case. The "=C:=C:\dir" entries (per-drive folders) start with
/// '=' and are kept as they are.
pub fn environment_block(current: &[u16], extra: &[(String, String)]) -> Vec<u16> {
    let mut block = Vec::new();
    for entry in current.split(|&c| c == 0).take_while(|e| !e.is_empty()) {
        let text = from_wide(entry);
        let name = match text.char_indices().skip(1).find(|&(_, c)| c == '=') {
            Some((i, _)) => &text[..i],
            None => &text,
        };
        if !extra.iter().any(|(n, _)| iequals(n, name)) {
            block.extend_from_slice(entry);
            block.push(0);
        }
    }
    for (name, value) in extra {
        block.extend(format!("{name}={value}").encode_utf16());
        block.push(0);
    }
    block.push(0);
    block
}

/// A started process; the handle closes with it (the process keeps running).
pub struct Process {
    handle: HANDLE,
    pub pid: u32,
}

// A process handle may be waited on from any thread.
unsafe impl Send for Process {}
unsafe impl Sync for Process {}

impl Process {
    pub fn raw(&self) -> HANDLE {
        self.handle
    }
    pub fn running(&self) -> bool {
        unsafe { WaitForSingleObject(self.handle, 0) == WAIT_TIMEOUT }
    }
}

impl Drop for Process {
    fn drop(&mut self) {
        unsafe { CloseHandle(self.handle) };
    }
}

/// Waits up to `ms` for the process or `wake` (an event handle, or null): true once the process has exited.
pub fn wait_process(p: &Process, wake: HANDLE, ms: u32) -> bool {
    let handles = [p.handle, wake];
    let n = if wake.is_null() { 1 } else { 2 };
    unsafe { WaitForMultipleObjects(n, handles.as_ptr(), 0, ms) == WAIT_OBJECT_0 }
}

/// `hidden`: no console window (for helper tools). `env`: variables set for the new process only.
pub fn start_process(exe: &str, args: &[String], work_dir: &str, hidden: bool, env: &[(String, String)]) -> Result<Process, String> {
    let cmd = build_command_line(exe, args);
    let env_text: String = env.iter().map(|(n, v)| format!(" {n}={v}")).collect();
    log::info(&format!(
        "Starting: {cmd}{}{}",
        if work_dir.is_empty() { String::new() } else { format!("  (in {work_dir})") },
        if env_text.is_empty() { String::new() } else { format!("  (with{env_text})") }
    ));
    let mut block = Vec::new();
    if !env.is_empty() {
        unsafe {
            let current = GetEnvironmentStringsW();
            let mut len = 0;
            // The block ends with two NULs.
            while !(*current.add(len) == 0 && *current.add(len + 1) == 0) {
                len += 1;
            }
            block = environment_block(std::slice::from_raw_parts(current, len + 2), env);
            FreeEnvironmentStringsW(current);
        }
    }
    let exe_w = wide(exe);
    let mut cmd_w = wide(&cmd);
    let dir_w = wide(work_dir);
    let mut si: STARTUPINFOW = unsafe { std::mem::zeroed() };
    si.cb = std::mem::size_of::<STARTUPINFOW>() as u32;
    let mut pi: PROCESS_INFORMATION = unsafe { std::mem::zeroed() };
    let flags = (if hidden { CREATE_NO_WINDOW } else { 0 }) | (if block.is_empty() { 0 } else { CREATE_UNICODE_ENVIRONMENT });
    let ok = unsafe {
        CreateProcessW(
            exe_w.as_ptr(),
            cmd_w.as_mut_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            0,
            flags,
            if block.is_empty() { std::ptr::null() } else { block.as_ptr().cast() },
            if work_dir.is_empty() { std::ptr::null() } else { dir_w.as_ptr() },
            &si,
            &mut pi,
        )
    };
    if ok == 0 {
        let error = win32_error_message(unsafe { GetLastError() });
        log::error(&format!("CreateProcess failed: {error}"));
        return Err(error);
    }
    unsafe { CloseHandle(pi.hThread) };
    Ok(Process { handle: pi.hProcess, pid: pi.dwProcessId })
}

#[derive(Clone, Debug, PartialEq)]
pub struct NetAddress {
    pub address: String, // "192.168.1.20"
    pub adapter: String, // "Wi-Fi", "ZeroTier One"
}

/// Up, non-loopback IPv4 addresses, physical adapters first.
pub fn local_addresses() -> Vec<NetAddress> {
    use windows_sys::Win32::NetworkManagement::IpHelper::{
        GAA_FLAG_SKIP_ANYCAST, GAA_FLAG_SKIP_DNS_SERVER, GAA_FLAG_SKIP_MULTICAST, GetAdaptersAddresses, IF_TYPE_ETHERNET_CSMACD,
        IF_TYPE_IEEE80211, IF_TYPE_SOFTWARE_LOOPBACK, IP_ADAPTER_ADDRESSES_LH,
    };
    use windows_sys::Win32::NetworkManagement::Ndis::IfOperStatusUp;
    use windows_sys::Win32::Networking::WinSock::{AF_INET, SOCKADDR_IN};
    const ERROR_BUFFER_OVERFLOW: u32 = 111;
    let (mut out, mut virtual_ones) = (Vec::new(), Vec::new());
    let mut size: u32 = 16 * 1024;
    let mut buf: Vec<u64> = Vec::new();
    let mut rc = ERROR_BUFFER_OVERFLOW;
    for _ in 0..3 {
        if rc != ERROR_BUFFER_OVERFLOW {
            break;
        }
        buf = vec![0u64; (size as usize).div_ceil(8)];
        rc = unsafe {
            GetAdaptersAddresses(
                AF_INET as u32,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                std::ptr::null(),
                buf.as_mut_ptr().cast(),
                &mut size,
            )
        };
    }
    if rc != 0 {
        return out;
    }
    let mut a = buf.as_ptr() as *const IP_ADAPTER_ADDRESSES_LH;
    while !a.is_null() {
        let ad = unsafe { &*a };
        a = ad.Next;
        if ad.OperStatus != IfOperStatusUp || ad.IfType == IF_TYPE_SOFTWARE_LOOPBACK {
            continue;
        }
        let adapter = if ad.FriendlyName.is_null() {
            String::new()
        } else {
            let mut n = 0;
            while unsafe { *ad.FriendlyName.add(n) } != 0 {
                n += 1;
            }
            from_wide(unsafe { std::slice::from_raw_parts(ad.FriendlyName, n) })
        };
        let mut u = ad.FirstUnicastAddress;
        while !u.is_null() {
            let ua = unsafe { &*u };
            u = ua.Next;
            let sa = ua.Address.lpSockaddr;
            if sa.is_null() || unsafe { (*sa).sa_family } != AF_INET {
                continue;
            }
            let sin = unsafe { &*(sa as *const SOCKADDR_IN) };
            let b = unsafe { sin.sin_addr.S_un.S_addr }.to_ne_bytes();
            if b[0] == 169 && b[1] == 254 {
                continue; // link-local: nobody can reach it
            }
            let n = NetAddress { address: format!("{}.{}.{}.{}", b[0], b[1], b[2], b[3]), adapter: adapter.clone() };
            // Hyper-V/WSL switches are noise; VPN adapters (ZeroTier, Radmin, Tailscale) are exactly what friends over
            // the internet need, so those stay.
            let lower = to_lower(&n.adapter);
            if lower.contains("vethernet") || lower.contains("virtualbox") || lower.contains("vmware") {
                continue;
            }
            if ad.IfType == IF_TYPE_ETHERNET_CSMACD || ad.IfType == IF_TYPE_IEEE80211 { out.push(n) } else { virtual_ones.push(n) }
        }
    }
    out.extend(virtual_ones);
    out
}

struct SendHandle(HANDLE);
unsafe impl Send for SendHandle {}

/// A background program next to a game, such as an HTML5 mod's co-op server: started hidden inside a job object, so
/// it and everything it starts (a tunnel) end together, even if the app crashes. Every line it prints goes to
/// `on_line` on a reader thread.
#[derive(Default)]
pub struct BackgroundProcess {
    job: Option<SendHandle>,
    process: Option<Process>,
    read: Option<SendHandle>,
    reader: Option<JoinHandle<()>>,
}

impl BackgroundProcess {
    pub fn start(&mut self, exe: &str, args: &[String], work_dir: &str, on_line: impl Fn(&str) + Send + 'static) -> Result<(), String> {
        self.stop();
        let sa = SECURITY_ATTRIBUTES { nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32, lpSecurityDescriptor: std::ptr::null_mut(), bInheritHandle: 1 };
        let (mut read, mut write): (HANDLE, HANDLE) = (std::ptr::null_mut(), std::ptr::null_mut());
        if unsafe { CreatePipe(&mut read, &mut write, &sa, 0) } == 0 {
            return Err(win32_error_message(unsafe { GetLastError() }));
        }
        unsafe { SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) }; // only the child's end is inherited
        self.read = Some(SendHandle(read));
        let nul_name = wide("NUL");
        let nul = unsafe { CreateFileW(nul_name.as_ptr(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, std::ptr::null_mut()) };
        let job = unsafe { CreateJobObjectW(std::ptr::null(), std::ptr::null()) };
        if !job.is_null() {
            let mut limits: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = unsafe { std::mem::zeroed() };
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            unsafe {
                SetInformationJobObject(job, JobObjectExtendedLimitInformation, (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(), std::mem::size_of_val(&limits) as u32)
            };
            self.job = Some(SendHandle(job));
        }
        let cmd = build_command_line(exe, args);
        log::info(&format!("Starting server: {cmd}  (in {work_dir})"));
        let mut si: STARTUPINFOW = unsafe { std::mem::zeroed() };
        si.cb = std::mem::size_of::<STARTUPINFOW>() as u32;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = write;
        si.hStdError = write;
        let mut pi: PROCESS_INFORMATION = unsafe { std::mem::zeroed() };
        let (exe_w, mut cmd_w, dir_w) = (wide(exe), wide(&cmd), wide(work_dir));
        // Suspended until it is in the job, so nothing it starts can escape the job.
        let ok = unsafe {
            CreateProcessW(
                exe_w.as_ptr(),
                cmd_w.as_mut_ptr(),
                std::ptr::null(),
                std::ptr::null(),
                1,
                CREATE_NO_WINDOW | CREATE_SUSPENDED,
                std::ptr::null(),
                if work_dir.is_empty() { std::ptr::null() } else { dir_w.as_ptr() },
                &si,
                &mut pi,
            )
        };
        let code = unsafe { GetLastError() };
        unsafe { CloseHandle(write) };
        if nul != INVALID_HANDLE_VALUE {
            unsafe { CloseHandle(nul) };
        }
        if ok == 0 {
            self.stop();
            return Err(win32_error_message(code));
        }
        if !job.is_null() {
            unsafe { AssignProcessToJobObject(job, pi.hProcess) };
        }
        unsafe {
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
        }
        self.process = Some(Process { handle: pi.hProcess, pid: pi.dwProcessId });
        let r = SendHandle(read);
        self.reader = Some(std::thread::spawn(move || {
            let r = r;
            let mut pending: Vec<u8> = Vec::new();
            let mut buf = [0u8; 4096];
            loop {
                let mut n = 0u32;
                if unsafe { ReadFile(r.0, buf.as_mut_ptr(), buf.len() as u32, &mut n, std::ptr::null_mut()) } == 0 || n == 0 {
                    break;
                }
                pending.extend_from_slice(&buf[..n as usize]);
                while let Some(nl) = pending.iter().position(|&b| b == b'\n') {
                    let mut line: Vec<u8> = pending.drain(..=nl).collect();
                    line.pop();
                    if line.last() == Some(&b'\r') {
                        line.pop();
                    }
                    on_line(&String::from_utf8_lossy(&line));
                }
            }
            if !pending.is_empty() {
                on_line(&String::from_utf8_lossy(&pending));
            }
        }));
        Ok(())
    }

    /// Ends the whole process tree.
    pub fn stop(&mut self) {
        if let Some(job) = self.job.take() {
            unsafe {
                TerminateJobObject(job.0, 1);
                CloseHandle(job.0);
            }
        } else if let Some(p) = &self.process {
            unsafe { TerminateProcess(p.handle, 1) };
        }
        // The pipe closes once every process holding it is gone; the reader ends then.
        if let Some(r) = self.reader.take() {
            let _ = r.join();
        }
        if let Some(read) = self.read.take() {
            unsafe { CloseHandle(read.0) };
        }
        self.process = None;
    }

    pub fn running(&self) -> bool {
        self.process.as_ref().is_some_and(Process::running)
    }
}

impl Drop for BackgroundProcess {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Polls until something accepts connections at `url`'s host and port (an http(s) address), up to `timeout_ms`.
/// False on timeout or when `alive` says the server already quit.
pub fn wait_for_server(url: &str, timeout_ms: u64, alive: impl Fn() -> bool) -> bool {
    let rest = match url.find("://") {
        Some(i) => &url[i + 3..],
        None => url,
    };
    let host_port = rest.split('/').next().unwrap_or("");
    let (mut host, mut port) = (host_port.to_string(), if starts_with_i(url, "https://") { "443".to_string() } else { "80".to_string() });
    if let Some(colon) = host_port.rfind(':') {
        host = host_port[..colon].to_string();
        port = host_port[colon + 1..].to_string();
    }
    let until = std::time::Instant::now() + std::time::Duration::from_millis(timeout_ms);
    let mut up = false;
    while !up && std::time::Instant::now() < until && alive() {
        if let Ok(Some(addr)) = super::rooms::resolve_v4(&host, &port) {
            up = TcpStream::connect(addr).is_ok();
        }
        if !up {
            std::thread::sleep(std::time::Duration::from_millis(250));
        }
    }
    up
}
