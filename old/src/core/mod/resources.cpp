#include "core/mod/resources.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>

#include "core/fs.h"
#include "core/strings.h"
#include "core/win.h"

namespace fl::mod {

bool Resources::readAll(const std::wstring& rel, std::vector<uint8_t>& out) const {
    auto st = stat(rel);
    auto r = open(rel);
    if (!st || !r) return false;
    out.resize(static_cast<size_t>(st->size));
    size_t done = 0;
    while (done < out.size()) {
        size_t n = r->read(out.data() + done, out.size() - done);
        if (n == 0 || n == SIZE_MAX) return false;
        done += n;
    }
    return true;
}

bool Resources::readText(const std::wstring& rel, std::string& out) const {
    std::vector<uint8_t> bytes;
    if (!readAll(rel, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

std::wstring normRel(const std::wstring& rel) {
    std::wstring s = replaceAll(rel, L"\\", L"/");
    if (s.find(L':') != std::wstring::npos) return {};
    if (s.rfind(L"//", 0) == 0) return {};
    std::vector<std::wstring> out;
    for (auto& part : split(s, L'/')) {
        if (part.empty() || part == L".") continue;
        if (part == L"..") return {};
        out.push_back(part);
    }
    return join(out, L"/");
}

namespace {

class HandleReader : public ResReader {
public:
    HandleReader(HANDLE h, uint64_t size) : h_(h), remaining_(size) {}
    ~HandleReader() override { CloseHandle(h_); }
    size_t read(void* buffer, size_t size) override {
        if (remaining_ == 0) return 0;
        DWORD want = static_cast<DWORD>(std::min<uint64_t>({size, remaining_, 1u << 30}));
        DWORD got = 0;
        if (!ReadFile(h_, buffer, want, &got, nullptr)) return SIZE_MAX;
        if (got == 0) return SIZE_MAX;  // file shorter than expected
        remaining_ -= got;
        return got;
    }

private:
    HANDLE h_;
    uint64_t remaining_;
};

HANDLE openRead(const std::wstring& path) {
    return CreateFileW(fs::longPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

class DirResources : public Resources {
public:
    explicit DirResources(std::wstring root) : root_(fs::normalize(root)) {}

    std::wstring describe() const override { return L"folder " + root_; }

    bool isDir(const std::wstring& rel) const override {
        std::wstring p;
        return resolve(rel, p) && fs::isDir(p);
    }

    std::optional<ResEntry> stat(const std::wstring& rel) const override {
        std::wstring p;
        if (!resolve(rel, p) || !fs::isFile(p)) return std::nullopt;
        auto size = fs::fileSize(p);
        return ResEntry{normRel(rel), size.value_or(0), {}};
    }

    std::vector<ResEntry> list(const std::wstring& dirRel) const override {
        std::vector<ResEntry> out;
        std::wstring p;
        if (!resolve(dirRel, p)) return out;
        if (fs::isFile(p)) {
            out.push_back({fs::fileName(p), fs::fileSize(p).value_or(0), {}});
            return out;
        }
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(fs::longPath(p), ec), end;
        size_t prefix = fs::longPath(p).size() + 1;
        for (; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::wstring full = it->path().wstring();
            out.push_back({replaceAll(full.substr(prefix), L"\\", L"/"), static_cast<uint64_t>(it->file_size(ec)), {}});
        }
        std::sort(out.begin(), out.end(), [](const ResEntry& a, const ResEntry& b) { return a.rel < b.rel; });
        return out;
    }

    std::unique_ptr<ResReader> open(const std::wstring& rel) const override {
        std::wstring p;
        if (!resolve(rel, p)) return nullptr;
        auto size = fs::fileSize(p);
        HANDLE h = openRead(p);
        if (!size || h == INVALID_HANDLE_VALUE) return nullptr;
        return std::make_unique<HandleReader>(h, *size);
    }

private:
    std::wstring root_;

    bool resolve(const std::wstring& rel, std::wstring& out) const {
        std::wstring n = normRel(rel);
        if (n.empty() && !trim(rel).empty() && trim(rel) != L"." && trim(rel) != L"/") return false;
        out = n.empty() ? root_ : fs::join(root_, n);
        return true;
    }
};

}  // namespace

std::unique_ptr<Resources> openDirectory(const std::wstring& root) { return std::make_unique<DirResources>(root); }

}  // namespace fl::mod
