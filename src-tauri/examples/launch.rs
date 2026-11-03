//! Starts a game the way the app's `launch` command does (src/launcher/launch.rs), printing what
//! `MultiplayerEverythingLauncher.exe --launch ... --json` printed: the relay's report lines, then the message on
//! stdout (exit 0) or stderr, with the same exit codes, so tests/parity/launch.mjs can compare the two.
//!
//!   launch <game>[:<action>] [--join CODE] [--address A] [--coop-save X] [--tv] [--wait] [--stop-after MS]
//!
//! --stop-after stands in for the window's Stop hosting (which killed the C++ launcher).

use std::sync::Arc;

use app_lib::launcher::launch::{Request, Stop, run};

fn main() {
    let mut args = std::env::args().skip(1);
    let target = args.next().unwrap_or_default();
    let (game, action) = match target.split_once(':') {
        Some((g, a)) => (g.to_string(), a.to_string()),
        None => (target.clone(), String::new()),
    };
    let mut req = Request { game, action, ..Default::default() };
    let mut stop_after = None;
    while let Some(a) = args.next() {
        match a.as_str() {
            "--join" => req.join = args.next().unwrap_or_default(),
            "--address" => req.address = args.next().unwrap_or_default(),
            "--coop-save" => req.coop_save = args.next().unwrap_or_default(),
            "--tv" => req.tv = true,
            "--wait" => req.wait = true,
            "--stop-after" => stop_after = args.next().and_then(|m| m.parse::<u64>().ok()),
            _ => {}
        }
    }
    let stop = Arc::new(Stop::default());
    if let Some(ms) = stop_after {
        let s = stop.clone();
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(ms));
            s.set();
        });
    }
    let (code, message) = run(&req, &mut |line| println!("{line}"), &stop);
    if stop.is_set() {
        // Killed, the C++ launcher printed nothing more and exited with 1 (TerminateProcess).
        std::process::exit(1);
    }
    if code == 0 {
        println!("{message}");
    } else {
        eprintln!("{message}");
    }
    std::process::exit(code);
}
