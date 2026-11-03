//! The C++ launcher's work, moved into the app part by part (PORTING.md). Part 1: launcher.json, its variables,
//! settings.json, the library (players, game files, command lines) and the website's games.json merge, which
//! together answer what `--list --json` answered. Part 2: the open rooms on the central server and which server to
//! ask (`--rooms <id> --json`, `--rooms all --json`). Part 3: starting a game (`--launch ... --wait`): the command
//! line, host options, the relay, the HTML5 web server, Discord and the play time.

pub mod catalog;
pub mod config;
pub mod discord;
pub mod embedded;
pub mod http;
pub mod json;
pub mod launch;
pub mod library;
pub mod library_json;
pub mod log;
pub mod paths;
pub mod process;
pub mod relay;
pub mod rooms;
pub mod settings;
pub mod site;
pub mod text;
pub mod texts;
pub mod vars;
pub mod webserver;

#[cfg(test)]
mod tests;
#[cfg(test)]
mod tests_launch;
