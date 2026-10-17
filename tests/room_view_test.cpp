// Room View tests: RoomView talks to a fake Steam debug port, which runs each
// script in Node against a fake dashboard bar. Skips with exit code 77 when
// node is not on PATH.
#include "check.hpp"
#include "room_view.hpp"

#include <json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using namespace knockthru;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// The fake bar. Buttons find their React action through __reactFiber, as on the Frame.
// The state file carries whether Room View is on between scripts.
constexpr const char* kPrelude = R"JS(
const fs = require('fs');
const statePath = process.argv[2];
const state = JSON.parse(fs.readFileSync(statePath, 'utf8'));
const button = (action, onClick) => {
  const element = {click: onClick};
  element['__reactFiber$test'] = {memoizedProps: {}, return: {memoizedProps: {action}, return: null}};
  return element;
};
const buttons = [button({invocation: 2, icon: {enum: 3}, icon_active: {enum: 1}, active: true}, () => {})];
if (state.present)
  buttons.push(button({invocation: 2, icon: {enum: 2}, icon_active: {enum: 1}, active: state.on},
                      () => { state.on = !state.on; }));
globalThis.document = {querySelectorAll: (selector) => {
  if (state.broken) throw new Error('querySelectorAll is gone');
  return selector === '[role="button"]' ? buttons : [];
}};
let value;
try { value = (EXPRESSION); } finally { fs.writeFileSync(statePath, JSON.stringify(state)); }
process.stdout.write(JSON.stringify(value));
)JS";

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// A Steam client debug port on 127.0.0.1 with two targets, the bar and its popup.
class FakeDebugPort {
public:
    explicit FakeDebugPort(fs::path directory) : directory_(std::move(directory)) {
        set_state(false, true, false);
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address);
        ::listen(listener_, 4);
        socklen_t size = sizeof address;
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        timeval timeout{0, 100000}; // accept wakes up to see stop_
        ::setsockopt(listener_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        thread_ = std::thread([this] { loop(); });
    }
    ~FakeDebugPort() {
        stop_ = true;
        thread_.join();
        ::close(listener_);
    }

    int port() const { return port_; }
    bool bar = true; // whether the target list has the bar page

    void set_state(bool on, bool present, bool broken) {
        std::ofstream(directory_ / "state.json", std::ios::trunc)
            << json{{"on", on}, {"present", present}, {"broken", broken}}.dump();
    }
    bool on() const { return json::parse(read(directory_ / "state.json")).value("on", false); }

private:
    void loop() {
        while (!stop_) {
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) continue;
            answer(client);
            ::close(client);
        }
    }

    static std::string receive(int client, std::size_t count) {
        std::string data(count, '\0');
        for (std::size_t got = 0; got < count;) {
            const auto n = ::recv(client, data.data() + got, count - got, 0);
            if (n <= 0) return {};
            got += static_cast<std::size_t>(n);
        }
        return data;
    }
    static void send_all(int client, const std::string& data) { ::send(client, data.data(), data.size(), MSG_NOSIGNAL); }

    // An unmasked text frame, as the Steam client sends.
    static std::string frame(const std::string& text) {
        std::string out(1, static_cast<char>(0x81));
        if (text.size() < 126) {
            out += static_cast<char>(text.size());
        } else {
            out += static_cast<char>(126);
            out += static_cast<char>(text.size() >> 8);
            out += static_cast<char>(text.size() & 0xFF);
        }
        return out + text;
    }

    // Reads one masked client frame.
    static std::string read_frame(int client) {
        const auto head = receive(client, 2);
        if (head.size() != 2) return {};
        std::size_t length = static_cast<unsigned char>(head[1]) & 0x7F;
        if (length == 126) {
            const auto bytes = receive(client, 2);
            length = (static_cast<unsigned char>(bytes[0]) << 8) | static_cast<unsigned char>(bytes[1]);
        }
        const auto mask = receive(client, 4);
        auto payload = receive(client, length);
        for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        return payload;
    }

    void answer(int client) {
        std::string headers;
        while (headers.find("\r\n\r\n") == std::string::npos) {
            const auto byte = receive(client, 1);
            if (byte.empty()) return;
            headers += byte;
        }
        const auto ws = "ws://127.0.0.1:" + std::to_string(port_) + "/devtools/page/";
        if (headers.rfind("GET /json ", 0) == 0) {
            json targets = json::array();
            targets.push_back({{"url", "https://steamloopback.host/?vrOverlayKey=valve.steam.gamepadui.barpopup"},
                               {"webSocketDebuggerUrl", ws + "POPUP"}});
            if (bar)
                targets.push_back({{"url", "https://steamloopback.host/?vrOverlayKey=valve.steam.gamepadui.bar"},
                                   {"webSocketDebuggerUrl", ws + "BAR"}});
            const auto body = targets.dump();
            // The connection stays open, as Steam's does, so the client must read by length.
            send_all(client, "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
            return;
        }
        if (headers.rfind("GET /devtools/page/BAR ", 0) != 0) {
            send_all(client, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        send_all(client, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n");
        const auto request = json::parse(read_frame(client), nullptr, false);
        if (request.is_discarded()) return;
        // An event comes first, longer than 125 bytes, which the client must skip.
        send_all(client, frame(json{{"method", "Runtime.consoleAPICalled"}, {"params", {{"pad", std::string(200, 'x')}}}}.dump()));
        send_all(client, frame(evaluate(request["id"], request["params"]["expression"])));
    }

    // Runs `expression` in Node and returns the reply Runtime.evaluate gives.
    std::string evaluate(const json& id, const std::string& expression) {
        std::string script = kPrelude;
        script.replace(script.find("EXPRESSION"), 10, expression);
        std::ofstream(directory_ / "script.js", std::ios::trunc) << script;
        const auto command = "node '" + (directory_ / "script.js").string() + "' '" +
                             (directory_ / "state.json").string() + "' 2>/dev/null";
        std::string output;
        int status = -1;
        if (FILE* pipe = ::popen(command.c_str(), "r")) {
            char buffer[256];
            for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0;) output.append(buffer, n);
            status = ::pclose(pipe);
        }
        json result;
        if (status == 0) result = {{"type", "string"}, {"value", json::parse(output)}};
        else result = {{"type", "object"}, {"subtype", "error"}, {"description", "Error: script threw"}};
        return json{{"id", id}, {"result", {{"result", result}}}}.dump();
    }

    fs::path directory_;
    int listener_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// The message RoomView threw, and whether it was DashboardChanged.
template <typename F> std::pair<std::string, bool> error_of(F f) {
    try {
        f();
    } catch (const DashboardChanged& error) {
        return {error.what(), true};
    } catch (const std::exception& error) {
        return {error.what(), false};
    }
    return {"", false};
}

} // namespace

int main() {
    if (std::system("node --version >/dev/null 2>&1") != 0) {
        std::cout << "node is not on PATH, so the Room View tests are skipped\n";
        return 77;
    }
    const auto root = fs::temp_directory_path() / ("knockthru-room-view-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    {
        FakeDebugPort steam(root);
        const RoomView room_view(steam.port());

        check(room_view.active() == false, "active reads off from the bar, skipping the popup page");
        steam.set_state(true, true, false);
        check(room_view.active() == true, "active reads on");

        steam.set_state(false, true, false);
        check(room_view.toggle() == true && steam.on(), "toggle clicks the Room View button and returns on");
        check(room_view.toggle() == false && !steam.on(), "toggle again returns off");

        steam.set_state(false, false, false);
        const auto [missing, missing_changed] = error_of([&] { room_view.active(); });
        check(missing_changed && missing.find("not found") != std::string::npos,
              "a bar without the button throws DashboardChanged: " + missing);
        const auto [click, click_changed] = error_of([&] { room_view.toggle(); });
        check(click_changed, "toggle without the button throws DashboardChanged");

        steam.set_state(false, true, true);
        const auto [broken, broken_changed] = error_of([&] { room_view.active(); });
        check(broken_changed && broken.find("Dashboard script failed") != std::string::npos,
              "a script error throws DashboardChanged: " + broken);

        steam.set_state(false, true, false);
        steam.bar = false;
        const auto [no_bar, no_bar_changed] = error_of([&] { room_view.active(); });
        check(!no_bar_changed && no_bar.find("dashboard bar not found") != std::string::npos,
              "a target list without the bar says SteamVR may not be running: " + no_bar);
    }
    // The fake port is closed now.
    {
        int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof address);
        socklen_t size = sizeof address;
        ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size);
        const int closed_port = ntohs(address.sin_port);
        ::close(probe); // nothing listens on this port
        const auto [closed, closed_changed] = error_of([&] { RoomView(closed_port).active(); });
        check(!closed_changed && closed.find("CEF Remote Debugging") != std::string::npos,
              "a closed port says to turn on CEF Remote Debugging: " + closed);
    }
    fs::remove_all(root);
    return finish();
}
