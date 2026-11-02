// Files built into the exe: launcher.json and the covers, so the launcher can
// be handed out as one .exe. A real file next to the exe always wins.
//
// They are RCDATA resources named "F_" + the path relative to the launcher
// folder, upper-cased, with every other character turned into '_'
// (covers\gunmayhem2.png -> F_COVERS_GUNMAYHEM2_PNG). CMakeLists.txt writes the
// same names when it builds the resource script.
#pragma once
#include <string>

namespace fl {

std::wstring embeddedName(const std::wstring& relativePath);
// The built-in copy of `relativePath`; false when the exe has none.
bool readEmbedded(const std::wstring& relativePath, std::string& out);

}  // namespace fl
