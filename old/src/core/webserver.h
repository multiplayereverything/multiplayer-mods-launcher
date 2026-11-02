// A tiny static web server for HTML5 games that are just a folder of files.
// Browsers block much of what games do from file:// pages (loading their own
// data, WebAssembly, audio), so the launcher serves the folder on 127.0.0.1
// while the game runs. GET and HEAD only, byte ranges for media, nothing
// outside the folder.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fl {

class WebServer {
public:
    WebServer() = default;
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Serves `root` on 127.0.0.1:`port` (0 = any free port). A fixed port per
    // game keeps its saves: browsers file local storage by address.
    bool start(const std::wstring& root, int port, std::wstring& error);
    void stop();
    int port() const { return port_; }

private:
    void loop();
    void serve(uintptr_t client);

    std::wstring root_;
    int port_ = 0;
    uintptr_t listener_ = ~uintptr_t(0);
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::thread> workers_;
};

// MIME type from a file name ("game.wasm" -> "application/wasm").
std::string mimeType(const std::wstring& fileName);

}  // namespace fl
