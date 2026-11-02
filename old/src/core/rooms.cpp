#include "rooms.h"

#include <mutex>
#include <set>

#include "install.h"
#include "json.h"
#include "log.h"
#include "strings.h"
#include "win.h"
// win.h first (WIN32_LEAN_AND_MEAN keeps the old winsock.h out).
#include <winsock2.h>
#include <ws2tcpip.h>

namespace fl {

namespace {

// Servers that answered RELAY|2 with "unsupported": asked with RELAY|1 from then on.
std::mutex g_oldMu;
std::set<std::wstring> g_oldServers;

bool olderServerReply(const std::string& reply) {
    return reply.rfind("E|", 0) == 0 &&
           (reply.find("RELAY|2") != std::string::npos || reply.find("handshake version") != std::string::npos ||
            reply.find("Bad handshake") != std::string::npos);
}

}  // namespace

bool parseRoomList(std::string_view reply, std::vector<RoomInfo>& out) {
    out.clear();
    bool v2 = reply.rfind("ROOMS|", 0) == 0;
    if (!v2 && reply.rfind("LIST|", 0) != 0) return false;
    std::string body(reply.substr(v2 ? 6 : 5));
    size_t fields = v2 ? 8 : 3;  // before the name
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find(';', start);
        std::string row = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? body.size() : end + 1;
        std::vector<std::string> f;
        size_t at = 0;
        while (f.size() < fields) {
            size_t comma = row.find(',', at);
            if (comma == std::string::npos) break;
            f.push_back(row.substr(at, comma - at));
            at = comma + 1;
        }
        if (f.size() < fields) continue;
        RoomInfo r;
        r.code = widen(f[0]);
        r.players = atoi(f[1].c_str());
        r.max = atoi(f[2].c_str());
        if (v2) {
            r.locked = f[3] == "1";
            r.state = widen(f[4]);
            r.region = widen(f[5]);
            r.hostPing = f[6].empty() ? -1 : atoi(f[6].c_str());
            r.version = widen(f[7]);
        }
        r.name = widen(row.substr(at));
        if (!r.code.empty()) out.push_back(std::move(r));
    }
    return true;
}

bool askServer(const std::wstring& server, const std::string& request, std::string& reply, std::wstring& error,
               int timeoutMs) {
    reply.clear();
    size_t colon = server.rfind(L':');
    if (colon == std::wstring::npos) {
        error = L"Server address needs a port: " + server;
        return false;
    }
    std::string host = narrow(server.substr(0, colon)), port = narrow(server.substr(colon + 1));
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = L"Networking is not available";
        return false;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    bool answered = false;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        error = L"Cannot find the server " + server;
    } else {
        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        DWORD t = static_cast<DWORD>(timeoutMs);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
        // A non-blocking connect with select() bounds the wait when the server is down.
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &w, nullptr, &tv) == 1) {
            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
            std::string req = request + std::string(1, '\0');
            send(s, req.data(), static_cast<int>(req.size()), 0);
            char buf[4096];
            for (;;) {
                int n = recv(s, buf, sizeof buf, 0);
                if (n <= 0) break;
                reply.append(buf, n);
                size_t nul = reply.find('\0');
                if (nul != std::string::npos) {
                    reply.resize(nul);
                    break;
                }
                if (reply.size() > 256 * 1024) break;
            }
            answered = !reply.empty();
            if (!answered) error = L"No answer from " + server;
        } else {
            error = L"Cannot reach " + server;
        }
        closesocket(s);
        freeaddrinfo(res);
    }
    WSACleanup();
    return answered;
}

bool listRooms(const std::wstring& server, const std::string& gameId, std::vector<RoomInfo>& out, std::wstring& error,
               int timeoutMs, bool* rejected) {
    if (rejected) *rejected = false;
    out.clear();
    bool old;
    {
        std::lock_guard<std::mutex> lock(g_oldMu);
        old = g_oldServers.count(server) > 0;
    }
    std::string reply;
    if (!askServer(server, (old ? "RELAY|1|LIST|" : "RELAY|2|ROOMS|") + gameId, reply, error, timeoutMs)) return false;
    if (!old && olderServerReply(reply)) {
        {
            std::lock_guard<std::mutex> lock(g_oldMu);
            g_oldServers.insert(server);
        }
        if (!askServer(server, "RELAY|1|LIST|" + gameId, reply, error, timeoutMs)) return false;
    }
    if (parseRoomList(reply, out)) return true;
    if (reply.rfind("E|", 0) == 0) {
        if (rejected) *rejected = true;
        error = widen(reply.substr(2));
    } else {
        error = L"Unexpected answer from " + server;
    }
    return false;
}

std::string probePort(const std::wstring& server, const std::string& gameId, int port, std::wstring& error) {
    std::string reply;
    if (!askServer(server, "RELAY|2|PROBE|" + gameId + "|" + std::to_string(port), reply, error, 12000)) return "";
    if (reply.rfind("PROBE|", 0) != 0) {
        error = reply.rfind("E|", 0) == 0 ? widen(reply.substr(2)) : L"The server can't test ports";
        return "";
    }
    std::string rest = reply.substr(6);
    std::string result = rest.substr(0, rest.find('|'));
    if (result == "error") {
        size_t bar = rest.find('|');
        error = bar == std::string::npos ? L"" : widen(rest.substr(bar + 1));
        return "";
    }
    return result;
}

std::wstring resolveRoomServer(const std::wstring& serversUrl, const std::wstring& fallback, std::wstring* error) {
    if (!serversUrl.empty()) {
        std::string text;
        std::wstring err;
        if (httpGetText(serversUrl, text, err)) {
            std::string perr;
            json::Value root = json::parse(text, &perr);
            const json::Value* list = root.isObject() ? root.find("servers") : nullptr;
            if (list && list->isArray())
                for (size_t i = 0; i < list->size(); ++i) {
                    const json::Value& sv = list->at(i);
                    const json::Value* addr = sv.find("address");
                    const json::Value* status = sv.find("status");
                    if (addr && addr->isString() && (!status || !status->isString() || status->str() == "online"))
                        return widen(addr->str());
                }
        } else if (error) {
            *error = err;
        }
    }
    return fallback;
}

}  // namespace fl
