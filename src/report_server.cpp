#include "report_server.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace knockthru {
namespace {

std::string html_escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// The interface that carries the default route, from /proc/net/route.
std::string default_interface() {
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line); // the header
    while (std::getline(routes, line)) {
        std::istringstream fields(line);
        std::string name, destination;
        if (fields >> name >> destination && destination == "00000000") return name;
    }
    return "";
}

void send_all(int fd, const std::string& data) {
    for (std::size_t sent = 0; sent < data.size();) {
        const auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}

std::string response(const std::string& status, const std::string& type, const std::string& body,
                     const std::string& extra = "") {
    return "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra + "\r\n" + body;
}

} // namespace

std::string local_address() {
    const auto preferred = default_interface();
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return "";
    std::string found, fallback;
    for (auto* item = list; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) continue;
        if ((item->ifa_flags & IFF_LOOPBACK) || !(item->ifa_flags & IFF_UP)) continue;
        char text[INET_ADDRSTRLEN];
        const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        if (!::inet_ntop(AF_INET, &address->sin_addr, text, sizeof text)) continue;
        if (item->ifa_name == preferred) found = text;
        else if (fallback.empty()) fallback = text;
    }
    ::freeifaddrs(list);
    return !found.empty() ? found : preferred.empty() ? fallback : "";
}

std::string report_page(const std::string& report, const std::string& issue_link, const std::string& download) {
    return R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>KnockThru bug report</title>
<style>
/* The panel's colors. The button blue is darker than the panel accent, so white text passes WCAG AA. */
:root{--bg:#121418;--surface:#1c1f25;--line:#2e323a;--text:#eceef1;--dim:#969ca8;--accent:#2a78c8;--link:#50a0f0}
@media (prefers-color-scheme: light){
 :root{--bg:#f4f5f7;--surface:#ffffff;--line:#d5d9e0;--text:#16181c;--dim:#545b68;--link:#1f66b0}
}
body{margin:0;padding:24px 16px;background:var(--bg);color:var(--text);font:18px/1.5 system-ui,sans-serif}
main{max-width:560px;margin:0 auto}
h1{font-size:26px;margin:0 0 8px}
p{margin:0 0 16px;color:var(--dim)}
button{display:block;width:100%;box-sizing:border-box;padding:18px;border:0;border-radius:16px;
 background:var(--accent);color:#f5f7fa;font:600 20px system-ui,sans-serif;cursor:pointer}
button:active{transform:scale(0.98)}
button:focus-visible,a:focus-visible{outline:3px solid var(--link);outline-offset:3px}
ol{padding-left:24px;margin:20px 0}
li{margin:0 0 8px}
a{color:var(--link)}
textarea{width:100%;box-sizing:border-box;height:220px;margin-top:8px;background:var(--surface);color:var(--text);
 border:1px solid var(--line);border-radius:12px;padding:12px;font:13px/1.4 ui-monospace,monospace}
details{margin-top:24px;color:var(--dim)}
@media (prefers-reduced-motion: reduce){button:active{transform:none}}
</style></head><body><main>
<h1>KnockThru bug report</h1>
<p>This page comes from your Frame. Nothing has been sent yet.</p>
<button id="go">Copy report and open GitHub</button>
<ol>
<li>Press the button above.</li>
<li>On GitHub, sign in if it asks.</li>
<li>Say what went wrong, then paste into the <b>Report</b> box.</li>
<li>Press <b>Create</b>.</li>
</ol>
<p id="status" role="status"></p>
<p>Copy did not work? <a href=")HTML" + html_escape(download) + R"HTML(" download>Download the report</a> and attach it instead, or
<a href=")HTML" + html_escape(issue_link) + R"HTML(">open GitHub</a> without it.</p>
<details><summary>See the report</summary><textarea id="report" readonly>)HTML" +
           html_escape(report) + R"HTML(</textarea></details>
<script>
const link = ")HTML" + issue_link + R"HTML(";
document.getElementById("go").addEventListener("click", async () => {
  const box = document.getElementById("report");
  let copied = false;
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(box.value);
      copied = true;
    }
  } catch (e) {}
  if (!copied) {
    // Phones block the clipboard API on plain HTTP, so copy through a selection.
    box.closest("details").open = true;
    box.focus();
    box.select();
    box.setSelectionRange(0, box.value.length);
    try { copied = document.execCommand("copy"); } catch (e) {}
    box.blur();
  }
  document.getElementById("status").textContent = copied
    ? "Copied. Opening GitHub..." : "Could not copy. Use Download the report below.";
  if (copied) setTimeout(() => { window.location.href = link; }, 600);
});
</script>
</main></body></html>
)HTML";
}

ReportServer::ReportServer(std::string report, std::string issue_link)
    : report_(std::move(report)) {
    const auto address = local_address();
    if (address.empty()) throw std::runtime_error("The Frame is not on a network");
    std::random_device random;
    static const char* digits = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) token_ += digits[random() % 16];

    listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_ < 0) throw std::runtime_error("Cannot create a socket");
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_address.sin_port = 0; // the kernel picks a free port, above 1024
    socklen_t size = sizeof bind_address;
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&bind_address), sizeof bind_address) < 0 ||
        ::listen(listener_, 8) < 0 ||
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&bind_address), &size) < 0) {
        ::close(listener_);
        throw std::runtime_error("Cannot listen for phones");
    }
    page_ = report_page(report_, issue_link, "/" + token_ + "/report/knockthru-report.txt");
    url_ = "http://" + address + ":" + std::to_string(ntohs(bind_address.sin_port)) + "/" + token_;
    thread_ = std::thread([this] { loop(); });
}

ReportServer::~ReportServer() {
    stop_ = true;
    thread_.join();
    ::close(listener_);
}

void ReportServer::loop() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
    while (!stop_ && std::chrono::steady_clock::now() < deadline) {
        pollfd waiting{listener_, POLLIN, 0};
        if (::poll(&waiting, 1, 200) <= 0) continue;
        const int client = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        answer(client);
        ::close(client);
    }
}

void ReportServer::answer(int client) {
    timeval timeout{3, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    std::string request;
    char buffer[1024];
    while (request.find("\r\n") == std::string::npos && request.size() < 4096) {
        const auto n = ::recv(client, buffer, sizeof buffer, 0);
        if (n <= 0) return;
        request.append(buffer, static_cast<std::size_t>(n));
    }
    std::istringstream line(request.substr(0, request.find("\r\n")));
    std::string method, path;
    line >> method >> path;
    if (method == "GET" && path == "/" + token_) {
        send_all(client, response("200 OK", "text/html; charset=utf-8", page_));
    } else if (method == "GET" && path == "/" + token_ + "/report/knockthru-report.txt") {
        send_all(client, response("200 OK", "text/plain; charset=utf-8", report_,
            "Content-Disposition: attachment; filename=\"knockthru-report.txt\"\r\n"));
    } else {
        send_all(client, response("404 Not Found", "text/plain", "Not found\n"));
    }
}

} // namespace knockthru
