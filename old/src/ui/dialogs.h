// Small standard dialogs shared by the launcher's windows.
#pragma once
#include <string>

#include "core/win.h"

namespace fl::ui {

// The folder chooser; empty when the player cancels. `start` is where it opens.
std::wstring pickFolder(HWND owner, const std::wstring& title, const std::wstring& start);

}  // namespace fl::ui
