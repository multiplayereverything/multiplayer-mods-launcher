//! Runs the app's player and game-file commands (src/launcher/install.rs), printing what
//! `MultiplayerEverythingLauncher.exe --install-player <id>`, `--update-players [--check] [--json]` and
//! `--download-game <id>` printed: stdout lines, the failure on stderr, the same exit codes, so
//! tests/parity/downloads.mjs can compare the two.
//!
//!   downloads install-player <id> | update-players [--check] [--json] | download-game <id>

use app_lib::launcher::install::{download_game_command, install_player_command, update_players_command};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let what = args.first().map(String::as_str).unwrap_or_default();
    let id = args.get(1).cloned().unwrap_or_default();
    let mut out = |line: &str| println!("{line}");
    let (code, message) = match what {
        "install-player" => install_player_command(&id, &mut out),
        "update-players" => update_players_command(args.iter().any(|a| a == "--check"), args.iter().any(|a| a == "--json"), &mut out),
        "download-game" => download_game_command(&id, &mut out),
        _ => (3, format!("unknown command {what}")),
    };
    if !message.is_empty() {
        eprintln!("{message}");
    }
    std::process::exit(code);
}
