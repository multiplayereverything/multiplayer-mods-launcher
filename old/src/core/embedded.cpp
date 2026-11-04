#include "embedded.h"

#include "win.h"

#include <cstring>

namespace fl {

std::wstring embeddedName(const std::wstring& relativePath) {
    std::wstring name = L"F_";
    for (wchar_t ch : relativePath) {
        if (ch >= L'a' && ch <= L'z') name += static_cast<wchar_t>(ch - L'a' + L'A');
        else if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) name += ch;
        else name += L'_';
    }
    return name;
}

#ifndef _WIN32
// Linux: the files are linked in by embedded_files.S (CMakeLists.txt), listed by their resource names.
struct EmbeddedEntry {
    const char* name;
    const char* begin;
    const char* end;
};
extern "C" const EmbeddedEntry fl_embedded_table[];

bool readEmbedded(const std::wstring& relativePath, std::string& out) {
    std::wstring wanted = embeddedName(relativePath);
    for (const EmbeddedEntry* e = fl_embedded_table; e->name; ++e) {
        std::wstring name(e->name, e->name + strlen(e->name));
        if (name != wanted) continue;
        out.assign(e->begin, static_cast<size_t>(e->end - e->begin));
        return true;
    }
    return false;
}
#else
bool readEmbedded(const std::wstring& relativePath, std::string& out) {
    HRSRC res = FindResourceW(nullptr, embeddedName(relativePath).c_str(), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) return false;
    HGLOBAL data = LoadResource(nullptr, res);
    const void* bytes = data ? LockResource(data) : nullptr;
    if (!bytes) return false;
    out.assign(static_cast<const char*>(bytes), SizeofResource(nullptr, res));
    return true;
}
#endif

}  // namespace fl
