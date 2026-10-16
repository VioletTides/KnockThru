#include "report_server.hpp"

#include "report.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

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

// Sends in waits of at most 200 ms, so a phone that stops reading never holds the
// server past `stop`, which Done sets, or past 30 s in all.
void send_all(int fd, const std::atomic<bool>& stop, const std::string& data) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (std::size_t sent = 0; sent < data.size();) {
        if (stop || std::chrono::steady_clock::now() > deadline) return;
        pollfd ready{fd, POLLOUT, 0};
        if (::poll(&ready, 1, 200) <= 0) continue;
        const auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
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

std::string make_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    // Stored entries, without compression: the recording is already gzipped, and the report is small.
    const auto crc32 = [](const std::string& data) {
        std::uint32_t crc = 0xFFFFFFFF;
        for (const unsigned char byte : data) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
        }
        return crc ^ 0xFFFFFFFF;
    };
    const auto put16 = [](std::string& out, std::uint32_t value) {
        out += static_cast<char>(value & 0xFF);
        out += static_cast<char>((value >> 8) & 0xFF);
    };
    const auto put32 = [&](std::string& out, std::uint32_t value) {
        put16(out, value & 0xFFFF);
        put16(out, value >> 16);
    };
    std::string zip, directory;
    for (const auto& [name, data] : files) {
        const auto crc = crc32(data);
        const auto offset = static_cast<std::uint32_t>(zip.size());
        const auto size = static_cast<std::uint32_t>(data.size());
        put32(zip, 0x04034b50);
        put16(zip, 20); put16(zip, 0); put16(zip, 0); // version, flags, stored
        put16(zip, 0); put16(zip, 33);                // 1980-01-01 00:00
        put32(zip, crc); put32(zip, size); put32(zip, size);
        put16(zip, static_cast<std::uint32_t>(name.size())); put16(zip, 0);
        zip += name;
        zip += data;
        put32(directory, 0x02014b50);
        put16(directory, 0x0314); put16(directory, 20); put16(directory, 0); put16(directory, 0);
        put16(directory, 0); put16(directory, 33);
        put32(directory, crc); put32(directory, size); put32(directory, size);
        put16(directory, static_cast<std::uint32_t>(name.size()));
        put16(directory, 0); put16(directory, 0); put16(directory, 0); put16(directory, 0);
        put32(directory, 0100644u << 16); // a regular file, rw-r--r--
        put32(directory, offset);
        directory += name;
    }
    const auto directory_offset = static_cast<std::uint32_t>(zip.size());
    zip += directory;
    put32(zip, 0x06054b50);
    put16(zip, 0); put16(zip, 0);
    put16(zip, static_cast<std::uint32_t>(files.size())); put16(zip, static_cast<std::uint32_t>(files.size()));
    put32(zip, static_cast<std::uint32_t>(directory.size())); put32(zip, directory_offset);
    put16(zip, 0);
    return zip;
}

std::string report_page(const std::string& report, const std::string& issue_link, const std::string& download,
                        const std::string& bundle) {
    // With a knock recording, the page leads with one file that holds the report and the
    // recording, because a second download link is easy to miss. Without one, it copies the report.
    const std::string steps = bundle.empty() ? R"HTML(<button id="go">Copy report and open GitHub</button>
<p id="status" role="status"></p>
<ol>
<li>Sign in to GitHub if it asks.</li>
<li>Paste into the <b>Report</b> box.</li>
<li>Say what went wrong, then tap <b>Create</b>.</li>
</ol>
<p>Copy didn't work? <a href=")HTML" + html_escape(download) + R"HTML(" download>Download the report</a> and attach it instead.</p>)HTML"
        : R"HTML(<p>Your report includes a knock recording, so it comes as one file to attach.</p>
<ol class="actions">
<li><a class="button" href=")HTML" + html_escape(bundle) + R"HTML(" download>Download the report file</a></li>
<li><a class="button" href=")HTML" + html_escape(issue_link) + R"HTML(">Open GitHub</a></li>
<li>Sign in if it asks. Attach <b class="file">knockthru-report.zip</b> from your downloads, say what went wrong, then tap <b>Create</b>.</li>
</ol>)HTML";
    return R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>KnockThru bug report</title>
<style>
/* The panel's colors. The button blue is darker than the panel accent, so its text reaches 4.7:1 (WCAG AA). */
:root{--bg:#121418;--surface:#1c1f25;--line:#2e323a;--text:#eceef1;--dim:#969ca8;--accent:#2670bc;--link:#50a0f0}
@media (prefers-color-scheme: light){
 :root{--bg:#f4f5f7;--surface:#ffffff;--line:#d5d9e0;--text:#16181c;--dim:#545b68;--link:#1f66b0}
}
body{margin:0;padding:24px 16px;background:var(--bg);color:var(--text);font:18px/1.5 system-ui,sans-serif}
main{max-width:560px;margin:0 auto}
h1{font-size:26px;margin:0 0 8px}
p{margin:0 0 16px;color:var(--dim)}
button,a.button{display:block;width:100%;box-sizing:border-box;padding:18px;border:0;border-radius:16px;
 background:var(--accent);color:#f5f7fa;font:600 20px system-ui,sans-serif;text-align:center;text-decoration:none;cursor:pointer}
button:active,a.button:active{transform:scale(0.98)}
button:focus-visible,a:focus-visible{outline:3px solid var(--link);outline-offset:3px}
ol{padding-left:24px;margin:20px 0}
li{margin:0 0 8px}
ol.actions li{margin:0 0 16px}
.file{white-space:nowrap}
a{color:var(--link)}
textarea{width:100%;box-sizing:border-box;height:220px;margin-top:8px;background:var(--surface);color:var(--text);
 border:1px solid var(--line);border-radius:16px;padding:12px;font:13px/1.4 ui-monospace,monospace}
details{margin-top:24px;color:var(--dim)}
@media (prefers-reduced-motion: reduce){button:active,a.button:active{transform:none}}
</style></head><body><main>
<h1>KnockThru bug report</h1>
<p>Sent from your Frame. GitHub gets nothing until you tap Create.</p>
)HTML" + steps + R"HTML(
<details><summary>See the report</summary><textarea id="report" readonly>)HTML" +
           html_escape(report) + R"HTML(</textarea></details>
<script>
const link = ")HTML" + issue_link + R"HTML(";
const go = document.getElementById("go");
if (go) go.addEventListener("click", async () => {
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
    ? "Copied. Opening GitHub..." : "Could not copy. Download the report below instead.";
  if (copied) setTimeout(() => { window.location.href = link; }, 600);
});
</script>
</main></body></html>
)HTML";
}

ReportServer::ReportServer(std::string report, std::string issue_link)
    : report_(std::move(report)), issue_link_(std::move(issue_link)) {
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
    url_ = "http://" + address + ":" + std::to_string(ntohs(bind_address.sin_port)) + "/" + token_;
    thread_ = std::thread([this] { loop(); });
}

void ReportServer::set_report(std::string report) {
    std::lock_guard<std::mutex> lock(mutex_);
    report_ = std::move(report);
}

void ReportServer::set_recording(std::filesystem::path recording) {
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = std::move(recording);
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
    // Short receive waits, so the stop check above runs at least every 200 ms.
    timeval timeout{0, 200000};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    std::string request;
    char buffer[1024];
    int waits = 0;
    while (request.find("\r\n") == std::string::npos && request.size() < 4096) {
        if (stop_) return;
        const auto n = ::recv(client, buffer, sizeof buffer, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++waits < 15) continue; // 3 s for the request line
        if (n <= 0) return;
        request.append(buffer, static_cast<std::size_t>(n));
    }
    std::istringstream line(request.substr(0, request.find("\r\n")));
    std::string method, path;
    line >> method >> path;
    std::string report;
    std::filesystem::path recording;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        report = report_;
        recording = recording_;
    }
    const auto base = "/" + token_ + "/report/";
    std::error_code error;
    const bool has_recording = !recording.empty() && std::filesystem::is_regular_file(recording, error);
    if (method == "GET" && path == "/" + token_) {
        // The page's copy goes into a GitHub issue body, so it fits GitHub's limit. The downloads stay whole.
        const auto page = report_page(fit_report(report), issue_link_, base + "knockthru-report.txt",
            has_recording ? base + "knockthru-report.zip" : "");
        send_all(client, stop_, response("200 OK", "text/html; charset=utf-8", page));
    } else if (method == "GET" && path == base + "knockthru-report.txt") {
        send_all(client, stop_, response("200 OK", "text/plain; charset=utf-8", report,
            "Content-Disposition: attachment; filename=\"knockthru-report.txt\"\r\n"));
    } else if (method == "GET" && path == base + "knockthru-report.zip" && has_recording) {
        std::ifstream file(recording, std::ios::binary);
        const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto zip = make_zip({{"knockthru-report.txt", report}, {"knockthru-knocks.csv.gz", data}});
        send_all(client, stop_, response("200 OK", "application/zip", zip,
            "Content-Disposition: attachment; filename=\"knockthru-report.zip\"\r\n"));
    } else {
        send_all(client, stop_, response("404 Not Found", "text/plain", "Not found\n"));
    }
}

} // namespace knockthru
