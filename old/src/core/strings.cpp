#include "strings.h"

#include <cwchar>
#include <cwctype>

#include "win.h"

namespace fl {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring toLower(std::wstring_view s) {
    std::wstring r(s);
    if (!r.empty()) CharLowerBuffW(r.data(), static_cast<DWORD>(r.size()));
    return r;
}

std::wstring toUpper(std::wstring_view s) {
    std::wstring r(s);
    if (!r.empty()) CharUpperBuffW(r.data(), static_cast<DWORD>(r.size()));
    return r;
}

bool iequals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool startsWithI(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

std::wstring trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

std::vector<std::wstring> split(std::wstring_view s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::wstring join(const std::vector<std::wstring>& parts, std::wstring_view sep) {
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::wstring replaceAll(std::wstring s, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::wstring formatBytes(uint64_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + (bytes == 1 ? L" byte" : L" bytes");
    static const wchar_t* units[] = {L"bytes", L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    swprintf(buf, 64, v < 10 ? L"%.1f %s" : L"%.0f %s", v, units[u]);
    return buf;
}

std::wstring win32ErrorMessage(unsigned long code) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring msg = n && buf ? trim(std::wstring(buf, n)) : L"Unknown error";
    if (buf) LocalFree(buf);
    return msg + L" (code " + std::to_wstring(code) + L")";
}

static bool globImpl(const wchar_t* p, const wchar_t* t) {
    while (*p) {
        if (p[0] == L'*' && p[1] == L'*') {
            p += 2;
            if (*p == L'/' && globImpl(p + 1, t)) return true;  // "**/" may match zero folders
            for (const wchar_t* s = t;; ++s) {
                if (globImpl(p, s)) return true;
                if (!*s) return false;
            }
        }
        if (*p == L'*') {
            ++p;
            for (const wchar_t* s = t;; ++s) {
                if (globImpl(p, s)) return true;
                if (!*s || *s == L'/') return false;
            }
        }
        if (!*t) return false;
        if (*p == L'?') {
            if (*t == L'/') return false;
        } else if (*p != *t) {
            return false;
        }
        ++p;
        ++t;
    }
    return *t == 0;
}

bool globMatch(std::wstring_view pattern, std::wstring_view text) {
    std::wstring p = replaceAll(toLower(pattern), L"\\", L"/");
    std::wstring t = replaceAll(toLower(text), L"\\", L"/");
    return globImpl(p.c_str(), t.c_str());
}

static std::vector<std::wstring> versionParts(std::wstring_view v) {
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.remove_prefix(1);
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : v) {
        if (std::iswalnum(c)) {
            cur += c;
        } else if (!cur.empty()) {
            parts.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

int compareVersions(std::wstring_view a, std::wstring_view b) {
    auto pa = versionParts(a), pb = versionParts(b);
    size_t n = pa.size() > pb.size() ? pa.size() : pb.size();
    for (size_t i = 0; i < n; ++i) {
        std::wstring x = i < pa.size() ? pa[i] : L"0";
        std::wstring y = i < pb.size() ? pb[i] : L"0";
        bool nx = !x.empty() && std::iswdigit(x[0]), ny = !y.empty() && std::iswdigit(y[0]);
        if (nx && ny) {
            unsigned long long vx = wcstoull(x.c_str(), nullptr, 10), vy = wcstoull(y.c_str(), nullptr, 10);
            if (vx != vy) return vx < vy ? -1 : 1;
        } else {
            int c = _wcsicmp(x.c_str(), y.c_str());
            if (c != 0) return c < 0 ? -1 : 1;
        }
    }
    return 0;
}

std::wstring hexEncode(const uint8_t* data, size_t n) {
    static const wchar_t* digits = L"0123456789abcdef";
    std::wstring out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

}  // namespace fl
