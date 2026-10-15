#pragma once

#include "knock_detector.hpp"

#include <vector>

namespace knockthru {

// Collects knocks of one kind with lowered thresholds, then sets that kind's
// threshold to 60 percent of the weakest knock. Floors come from replaying
// tests/data: surface below 50 m/s^2 splits real doubles, and together or
// headset below 20 lets rebounds pass the fast_gap_s check and turn doubles
// into triples.
class Calibrator {
public:
    static constexpr int kWanted = 10;
    static constexpr double kSeconds = 90;

    Calibrator(KnockKind kind, const DetectorConfig& base, double start);

    // Detector settings to use while calibrating.
    const DetectorConfig& detector_config() const { return config_; }

    // Counts the knock if it has the right kind. Returns whether it counted.
    bool add(const Knock& knock);

    KnockKind kind() const { return kind_; }
    int count() const { return static_cast<int>(peaks_.size()); }
    bool done() const { return count() >= kWanted; }
    double seconds_left(double now) const;
    double weakest() const;

    // Saves the threshold to calibration.json and returns it. Call after done().
    double save() const;

private:
    KnockKind kind_;
    DetectorConfig config_;
    double start_;
    std::vector<double> peaks_;
};

} // namespace knockthru
