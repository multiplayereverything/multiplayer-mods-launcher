// Read-only view over a mod package's files (an unpacked folder).
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fl::mod {

struct ResEntry {
    std::wstring rel;    // '/'-separated, relative to the listed folder
    uint64_t size = 0;
    std::string sha256;  // known for bundles, empty for folders
};

class ResReader {
public:
    virtual ~ResReader() = default;
    // Returns bytes read; 0 at the end; SIZE_MAX on a read error.
    virtual size_t read(void* buffer, size_t size) = 0;
};

class Resources {
public:
    virtual ~Resources() = default;
    virtual std::wstring describe() const = 0;
    virtual bool isDir(const std::wstring& rel) const = 0;
    virtual std::optional<ResEntry> stat(const std::wstring& rel) const = 0;
    // Every file below `dirRel` (recursive, sorted). A file path lists itself.
    virtual std::vector<ResEntry> list(const std::wstring& dirRel) const = 0;
    virtual std::unique_ptr<ResReader> open(const std::wstring& rel) const = 0;

    bool exists(const std::wstring& rel) const { return stat(rel).has_value() || isDir(rel); }
    bool readAll(const std::wstring& rel, std::vector<uint8_t>& out) const;
    bool readText(const std::wstring& rel, std::string& out) const;
};

// Canonical relative path: '/' separators, no "." segments. Empty when the
// path is absolute or escapes upwards with "..".
std::wstring normRel(const std::wstring& rel);

std::unique_ptr<Resources> openDirectory(const std::wstring& root);

}  // namespace fl::mod
