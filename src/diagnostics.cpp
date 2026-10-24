#include "diagnostics.hpp"

#include "config.hpp"
#include "steamvr.hpp"
#include "style.hpp"
#include "update.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <sys/utsname.h>

namespace knockthru {

namespace fs = std::filesystem;

namespace {

fs::path home_directory() {
    const auto home = std::getenv("HOME");
    return home && *home ? home : "/tmp";
}

// The last `count` lines of a text file, or a note if it is missing.
std::string tail(const fs::path& path, std::size_t count) {
    std::ifstream stream(path);
    if (!stream) return "(missing)\n";
    std::deque<std::string> lines;
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(std::move(line));
        if (lines.size() > count) lines.pop_front();
    }
    std::string text;
    for (const auto& line : lines) text += line + "\n";
    return text.empty() ? "(empty)\n" : text;
}

std::string os_name() {
    std::ifstream stream("/etc/os-release");
    std::string name = "unknown";
    for (std::string line; std::getline(stream, line);)
        if (line.rfind("PRETTY_NAME=", 0) == 0) name = line.substr(12);
    name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
    utsname system{};
    if (::uname(&system) == 0) name += std::string(", kernel ") + system.release;
    return name;
}

} // namespace

fs::path report_path() { return home_directory() / "knockthru-report.txt"; }

fs::path recording_path() { return home_directory() / "knockthru-knocks.csv.gz"; }

void write_recording(const KnockRecording& recording) {
    const auto csv = user_directory() / "knocks.csv";
    {
        std::ofstream out(csv, std::ios::trunc);
        out << kCsvHeader;
        for (const auto& s : recording.samples)
            write_csv_row(out, s.t - recording.start, s.device, s.position, s.velocity);
        if (!out) throw std::runtime_error("Cannot write " + csv.string());
    }
    if (run_program({"gzip", "-f", csv.string()}) != 0)
        throw std::runtime_error("gzip could not compress the recording");
    fs::copy_file(csv.string() + ".gz", recording_path(), fs::copy_options::overwrite_existing);
    fs::remove(csv.string() + ".gz");
    std::ofstream summary(user_directory() / "knocks.txt", std::ios::trunc);
    summary << "Recorded " << recording.samples.size() << " samples over " << KnockRecording::kSeconds
            << " s. Heard " << recording.heard.size() << " knocks and actions:\n";
    for (const auto& line : recording.heard) summary << line << "\n";
}

std::string report_text(bool vr, const std::string& room_view) {
    std::ostringstream out;
    const auto now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%F %T", std::localtime(&now));
    out << "KnockThru bug report, " << stamp << "\n\n";
    out << "KnockThru: " << kVersion << "\n";
    out << "Program: " << executable().string() << "\n";
    out << "System: " << os_name() << "\n";
    if (vr) {
        auto* apps = vr::VRApplications();
        out << "SteamVR: " << steamvr_version() << "\n";
        const auto remembered = load_state();
        if (!remembered.steamvr_since.empty())
            out << "SteamVR seen since: " << remembered.steamvr_since
                << (remembered.steamvr_previous.empty() ? "" : ", before that " + remembered.steamvr_previous)
                << "\n";
        out << "Registered with SteamVR: " << (apps->IsApplicationInstalled(kAppKey) ? "yes" : "no") << "\n";
        out << "Starts with SteamVR: " << (apps->GetApplicationAutoLaunch(kAppKey) ? "yes" : "no") << "\n";
        const auto pid = apps->GetApplicationProcessId(kAppKey);
        out << "Running: " << (pid ? "yes, pid " + std::to_string(pid) : std::string("no")) << "\n";
        try {
            out << "Style: " << to_string(read_style()) << "\n";
        } catch (const std::exception& error) {
            out << "Style: " << error.what() << "\n";
        }
        const auto key = scene_app_key();
        out << "Current app: " << (key.empty() ? "none" : key) << "\n";
    } else {
        out << "SteamVR: not running\n";
    }
    out << "Room View: " << room_view << "\n";
    const auto user = user_directory();
    // The summary of the last knock recording, if it is under an hour old.
    std::error_code age_error;
    const auto knocks = user / "knocks.txt";
    if (fs::exists(knocks, age_error)
        && fs::file_time_type::clock::now() - fs::last_write_time(knocks, age_error) < std::chrono::hours(1))
        out << "\n== Knock recording, knockthru-knocks.csv.gz ==\n" << tail(knocks, 200);
    for (const auto* name : {"config.json", "calibration.json"})
        out << "\n== " << (user / name).string() << " ==\n" << tail(user / name, 400);
    out << "\n== " << (user / "application.log").string() << ", last 300 lines ==\n"
        << tail(user / "application.log", 300);
    const auto home = std::getenv("HOME");
    if (home && *home) {
        const auto steam_log = fs::path(home) / ".local/share/Steam/logs/vrclient_knockthru.txt";
        out << "\n== " << steam_log.string() << ", last 100 lines ==\n" << tail(steam_log, 100);
    }
    return out.str();
}

fs::path write_report(bool vr, const std::string& room_view) {
    const auto path = report_path();
    std::ofstream file(path, std::ios::trunc);
    if (!(file << report_text(vr, room_view))) throw std::runtime_error("Cannot write " + path.string());
    return path;
}

} // namespace knockthru
