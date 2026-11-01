#include "lobby_cli.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "core/fs.h"
#include "core/json.h"
#include "core/lobby.h"
#include "core/log.h"
#include "core/net.h"
#include "core/rooms.h"
#include "core/strings.h"
#include "ui/launcher.h"

namespace fl {

namespace {

void emit(const json::Value& v) {
    std::string line = json::serialize(v, false) + "\n";
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n = 0;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
}

void emitError(const std::wstring& text) {
    json::Value o = json::Value::object();
    o.set("t", "error");
    o.set("message", narrow(text));
    emit(o);
}

const char* phaseId(lobby::Phase p) {
    switch (p) {
        case lobby::Phase::Connecting: return "connecting";
        case lobby::Phase::Lobby: return "lobby";
        case lobby::Phase::Starting: return "starting";
        case lobby::Phase::InGame: return "ingame";
        case lobby::Phase::Closed: return "closed";
    }
    return "closed";
}

// Why the room ended, in the window's words (the same texts the C++ lobby window shows).
std::wstring reasonText(const Config& cfg, const lobby::Snapshot& s) {
    using R = lobby::Reason;
    switch (s.reason) {
        case R::HostLeft: return cfg.t("lobby.r.hostLeft");
        case R::Kicked: return cfg.t("lobby.r.kicked");
        case R::Banned: return cfg.t("lobby.r.banned");
        case R::Full: return cfg.t("lobby.r.full");
        case R::InGame: return cfg.t("lobby.r.ingame");
        case R::Version: return cfg.t("lobby.r.version", {{L"HOST", widen(lobby::versionName(narrow(s.hostVersion)))}});
        case R::Password: return cfg.t("lobby.r.password");
        case R::TooManyTries: return cfg.t("lobby.r.tries");
        case R::NoRoom: return cfg.t("lobby.r.noRoom");
        case R::Unreachable: return cfg.t("lobby.r.unreachable", {{L"DETAIL", s.detail}});
        case R::Lost: return cfg.t("lobby.r.lost");
        case R::Server: return cfg.t("lobby.r.server", {{L"DETAIL", s.detail}});
        case R::None: break;
    }
    return {};
}

json::Value snapshotJson(const Config& cfg, const lobby::Snapshot& s) {
    json::Value o = json::Value::object();
    o.set("t", "snapshot");
    o.set("phase", phaseId(s.phase));
    o.set("direct", s.transport == lobby::Transport::Direct);
    o.set("host", s.host);
    o.set("reconnecting", s.reconnecting);
    o.set("locked", s.locked);
    o.set("isPublic", s.isPublic);
    o.set("code", narrow(s.code));
    o.set("roomName", narrow(s.roomName));
    o.set("maxPlayers", s.maxPlayers);
    o.set("mySlot", s.mySlot);
    o.set("gameConnected", s.gameConnected);
    o.set("serverV2", s.serverV2);
    o.set("reason", narrow(reasonText(cfg, s)));
    json::Value players = json::Value::array();
    for (const auto& p : s.players) {
        json::Value po = json::Value::object();
        po.set("slot", p.slot);
        po.set("name", narrow(p.name));
        po.set("host", p.host);
        po.set("ready", p.ready);
        po.set("connected", p.connected);
        po.set("loaded", p.loaded);
        po.set("you", p.you);
        po.set("ping", p.pingMs);
        players.push(std::move(po));
    }
    o.set("players", std::move(players));
    json::Value chat = json::Value::array();
    for (const auto& c : s.chat) {
        json::Value co = json::Value::object();
        co.set("slot", c.slot);
        co.set("name", narrow(c.name));
        co.set("text", narrow(c.text));
        chat.push(std::move(co));
    }
    o.set("chat", std::move(chat));
    return o;
}

std::string str(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    return v && v->isString() ? v->str() : std::string();
}
double num(const json::Value& o, const char* key, double def) {
    const json::Value* v = o.find(key);
    return v && v->isNumber() ? v->number() : def;
}
bool flag(const json::Value& o, const char* key, bool def) {
    const json::Value* v = o.find(key);
    return v && v->isBool() ? v->boolean() : def;
}

// Lines from standard input, read on their own thread so the room keeps running while nothing is typed.
struct Input {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> lines;
    bool closed = false;
};

}  // namespace

int runLobbySession(const Config& cfg, Library& lib, UserData& user, const std::wstring& userPath, const GameSpec& game) {
    if (!game.lobby.enabled || game.mod) {
        emitError(game.name + L" has no lobby.");
        return 3;
    }
    if (user.playerName.empty()) {
        emitError(L"Choose a player name in Settings first: everyone in the room sees it.");
        return 4;
    }
    size_t joinIndex = 0;
    for (size_t i = 0; i < game.actions.size(); ++i)
        if (game.actions[i].id == game.joinAction) joinIndex = i;
    const ActionSpec& joinAction = game.actions[joinIndex];
    ResolvedAction r = lib.resolve(game, joinAction, user);
    if (r.state == ActionState::NoSwf) {
        emitError(cfg.t("lobby.noGameFile"));
        return 4;
    }
    // "<lobby version>#<first 16 hex of the game file's SHA-256>": another build of the file can't join.
    std::string hash = !r.swf.empty() && !r.swfIsUrl ? fs::sha256File(r.swf) : std::string();
    std::wstring server = resolveRoomServer(cfg.rooms.servers, cfg.rooms.server, nullptr);
    if (server.empty()) server = cfg.rooms.server;

    auto input = std::make_shared<Input>();
    std::thread([input] {
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) {
            pending.append(buf, n);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::lock_guard<std::mutex> lock(input->mu);
                input->lines.push_back(std::move(line));
                input->cv.notify_all();
            }
        }
        std::lock_guard<std::mutex> lock(input->mu);
        input->closed = true;
        input->cv.notify_all();
    }).detach();

    auto next = [&](int waitMs, std::string& line) {
        std::unique_lock<std::mutex> lock(input->mu);
        input->cv.wait_for(lock, std::chrono::milliseconds(waitMs), [&] { return !input->lines.empty() || input->closed; });
        if (input->lines.empty()) return false;
        line = std::move(input->lines.front());
        input->lines.pop_front();
        return true;
    };

    // The first line: host or join.
    std::string first;
    while (!next(1000, first))
        if (input->closed) return 1;
    std::string perr;
    json::Value req = json::parse(first, &perr);
    lobby::Options o;
    o.gameId = game.onlineId;
    o.version = lobby::versionTag(narrow(game.lobby.version), hash);
    o.playerName = user.playerName;
    o.maxPlayers = std::max(2, game.maxPlayers);
    o.region = net::userRegion();
    std::string op = str(req, "op");
    bool direct = flag(req, "direct", false);
    o.transport = direct ? lobby::Transport::Direct : lobby::Transport::Online;
    o.password = widen(str(req, "password"));
    if (op == "host") {
        o.host = true;
        o.roomName = trim(widen(str(req, "roomName")));
        o.isPublic = flag(req, "public", true);
        o.maxPlayers = std::clamp(static_cast<int>(num(req, "maxPlayers", o.maxPlayers)), 2, 8);
        o.directPort = std::clamp(static_cast<int>(num(req, "port", game.lobby.port)), 1, 65535);
        if (!direct) o.server = server;
    } else if (op == "join") {
        if (direct) {
            std::wstring addr = trim(widen(str(req, "address")));
            if (addr.empty()) {
                emitError(cfg.t("lobby.badAddress"));
                return 3;
            }
            if (addr.find(L':') == std::wstring::npos) addr += L":" + std::to_wstring(game.lobby.port);
            o.server = addr;
        } else {
            std::wstring code;
            for (wchar_t ch : widen(str(req, "code")))
                if (iswalnum(ch)) code += static_cast<wchar_t>(towupper(ch));
            if (code.size() != 5) {
                emitError(cfg.t("lobby.badCode"));
                return 3;
            }
            o.code = code;
            o.server = server;
        }
    } else {
        emitError(L"The first line must host or join.");
        return 3;
    }

    lobby::Session session;
    HANDLE changed = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    session.setOnChange([changed] { SetEvent(changed); });
    std::wstring err;
    if (!session.start(o, err)) {
        emitError(cfg.t("lobby.r.unreachable", {{L"DETAIL", err}}));
        CloseHandle(changed);
        return 1;
    }
    uint64_t shown = ~0ull;
    HANDLE gameProcess = nullptr;
    bool leaving = false;
    for (;;) {
        // Commands first, then what changed.
        std::string line;
        while (next(0, line)) {
            json::Value c = json::parse(line, &perr);
            std::string cmd = str(c, "op");
            if (cmd == "ready") session.setReady(flag(c, "v", true));
            else if (cmd == "start") session.startGame();
            else if (cmd == "chat") session.sendChat(widen(str(c, "text")));
            else if (cmd == "kick") session.kick(static_cast<int>(num(c, "slot", -1)), flag(c, "ban", false));
            else if (cmd == "leave") leaving = true;
        }
        if (input->closed) leaving = true;  // the window went away
        if (leaving) break;
        lobby::Snapshot s = session.snapshot();
        if (s.revision != shown) {
            emit(snapshotJson(cfg, s));
            shown = s.revision;
        }
        if (s.startNow && !gameProcess) {
            // The room's values go to the game as FlashVars, as the C++ lobby window passes them.
            ui::Launcher::HeadlessOptions ho;
            ho.flashVars = {{L"room", s.code},
                            {L"role", s.host ? L"host" : L"client"},
                            {L"slot", std::to_wstring(s.mySlot)},
                            {L"bridgePort", std::to_wstring(s.bridgePort)},
                            {L"sessionToken", s.token},
                            {L"seed", std::to_wstring(s.seed)},
                            {L"lobby", L"1"}};
            ho.process = &gameProcess;
            std::wstring message;
            int code = ui::Launcher::launchHeadless(cfg, lib, user, userPath, game, joinAction, message, ho);
            json::Value g = json::Value::object();
            g.set("t", "game");
            g.set("state", code == 0 && gameProcess ? "started" : "failed");
            g.set("message", narrow(message));
            emit(g);
            if (code == 0 && gameProcess) session.gameLaunched();
            else session.gameExited();  // the host goes back to the lobby; a guest tells the host
        }
        if (gameProcess && WaitForSingleObject(gameProcess, 0) == WAIT_OBJECT_0) {
            CloseHandle(gameProcess);
            gameProcess = nullptr;
            session.gameExited();
            json::Value g = json::Value::object();
            g.set("t", "game");
            g.set("state", "exited");
            emit(g);
        }
        if (!session.running() || s.phase == lobby::Phase::Closed) break;
        WaitForSingleObject(changed, 250);
    }
    session.stop();
    lobby::Snapshot end = session.snapshot();
    json::Value c = json::Value::object();
    c.set("t", "closed");
    c.set("reason", narrow(reasonText(cfg, end)));
    emit(c);
    if (gameProcess) CloseHandle(gameProcess);
    CloseHandle(changed);
    return 0;
}

}  // namespace fl
