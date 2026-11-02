// String helpers shared by every module. All UI and path text is UTF-16
// (std::wstring); config files and logs are UTF-8.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

std::wstring toLower(std::wstring_view s);
std::wstring toUpper(std::wstring_view s);
bool iequals(std::wstring_view a, std::wstring_view b);
bool startsWithI(std::wstring_view s, std::wstring_view prefix);
std::wstring trim(std::wstring_view s);
std::vector<std::wstring> split(std::wstring_view s, wchar_t sep);
std::wstring join(const std::vector<std::wstring>& parts, std::wstring_view sep);
std::wstring replaceAll(std::wstring s, std::wstring_view from, std::wstring_view to);

// "1.4 MB", "820 KB", "12 bytes".
std::wstring formatBytes(uint64_t bytes);
// Human readable text for a Win32 error code, e.g. "Access is denied. (code 5)".
std::wstring win32ErrorMessage(unsigned long code);

// Case-insensitive glob over '/'-separated relative paths.
// '*' and '?' stay inside one path segment, '**' spans segments.
bool globMatch(std::wstring_view pattern, std::wstring_view text);

// Compares dotted versions ("1.10.0" > "1.9.2"). Returns -1, 0 or 1.
int compareVersions(std::wstring_view a, std::wstring_view b);

std::wstring hexEncode(const uint8_t* data, size_t n);

}  // namespace fl
