// --lobby-session <game>: the lobby (core/lobby.h) driven over standard input and output in JSON lines, for a front
// end that draws its own lobby window (the Tauri prototype). The first input line hosts or joins; later lines are
// commands; every change of the room is printed as a snapshot; when the host starts, the game is launched here.
#pragma once
#include <string>

#include "core/config.h"
#include "core/library.h"
#include "core/settings.h"

namespace fl {

int runLobbySession(const Config& cfg, Library& lib, UserData& user, const std::wstring& userPath, const GameSpec& game);

}  // namespace fl
