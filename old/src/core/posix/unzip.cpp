#include "core/posix/unzip.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "core/fs.h"
#include "core/process.h"
#include "core/strings.h"
#include "core/win.h"

namespace fl::sys {

namespace {

struct File {
    FILE* f = nullptr;
    ~File() {
        if (f) fclose(f);
    }
};

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return u16(p) | static_cast<uint32_t>(u16(p + 2)) << 16; }
uint64_t u64(const uint8_t* p) { return u32(p) | static_cast<uint64_t>(u32(p + 4)) << 32; }

bool readAt(FILE* f, uint64_t at, void* buf, size_t n) {
    return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

struct Entry {
    std::string name;
    uint16_t method = 0, madeBy = 0, flags = 0;
    uint32_t crc = 0, external = 0;
    uint64_t packed = 0, size = 0, local = 0;
};

// The central directory, from the end record (and its Zip64 twin when the archive is that big).
bool readDirectory(FILE* f, std::vector<Entry>& out, std::wstring& error) {
    if (fseeko(f, 0, SEEK_END) != 0) return false;
    uint64_t fileSize = static_cast<uint64_t>(ftello(f));
    size_t tail = static_cast<size_t>(std::min<uint64_t>(fileSize, 65557));
    std::vector<uint8_t> buf(tail);
    if (tail < 22 || !readAt(f, fileSize - tail, buf.data(), tail)) {
        error = L"The download isn't a complete zip file";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = tail - 22 + 1; i-- > 0;)
        if (u32(&buf[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) {
        error = L"The download isn't a complete zip file";
        return false;
    }
    uint64_t count = u16(&buf[eocd + 10]), dirSize = u32(&buf[eocd + 12]), dirAt = u32(&buf[eocd + 16]);
    if ((count == 0xFFFF || dirAt == 0xFFFFFFFF) && eocd >= 20 && u32(&buf[eocd - 20]) == 0x07064b50) {
        uint8_t rec[56];
        if (!readAt(f, u64(&buf[eocd - 20 + 8]), rec, sizeof rec) || u32(rec) != 0x06064b50) {
            error = L"The zip file's index is damaged";
            return false;
        }
        count = u64(rec + 32);
        dirSize = u64(rec + 40);
        dirAt = u64(rec + 48);
    }
    if (dirAt + dirSize > fileSize || dirSize > (1u << 30)) {
        error = L"The zip file's index is damaged";
        return false;
    }
    std::vector<uint8_t> dir(static_cast<size_t>(dirSize));
    if (!readAt(f, dirAt, dir.data(), dir.size())) return false;
    size_t p = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (p + 46 > dir.size() || u32(&dir[p]) != 0x02014b50) {
            error = L"The zip file's index is damaged";
            return false;
        }
        const uint8_t* h = &dir[p];
        Entry e;
        e.madeBy = u16(h + 4);
        e.flags = u16(h + 8);
        e.method = u16(h + 10);
        e.crc = u32(h + 16);
        e.packed = u32(h + 20);
        e.size = u32(h + 24);
        size_t nameLen = u16(h + 28), extraLen = u16(h + 30), commentLen = u16(h + 32);
        e.external = u32(h + 38);
        e.local = u32(h + 42);
        if (p + 46 + nameLen + extraLen + commentLen > dir.size()) {
            error = L"The zip file's index is damaged";
            return false;
        }
        e.name.assign(reinterpret_cast<const char*>(h + 46), nameLen);
        // Zip64: the sizes and offset that didn't fit, in this order.
        for (size_t x = 0; x + 4 <= extraLen;) {
            const uint8_t* ex = h + 46 + nameLen + x;
            uint16_t id = u16(ex), len = u16(ex + 2);
            if (x + 4 + len > extraLen) break;
            if (id == 1) {
                const uint8_t* v = ex + 4;
                const uint8_t* end = v + len;
                if (e.size == 0xFFFFFFFF && v + 8 <= end) e.size = u64(v), v += 8;
                if (e.packed == 0xFFFFFFFF && v + 8 <= end) e.packed = u64(v), v += 8;
                if (e.local == 0xFFFFFFFF && v + 8 <= end) e.local = u64(v), v += 8;
            }
            x += 4 + len;
        }
        out.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

// A safe relative path inside the archive, with '/' separators; empty when it isn't one.
std::string safeName(std::string name) {
    for (char& c : name)
        if (c == '\\') c = '/';
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return {};
    std::string out;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        std::string part = name.substr(start, end - start);
        if (part == "..") return {};
        if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
        start = end + 1;
    }
    return out;
}

bool makeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        std::string part = path.substr(0, i);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool extract(FILE* f, const Entry& e, const std::string& path, std::wstring& error) {
    uint8_t local[30];
    if (!readAt(f, e.local, local, sizeof local) || u32(local) != 0x04034b50) {
        error = L"The zip file is damaged";
        return false;
    }
    uint64_t dataAt = e.local + 30 + u16(local + 26) + u16(local + 28);
    if (fseeko(f, static_cast<off_t>(dataAt), SEEK_SET) != 0) return false;
    if (e.flags & 1) {
        error = L"The zip file is password-protected";
        return false;
    }
    if (e.method != 0 && e.method != 8) {
        error = L"The zip file uses a compression this launcher can't unpack (method " + std::to_wstring(e.method) + L")";
        return false;
    }
    mode_t mode = 0644;
    if ((e.madeBy >> 8) == 3 && (e.external >> 16) & 0111) mode = 0755;  // made on Unix with an x bit
    int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        error = L"Cannot write " + widen(path) + L": " + win32ErrorMessage(flWin32FromErrno(errno));
        return false;
    }
    std::vector<uint8_t> in(256 * 1024), buf(256 * 1024);
    uint64_t left = e.packed, written = 0;
    uLong crc = crc32(0, nullptr, 0);
    bool ok = true;
    auto put = [&](const uint8_t* data, size_t n) {
        crc = crc32(crc, data, static_cast<uInt>(n));
        written += n;
        for (size_t done = 0; done < n;) {
            ssize_t w = write(out, data + done, n - done);
            if (w <= 0) return false;
            done += static_cast<size_t>(w);
        }
        return true;
    };
    if (e.method == 0) {
        while (ok && left) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(left, in.size()));
            ok = fread(in.data(), 1, n, f) == n && put(in.data(), n);
            left -= n;
        }
    } else {
        z_stream z{};
        ok = inflateInit2(&z, -MAX_WBITS) == Z_OK;
        int rc = Z_OK;
        while (ok && rc != Z_STREAM_END) {
            if (z.avail_in == 0) {
                if (!left) {
                    ok = false;
                    break;
                }
                size_t n = static_cast<size_t>(std::min<uint64_t>(left, in.size()));
                if (fread(in.data(), 1, n, f) != n) {
                    ok = false;
                    break;
                }
                left -= n;
                z.next_in = in.data();
                z.avail_in = static_cast<uInt>(n);
            }
            z.next_out = buf.data();
            z.avail_out = static_cast<uInt>(buf.size());
            rc = inflate(&z, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) ok = false;
            else ok = put(buf.data(), buf.size() - z.avail_out);
        }
        inflateEnd(&z);
    }
    close(out);
    if (ok && (written != e.size || crc != e.crc)) ok = false;
    if (!ok) {
        if (error.empty()) error = L"The zip file is damaged (" + widen(e.name) + L")";
        unlink(path.c_str());
    }
    return ok;
}

bool unzip(const std::string& archive, const std::string& dest, std::wstring& error) {
    File f{fopen(archive.c_str(), "rb")};
    if (!f.f) {
        error = win32ErrorMessage(flWin32FromErrno(errno));
        return false;
    }
    std::vector<Entry> entries;
    if (!readDirectory(f.f, entries, error)) {
        if (error.empty()) error = L"The zip file is damaged";
        return false;
    }
    for (const Entry& e : entries) {
        std::string rel = safeName(e.name);
        if (rel.empty()) {
            if (e.name.find_first_not_of("./\\") != std::string::npos) {
                error = L"The zip file has a path outside its folder (" + widen(e.name) + L")";
                return false;
            }
            continue;  // "./" or the like: nothing to create
        }
        // A symbolic link stored by a Unix zip: refused, as on Windows.
        if ((e.madeBy >> 8) == 3 && ((e.external >> 16) & 0170000) == 0120000) {
            error = L"The zip file contains a link (" + widen(e.name) + L")";
            return false;
        }
        std::string path = dest + "/" + rel;
        bool isDir = e.name.back() == '/' || e.name.back() == '\\';
        if (isDir) {
            if (!makeDirs(path)) {
                error = L"Cannot create " + widen(path);
                return false;
            }
            continue;
        }
        size_t slash = path.rfind('/');
        if (!makeDirs(path.substr(0, slash))) {
            error = L"Cannot create " + widen(path.substr(0, slash));
            return false;
        }
        if (!extract(f.f, e, path, error)) return false;
    }
    return true;
}

bool isZip(const std::string& path) {
    File f{fopen(path.c_str(), "rb")};
    uint8_t head[4];
    return f.f && fread(head, 1, 4, f.f) == 4 && u32(head) == 0x04034b50;
}

}  // namespace

bool unpackArchive(const std::wstring& archive, const std::wstring& destDir, std::wstring& error) {
    std::string a = flOsPath(archive), d = flOsPath(destDir);
    if (isZip(a)) return unzip(a, d, error);
    Started tar;
    if (!startProcess(L"/usr/bin/tar", {L"-xf", widen(a), L"-C", widen(d)}, destDir, tar, error, true)) return false;
    WaitForSingleObject(tar.process, 180000);
    DWORD code = 1;
    GetExitCodeProcess(tar.process, &code);
    CloseHandle(tar.process);
    if (code != 0) error = L"tar couldn't unpack it (exit code " + std::to_wstring(code) + L")";
    return code == 0;
}

}  // namespace fl::sys
