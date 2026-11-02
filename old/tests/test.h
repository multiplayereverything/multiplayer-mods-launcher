// Tiny self-contained test framework plus fixtures shared by the test files.
#pragma once
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/fs.h"
#include "core/strings.h"

struct TestCase {
    const char* name;
    void (*fn)();
};
std::vector<TestCase>& testRegistry();
extern int g_checkFailures;

struct TestRegistrar {
    TestRegistrar(const char* n, void (*f)()) { testRegistry().push_back({n, f}); }
};

#define TEST(name)                                       \
    static void name();                                  \
    static TestRegistrar registrar_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_checkFailures;                                                      \
            fprintf(stderr, "    %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define REQUIRE(cond)                                                                 \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "    %s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond); \
            throw std::runtime_error("requirement failed");                           \
        }                                                                             \
    } while (0)

// A unique scratch folder removed when the test ends.
class TempDir {
public:
    explicit TempDir(const std::string& name);
    ~TempDir();
    const std::wstring& path() const { return path_; }
    std::wstring operator/(const std::wstring& rel) const { return fl::fs::join(path_, rel); }

private:
    std::wstring path_;
};

void writeText(const std::wstring& path, const std::string& text);
std::string readText(const std::wstring& path);
bool exists(const std::wstring& path);
