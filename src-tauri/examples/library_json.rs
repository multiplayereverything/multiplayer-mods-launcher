//! Prints what the app's `library` command returns, as `MultiplayerEverythingLauncher.exe --list --json` prints it,
//! so tests/parity/library.mjs can compare the two on the same launcher.json, settings and data folder.

fn main() {
    match app_lib::launcher::library_json::library_json() {
        Ok(v) => println!("{v}"),
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(3);
        }
    }
}
