//! Prints what the app's `rooms` and `all_rooms` commands return, as `MultiplayerEverythingLauncher.exe --rooms <id>
//! --json` and `--rooms all --json` print it, with the same exit codes, so tests/parity/rooms.mjs can compare the two
//! on the same launcher.json and server answers.
//!
//!   rooms_json <game id | all>

fn main() {
    let game = std::env::args().nth(1).unwrap_or_default();
    let answer = if game == "all" {
        app_lib::launcher::rooms::all_rooms().map(|v| (v, true))
    } else {
        app_lib::launcher::rooms::game_rooms(&game)
    };
    match answer {
        Ok((v, listed)) => {
            println!("{v}");
            std::process::exit(if listed { 0 } else { 1 });
        }
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(3);
        }
    }
}
