#include "usage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace knockthru {
namespace fs = std::filesystem;
namespace {

std::string read_line(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

// utime + stime of this process, in clock ticks. The fields after the ")" that ends
// the command name start at field 3, so utime and stime are the 12th and 13th.
std::optional<long> cpu_ticks() {
    const std::string line = read_line("/proc/self/stat");
    const auto close = line.rfind(')');
    if (close == std::string::npos) return std::nullopt;
    std::istringstream fields(line.substr(close + 1));
    std::string field;
    long utime = 0, stime = 0;
    for (int i = 1; i <= 13 && (fields >> field); ++i) {
        if (i == 12) utime = std::atol(field.c_str());
        if (i == 13) stime = std::atol(field.c_str());
    }
    return utime + stime;
}

} // namespace

double CpuUsage::sample() {
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto used = cpu_ticks();
    double percent = 0;
    if (used && ticks_ && now > at_)
        percent =
            std::max(0.0, static_cast<double>(*used - *ticks_) / sysconf(_SC_CLK_TCK) / (now - at_) * 100);
    ticks_ = used;
    at_ = now;
    return percent;
}

std::optional<double> battery_mw(const fs::path& supplies) {
    const auto battery = supplies / "max1720x_bat_7-36";
    // The gauge reads charging state while a charger is attached, so power only means something on battery.
    if (read_line(battery / "status") != "Discharging") return std::nullopt;
    std::error_code error;
    for (const auto& supply : fs::directory_iterator(supplies, error))
        if (read_line(supply.path() / "online") == "1") return std::nullopt;
    const std::string current = read_line(battery / "current_now");
    const std::string voltage = read_line(battery / "voltage_now");
    if (current.empty() || voltage.empty()) return std::nullopt;
    // current_now is in uA and voltage_now in uV. The sign of current_now depends on the gauge driver.
    return std::abs(std::atof(current.c_str())) * std::atof(voltage.c_str()) / 1e9;
}

} // namespace knockthru
