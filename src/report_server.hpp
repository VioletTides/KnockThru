#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace knockthru {

// The headset's IPv4 address on the network its default route uses, such as
// 192.168.2.222. Empty without a network.
std::string local_address();

// A ZIP archive of named files, stored without compression.
std::string make_zip(const std::vector<std::pair<std::string, std::string>>& files);

// The phone page. Without a knock recording, one button copies the report and
// opens `issue_link`, the prefilled GitHub form, and `download` is the report
// as a text file. With one, `bundle` is a ZIP of the report and the recording,
// and the page asks for it to be attached. `issue_link` must be URL-encoded,
// so it has no quotes.
std::string report_page(const std::string& report, const std::string& issue_link, const std::string& download,
    const std::string& bundle = "");

// Serves one report to phones on the same network, so a QR code can carry a
// short link instead of the report. Paths need a random token, and the server
// stops after 15 minutes or when destroyed. SteamOS's firewall opens TCP
// ports 1024 to 65535, and the kernel picks the port from that range.
class ReportServer {
public:
    // `report` is the whole report. The page's copy button gets it shortened to fit
    // a GitHub issue, and the downloads get it whole. Throws if there is no
    // network or the socket cannot listen.
    ReportServer(std::string report, std::string issue_link);
    ~ReportServer();
    ReportServer(const ReportServer&) = delete;

    // http://address:port/token, the page link for the QR code.
    const std::string& url() const { return url_; }

    // Replaces the report, for one that gained a knock recording summary.
    void set_report(std::string report);
    // Offers a gzipped knock recording on the page. The file is read when a phone asks for it.
    void set_recording(std::filesystem::path recording);

private:
    void loop();
    void answer(int client);

    std::mutex mutex_; // guards report_ and recording_, which the app sets while phones read them
    std::string report_, issue_link_, url_, token_;
    std::filesystem::path recording_;
    int listener_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace knockthru
