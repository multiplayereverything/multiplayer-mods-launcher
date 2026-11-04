// Linux: the Microsoft C runtime names the sources use without including anything Windows-only (MSVC's <cstdlib>
// and <cwchar> declare them). CMakeLists.txt includes this in every source file. Defined in posix.cpp.
#pragma once
#ifdef __cplusplus
#include <cstddef>
#include <cstdlib>
#include <cwchar>

int _wcsicmp(const wchar_t* a, const wchar_t* b);
int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n);
int _stricmp(const char* a, const char* b);
inline int _wtoi(const wchar_t* s) { return static_cast<int>(wcstol(s, nullptr, 10)); }
inline long long _wcstoi64(const wchar_t* s, wchar_t** end, int base) { return wcstoll(s, end, base); }
inline unsigned long long _wcstoui64(const wchar_t* s, wchar_t** end, int base) { return wcstoull(s, end, base); }
// Only ever given numbers (%d), where the _s checks add nothing.
#define swscanf_s swscanf
#endif
