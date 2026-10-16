#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace knockthru {

// The headset's IPv4 address on the network its default route uses, such as
// 192.168.2.222. Empty without a network.
std::string local_address();

// The phone page: the report in a box, and one button that copies it and
// opens `issue_link`, the prefilled GitHub form. `download` is the path of
// the report as a file. `issue_link` must be URL-encoded, so it has no quotes.
std::string report_page(const std::string& report, const std::string& issue_link, const std::string& download);

// Serves one report to phones on the same network, so a QR code can carry a
// short link instead of the report. Paths need a random token, and the server
// stops after 15 minutes or when destroyed. SteamOS's firewall opens TCP
// ports 1024 to 65535, and the kernel picks the port from that range.
class ReportServer {
public:
    // Throws if there is no network or the socket cannot listen.
    ReportServer(std::string report, std::string issue_link);
    ~ReportServer();
    ReportServer(const ReportServer&) = delete;

    // http://address:port/token, the page link for the QR code.
    const std::string& url() const { return url_; }

private:
    void loop();
    void answer(int client);

    std::string report_, page_, url_, token_;
    int listener_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace knockthru
