#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace fl::sys {

// Linux stand-in for WinHTTP: a GET through the system's curl (proxy settings and CA certificates come from the
// system, as WinHTTP's do on Windows), following redirects. `onHeaders` gets the final status and headers (names
// lower-cased) before any body bytes; returning false from it stops. False with `error` set when curl can't reach
// the server, the transfer breaks or `cancel` is set.
struct HttpHead {
    int status = 0;
    std::map<std::string, std::string> headers;
};
bool curlGet(const std::wstring& url, const std::string& userAgent, const std::string& accept,
             const std::function<bool(const HttpHead&)>& onHeaders, const std::function<bool(const char*, size_t)>& sink,
             const std::atomic<bool>* cancel, std::wstring& error);

}  // namespace fl::sys
