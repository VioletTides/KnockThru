#include "debug_view.hpp"

#include <algorithm>
#include <cmath>

namespace knockthru {

namespace {

struct Row {
    const char* key;
    const char* label;
    double step;
    int decimals;
    const char* unit;
};

constexpr double kPeakStep = 1, kSpeedStep = 0.05, kLengthStep = 0.01;

// The first five are the tab's first page.
const Row kRows[] = {
    {"controller_peak", "Surface strength", kPeakStep, 0, "m/s²"},
    {"confirmed_peak", "Together strength", kPeakStep, 0, "m/s²"},
    {"headset_knock_peak", "Headset strength", kPeakStep, 0, "m/s²"},
    {"max_speed_after", "Speed after knock", kSpeedStep, 2, "m/s"},
    {"max_gap_s", "Longest gap in a set", kLengthStep, 2, "s"},
    {"partner_peak", "Other hand's spike", kPeakStep, 0, "m/s²"},
    {"headset_peak", "Headset spike", kPeakStep, 0, "m/s²"},
    {"surface_speed_after", "Surface speed after", kSpeedStep, 2, "m/s"},
    {"max_speed_before", "Speed before knock", kSpeedStep, 2, "m/s"},
    {"surface_speed_before", "Surface speed before", kSpeedStep, 2, "m/s"},
    {"together_distance", "Controller distance", kLengthStep, 2, "m"},
    {"same_spot", "Same spot", kLengthStep, 2, "m"},
    {"merge_s", "Bounce window", kLengthStep, 2, "s"},
    {"min_gap_s", "Surface rebound gap", kLengthStep, 2, "s"},
    {"fast_gap_s", "Rebound gap", kLengthStep, 2, "s"},
    {"quiet_before_s", "Quiet before a set", kLengthStep, 2, "s"},
};

} // namespace

std::vector<Tunable> tunables(const DetectorConfig& live, const AppConfig& saved) {
    std::vector<Tunable> out;
    for (const auto& row : kRows) {
        const auto field = detector_member(row.key);
        Tunable tunable{std::string(row.label) + " (" + row.unit + ")", live.*field, saved.detector.*field,
            row.decimals, ""};
        if (tunable.changed()) {
            tunable.detail = "Saved value " + number(tunable.saved, tunable.decimals);
        } else if (const auto source = saved.detector_sources.find(row.key);
            source != saved.detector_sources.end()) {
            if (source->second == Source::Calibration)
                tunable.detail = "From calibration";
            else if (source->second != Source::Default)
                tunable.detail = "Saved";
        }
        out.push_back(std::move(tunable));
    }
    return out;
}

void step_tunable(DetectorConfig& live, int index, int direction) {
    if (index < 0 || index >= static_cast<int>(std::size(kRows))) return;
    const auto& row = kRows[index];
    // Rounds to the row's decimals, so repeated steps never drift.
    const double scale = std::pow(10.0, row.decimals);
    const auto field = detector_member(row.key);
    live.*field = std::max(0.0, std::round((live.*field + direction * row.step) * scale) / scale);
}

void keep_tuning(DetectorConfig& next, const DetectorConfig& live, const DetectorConfig& saved) {
    for (const auto& row : kRows) {
        const auto field = detector_member(row.key);
        if (live.*field != saved.*field && next.*field == saved.*field) next.*field = live.*field;
    }
}

} // namespace knockthru
