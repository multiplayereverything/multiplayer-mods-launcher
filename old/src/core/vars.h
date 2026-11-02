// Runtime variables used in config strings: ${LAUNCHER_DIR}, ${SWF},
// ${env:APPDATA}, ${folder:Desktop}... (see CONFIG.md for the full list).
#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

class Vars {
public:
    void set(const std::wstring& name, const std::wstring& value);
    std::wstring get(const std::wstring& name) const;
    bool has(const std::wstring& name) const;

    // Unknown variables are left as written and reported through `unknown`.
    std::wstring expand(std::wstring_view text, std::vector<std::wstring>* unknown = nullptr) const;

private:
    std::wstring expandDepth(std::wstring_view text, int depth, std::vector<std::wstring>* unknown) const;
    std::map<std::wstring, std::wstring> values_;  // keys upper-case
};

// Names every config string may reference (without ${}), for validation.
const std::vector<std::wstring>& knownVariableNames();

}  // namespace fl
