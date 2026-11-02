#include <cstring>

#include "core/log.h"
#include "core/strings.h"
#include "core/win.h"
#include "test.h"
// win.h first.
#include <shellapi.h>
#include <winsock2.h>

int g_checkFailures = 0;

std::vector<TestCase>& testRegistry() {
    static std::vector<TestCase> r;
    return r;
}

TempDir::TempDir(const std::string& name) {
    static int counter = 0;
    path_ = fl::fs::join(fl::fs::tempDir(), L"fl-tests\\" + fl::widen(name) + L"-" +
                                                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter));
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

void writeText(const std::wstring& path, const std::string& text) {
    fl::fs::ensureDir(fl::fs::parent(path), nullptr, nullptr);
    fl::fs::writeFile(path, text.data(), text.size(), nullptr);
}

std::string readText(const std::wstring& path) {
    std::string s;
    fl::fs::readText(path, s);
    return s;
}

bool exists(const std::wstring& path) { return fl::fs::exists(path); }

static void appendOut(const std::string& text) {
    wchar_t out[1024] = L"";
    GetEnvironmentVariableW(L"FL_FAKE_OUT", out, 1024);
    if (!*out) return;
    FILE* f = _wfopen(out, L"ab");
    if (f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

// A game on the launcher's lobby bridge: hello with the session token, "loaded"
// once welcomed, then it notes what arrives ("BRIDGE {welcome...}", "BRIDGE go",
// "BRIDGE {msg...}") until `untilTick`.
static void fakeBridgeGame(int port, const std::string& token, ULONGLONG untilTick) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        appendOut("BRIDGE connect failed\n");
        return;
    }
    DWORD timeout = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    auto sendMsg = [&](const std::string& m) {
        std::string d = m + std::string(1, '\0');
        send(s, d.data(), static_cast<int>(d.size()), 0);
    };
    sendMsg("{\"t\":\"hello\",\"token\":\"" + token + "\"}");
    std::string buf;
    while (GetTickCount64() < untilTick) {
        char tmp[4096];
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n == 0) break;
        if (n > 0) buf.append(tmp, n);
        size_t nul;
        while ((nul = buf.find('\0')) != std::string::npos) {
            std::string m = buf.substr(0, nul);
            buf.erase(0, nul + 1);
            appendOut("BRIDGE " + m + "\n");
            if (m.find("\"t\":\"welcome\"") != std::string::npos) sendMsg("{\"t\":\"loaded\"}");
            if (m.find("\"t\":\"go\"") != std::string::npos)
                sendMsg("{\"t\":\"send\",\"to\":-1,\"d\":\"hello from the game\"}");
        }
    }
    closesocket(s);
    WSACleanup();
}

// Stand-in for a Flash player in tests\e2e.ps1: appends its arguments (one per
// line) to %FL_FAKE_OUT%, stays open for %FL_FAKE_MS% milliseconds, exits 0.
// Each variable named in %FL_FAKE_ENV% (separated by ';') it was started with adds an "ENV NAME=value" line.
// Given the lobby's FlashVars (-PbridgePort=, -PsessionToken=) it also plays the
// game's side of the bridge meanwhile.
static int fakePlayer() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string lines, token;
    int bridgePort = 0;
    for (int i = 2; i < argc; ++i) {
        std::string arg = fl::narrow(argv[i]);
        lines += arg + "\n";
        if (arg.rfind("-PbridgePort=", 0) == 0) bridgePort = atoi(arg.c_str() + 13);
        if (arg.rfind("-PsessionToken=", 0) == 0) token = arg.substr(15);
    }
    LocalFree(argv);
    wchar_t names[512] = L"";
    GetEnvironmentVariableW(L"FL_FAKE_ENV", names, 512);
    for (const auto& name : fl::split(names, L';')) {
        wchar_t value[512] = L"";
        if (!name.empty() && GetEnvironmentVariableW(name.c_str(), value, 512)) lines += "ENV " + fl::narrow(name + L"=" + value) + "\n";
    }
    wchar_t ms[32] = L"";
    GetEnvironmentVariableW(L"FL_FAKE_MS", ms, 32);
    appendOut(lines);
    ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(_wtoi(ms));
    if (bridgePort) fakeBridgeGame(bridgePort, token, until);
    ULONGLONG now = GetTickCount64();
    if (now < until) Sleep(static_cast<DWORD>(until - now));
    return 0;
}

// Stand-in for an HTML5 game's server (tests\e2e.ps1): prints a tunnel-style
// link the way the PB2 co-op server does, then answers every HTTP request with
// "ok" until it is stopped.
static int fakeServer(int port) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (bind(l, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(l, 16) != 0) return 1;
    printf("Fake server running.\n  Internet:  https://fake-test-link.trycloudflare.com   <- send this\n");
    printf("  Same Wi-Fi/LAN:   http://10.1.2.3:%d/\n", port);
    fflush(stdout);
    for (;;) {
        SOCKET c = accept(l, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        char buf[2048];
        recv(c, buf, sizeof buf, 0);
        const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        send(c, reply, sizeof reply - 1, 0);
        closesocket(c);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--fake-player") == 0) return fakePlayer();
    if (argc > 2 && strcmp(argv[1], "--fake-server") == 0) return fakeServer(atoi(argv[2]));
    const char* filter = argc > 1 ? argv[1] : nullptr;
    TempDir logs("logs");
    fl::logger().open(logs / L"tests.log");
    fl::logger().setLevel(fl::LogLevel::Debug);
    int failed = 0, ran = 0;
    for (const auto& t : testRegistry()) {
        if (filter && !strstr(t.name, filter)) continue;
        ++ran;
        int before = g_checkFailures;
        bool threw = false;
        try {
            t.fn();
        } catch (const std::exception& e) {
            threw = true;
            fprintf(stderr, "    exception: %s\n", e.what());
        }
        bool ok = !threw && g_checkFailures == before;
        printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", t.name);
        fflush(stdout);
        if (!ok) ++failed;
    }
    printf("\n%d of %d tests passed\n", ran - failed, ran);
    if (failed) {
        fl::logger().close();
        printf("log: %ls\n", (logs / L"tests.log").c_str());
    }
    return failed ? 1 : 0;
}
