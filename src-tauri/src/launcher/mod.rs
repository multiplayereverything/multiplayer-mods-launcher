//! The C++ launcher's work, moved into the app part by part (PORTING.md). Part 1: launcher.json, its variables,
//! settings.json, the library (players, game files, command lines) and the website's games.json merge, which
//! together answer what `--list --json` answered.

pub mod catalog;
pub mod config;
pub mod embedded;
pub mod json;
pub mod library;
pub mod library_json;
pub mod paths;
pub mod settings;
pub mod site;
pub mod text;
pub mod texts;
pub mod vars;

#[cfg(test)]
mod tests;
