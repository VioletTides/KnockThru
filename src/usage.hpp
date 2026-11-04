#pragma once

#include <optional>
#include <string>

namespace knockthru {

// CPU use of this process, for the debug view.
class CpuUsage {
public:
    // Percent of one core since the previous call, summed over threads. The first call returns 0.
    double sample();

private:
    std::optional<long> ticks_; // utime + stime at the previous call
    double at_ = 0; // steady clock seconds of the previous call
};

// The headset's battery power now, from the gauge scripts/measure.sh reads.
// Nothing while a charger is online or when the gauge files are missing.
std::optional<double> battery_mw();

} // namespace knockthru
