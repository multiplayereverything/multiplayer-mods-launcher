#include "embedded.h"

#include "win.h"

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

bool readEmbedded(const std::wstring& relativePath, std::string& out) {
    HRSRC res = FindResourceW(nullptr, embeddedName(relativePath).c_str(), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) return false;
    HGLOBAL data = LoadResource(nullptr, res);
    const void* bytes = data ? LockResource(data) : nullptr;
    if (!bytes) return false;
    out.assign(static_cast<const char*>(bytes), SizeofResource(nullptr, res));
    return true;
}

}  // namespace fl
