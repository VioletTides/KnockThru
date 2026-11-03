#include "debug_view.hpp"

#include <algorithm>
#include <cmath>

namespace knockthru {

namespace {

struct Row {
    const char* key;
    const char* label;
    double DetectorConfig::* field;
    double step;
    int decimals;
    const char* unit;
};

constexpr double kPeakStep = 1, kSpeedStep = 0.05, kLengthStep = 0.01;

// The first five are the tab's first page.
const Row kRows[] = {
    {"controller_peak", "Surface strength", &DetectorConfig::controller_peak, kPeakStep, 0, "m/s²"},
    {"confirmed_peak", "Together strength", &DetectorConfig::confirmed_peak, kPeakStep, 0, "m/s²"},
    {"headset_knock_peak", "Headset strength", &DetectorConfig::headset_knock_peak, kPeakStep, 0, "m/s²"},
    {"max_speed_after", "Speed after knock", &DetectorConfig::max_speed_after, kSpeedStep, 2, "m/s"},
    {"max_gap_s", "Longest gap in a set", &DetectorConfig::max_gap_s, kLengthStep, 2, "s"},
    {"partner_peak", "Other hand's spike", &DetectorConfig::partner_peak, kPeakStep, 0, "m/s²"},
    {"headset_peak", "Headset spike", &DetectorConfig::headset_peak, kPeakStep, 0, "m/s²"},
    {"surface_speed_after", "Surface speed after", &DetectorConfig::surface_speed_after, kSpeedStep, 2,
        "m/s"},
    {"max_speed_before", "Speed before knock", &DetectorConfig::max_speed_before, kSpeedStep, 2, "m/s"},
    {"surface_speed_before", "Surface speed before", &DetectorConfig::surface_speed_before, kSpeedStep, 2,
        "m/s"},
    {"together_distance", "Controller distance", &DetectorConfig::together_distance, kLengthStep, 2, "m"},
    {"same_spot", "Same spot", &DetectorConfig::same_spot, kLengthStep, 2, "m"},
    {"merge_s", "Bounce window", &DetectorConfig::merge_s, kLengthStep, 2, "s"},
    {"min_gap_s", "Surface rebound gap", &DetectorConfig::min_gap_s, kLengthStep, 2, "s"},
    {"fast_gap_s", "Rebound gap", &DetectorConfig::fast_gap_s, kLengthStep, 2, "s"},
    {"quiet_before_s", "Quiet before a set", &DetectorConfig::quiet_before_s, kLengthStep, 2, "s"},
};

} // namespace

std::vector<Tunable> tunables(const DetectorConfig& live, const AppConfig& saved) {
    std::vector<Tunable> out;
    for (const auto& row : kRows) {
        Tunable tunable{std::string(row.label) + " (" + row.unit + ")", live.*row.field,
            saved.detector.*row.field, row.decimals, ""};
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
    live.*row.field = std::max(0.0, std::round((live.*row.field + direction * row.step) * scale) / scale);
}

} // namespace knockthru
