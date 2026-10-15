#include "room_view.hpp"

#include <json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <thread>

namespace knockthru {
namespace {

using json = nlohmann::json;

// Finds the Room View button by its action signature. action_id changes when
// SteamVR restarts, so it cannot be used.
constexpr const char* kFindButton = R"JS(
  const roomViewAction = (button) => {
    const key = Object.keys(button).find(k => k.startsWith("__reactFiber"));
    for (let f = key && button[key], depth = 0; f && depth < 16; f = f.return, depth++) {
      const a = f.memoizedProps?.action;
      if (a?.invocation === 2 && a?.icon?.enum === 2 && a?.icon_active?.enum === 1) return a;
    }
  };
  const button = [...document.querySelectorAll('[role="button"]')].find(roomViewAction);
)JS";

class Socket {
public:
    explicit Socket(int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) throw std::runtime_error("Cannot create socket");
        timeval timeout{3, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) < 0)
            throw std::runtime_error("Steam debug port " + std::to_string(port) +
                " is closed. Turn on CEF Remote Debugging in the Frame's Developer settings.");
    }
    ~Socket() { ::close(fd_); }
    Socket(const Socket&) = delete;

    void send(const std::string& data) {
        for (std::size_t sent = 0; sent < data.size();) {
            const auto n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) throw std::runtime_error("Steam debug port write failed");
            sent += static_cast<std::size_t>(n);
        }
    }
    std::string receive(std::size_t count) {
        std::string data(count, '\0');
        for (std::size_t got = 0; got < count;) {
            const auto n = ::recv(fd_, data.data() + got, count - got, 0);
            if (n <= 0) throw std::runtime_error("Steam debug port closed the connection");
            got += static_cast<std::size_t>(n);
        }
        return data;
    }
    std::string receive_headers() {
        std::string data;
        while (data.find("\r\n\r\n") == std::string::npos) data += receive(1);
        return data;
    }

private:
    int fd_ = -1;
};

std::string websocket_frame(const std::string& text) {
    static std::mt19937 random{std::random_device{}()};
    std::string frame(1, static_cast<char>(0x81)); // final text frame
    const auto length = text.size();
    if (length < 126) {
        frame += static_cast<char>(0x80 | length);
    } else if (length < 65536) {
        frame += static_cast<char>(0xFE);
        frame += static_cast<char>(length >> 8);
        frame += static_cast<char>(length & 0xFF);
    } else {
        frame += static_cast<char>(0xFF);
        for (int shift = 56; shift >= 0; shift -= 8) frame += static_cast<char>((length >> shift) & 0xFF);
    }
    char mask[4];
    for (auto& byte : mask) byte = static_cast<char>(random());
    frame.append(mask, 4);
    for (std::size_t i = 0; i < length; ++i) frame += static_cast<char>(text[i] ^ mask[i % 4]);
    return frame;
}

std::string websocket_read(Socket& socket) {
    const auto head = socket.receive(2);
    std::uint64_t length = static_cast<unsigned char>(head[1]) & 0x7F;
    if (length == 126) {
        const auto bytes = socket.receive(2);
        length = (static_cast<unsigned char>(bytes[0]) << 8) | static_cast<unsigned char>(bytes[1]);
    } else if (length == 127) {
        const auto bytes = socket.receive(8);
        length = 0;
        for (const char byte : bytes) length = (length << 8) | static_cast<unsigned char>(byte);
    }
    if ((static_cast<unsigned char>(head[0]) & 0x0F) == 8) throw std::runtime_error("Steam debug port closed the connection");
    return socket.receive(length); // the server never masks its frames
}

std::string bar_websocket_path(int port) {
    Socket socket(port);
    socket.send("GET /json HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    // Steam keeps the connection open despite Connection: close, so read
    // exactly Content-Length bytes instead of waiting for the socket to close.
    auto headers = socket.receive_headers();
    for (auto& c : headers) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto field = headers.find("\r\ncontent-length:");
    if (field == std::string::npos) throw std::runtime_error("Steam debug port sent no target list");
    const auto length = std::stoul(headers.substr(field + 17));
    for (const auto& target : json::parse(socket.receive(length))) {
        const auto url = target.value("url", "");
        const std::string key = "vrOverlayKey=valve.steam.gamepadui.bar";
        const auto at = url.find(key);
        if (at == std::string::npos) continue;
        const auto end = at + key.size();
        if (end != url.size() && url[end] != '&') continue; // skip barpopup and others
        const auto ws = target.value("webSocketDebuggerUrl", "");
        const auto path = ws.find('/', ws.find("://") + 3);
        if (path != std::string::npos) return ws.substr(path);
    }
    throw std::runtime_error("SteamVR dashboard bar not found. Is SteamVR running?");
}

} // namespace

std::string RoomView::evaluate(const std::string& expression) const {
    const auto path = bar_websocket_path(port_);
    Socket socket(port_);
    // The key is fixed because only the local Steam client is on the other end.
    socket.send("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port_) +
                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Key: a25vY2t0aHJ1LWtub2NrdGhydQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    const auto headers = socket.receive_headers();
    if (headers.rfind("HTTP/1.1 101", 0) != 0) throw std::runtime_error("Steam debug port refused the connection");

    const json request = {{"id", 1}, {"method", "Runtime.evaluate"},
                          {"params", {{"expression", expression}, {"returnByValue", true}}}};
    socket.send(websocket_frame(request.dump()));
    for (;;) {
        const auto reply = json::parse(websocket_read(socket));
        if (reply.value("id", 0) != 1) continue;
        if (reply.contains("error")) throw std::runtime_error("Dashboard script failed: " + reply["error"].dump());
        const auto& result = reply["result"]["result"];
        if (result.value("subtype", "") == "error")
            throw std::runtime_error("Dashboard script failed: " + result.value("description", ""));
        return result.value("value", "");
    }
}

bool RoomView::active() const {
    const auto state = evaluate(std::string("(() => {") + kFindButton +
        "  if (!button) return 'missing';\n  return roomViewAction(button).active ? 'on' : 'off';\n})()");
    if (state == "missing") throw std::runtime_error("Room View button not found on the SteamVR dashboard");
    return state == "on";
}

bool RoomView::toggle() const {
    const bool before = active();
    const auto clicked = evaluate(std::string("(() => {") + kFindButton +
        "  if (!button) return 'missing';\n  button.click();\n  return 'clicked';\n})()");
    if (clicked != "clicked") throw std::runtime_error("Room View button not found on the SteamVR dashboard");
    // The button state updates after SteamVR applies the change.
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (active() != before) return !before;
    }
    throw std::runtime_error("Clicked Room View, but its state did not change");
}

} // namespace knockthru
