#pragma once

#include "knock_detector.hpp"

#include <vector>

namespace knockthru {

// Collects knocks of one kind with lowered thresholds, then sets that kind's
// threshold to a fraction of the weakest knock. The sensitivity slider picks
// the fraction, and the middle of the slider gives 60 percent. Floors come from replaying
// tests/data: surface below 50 m/s^2 splits real doubles, and together or
// headset below 20 lets rebounds pass the fast_gap_s check and turn doubles
// into triples.
//
// Only deliberate knocks count: nothing else in the 0.6 s before, and the
// controller nearly still before its swing. At the lowered thresholds, waving
// the controllers or getting ready gave about one knock a second in the play
// and noise recordings. These two rules leave 2 in their 90 s, and keep every
// first knock of a double in the knock recordings.
class Calibrator {
public:
    static constexpr int kWanted = 10;
    static constexpr double kSeconds = 90;
    static constexpr double kQuietBefore = 0.6; // s without another knock
    static constexpr double kStillSpeed = 0.6; // m/s, mean speed before the swing

    enum class Heard { Counted, OtherKind, Ignored };

    Calibrator(KnockKind kind, const DetectorConfig& base, double start);

    // Detector settings to use while calibrating.
    const DetectorConfig& detector_config() const { return config_; }

    // Takes every knock the detector judged, in time order. Counts a deliberate
    // knock of this kind. A deliberate knock of another kind is OtherKind.
    Heard add(const Knock& knock);

    KnockKind kind() const { return kind_; }
    int count() const { return static_cast<int>(peaks_.size()); }
    bool done() const { return count() >= kWanted; }
    double seconds_left(double now) const;
    double weakest() const;
    const std::vector<double>& peaks() const { return peaks_; }

    // The threshold for recorded peaks at a sensitivity from 0 (least) to 1 (most).
    static double threshold(KnockKind kind, const std::vector<double>& peaks, double sensitivity);
    // The lowest threshold calibration sets for a kind.
    static double floor(KnockKind kind);

    // The app's detector settings with this kind's threshold at a sensitivity, for
    // trying a calibration before saving it.
    DetectorConfig trial_config(const DetectorConfig& base, double sensitivity) const;

    // Saves the threshold to calibration.json and returns it. Call after done().
    double save(double sensitivity = 0.5) const;

private:
    KnockKind kind_;
    DetectorConfig config_;
    double start_;
    double last_heard_ = -1e9;
    std::vector<double> peaks_;
};

} // namespace knockthru
