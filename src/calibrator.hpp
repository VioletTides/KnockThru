#pragma once

#include "knock_detector.hpp"

#include <optional>
#include <vector>

namespace knockthru {

// Collects knocks of one kind with lowered thresholds, then sets that kind's
// threshold to a fraction of the weakest knock. The sensitivity slider picks
// the fraction, and the middle of the slider gives 60 percent. Floors come from replaying
// tests/data: surface below 50 m/s^2 splits real doubles, and together or
// headset below 20 lets rebounds pass the fast_gap_s check and turn doubles
// into triples.
//
// At the lowered thresholds, waving the controllers or getting ready gave
// about one knock a second in the play and noise recordings. Starting a swing
// from rest also spikes, about 0.1 s before the knock it leads to. So knocks
// less than 0.25 s apart form a burst, and a burst stands for its knock with
// the fastest approach. A burst counts when:
// - Nothing else came in the 0.6 s before it.
// - Its knock approached at 0.25 m/s or faster.
// - For surface knocks, the controller was nearly still before the swing.
//   Together and headset knocks have a second device to confirm them, and
//   headset taps start with the arm rising.
// These rules leave 1 knock in the 90 s of play and noise.
class Calibrator {
public:
    static constexpr int kWanted = 10;
    static constexpr double kSeconds = 90;
    static constexpr double kBurstGap = 0.25;   // s, knocks closer than this are one burst
    static constexpr double kQuietBefore = 0.6; // s without another knock before a burst
    static constexpr double kMinApproach = 0.25; // m/s
    static constexpr double kStillSpeed = 0.6;  // m/s, mean speed before a surface swing

    enum class Heard { Counted, OtherKind, Ignored };
    struct Result {
        Heard heard;
        Knock knock; // the burst's knock with the fastest approach
    };

    Calibrator(KnockKind kind, const DetectorConfig& base, double start);

    // Detector settings to use while calibrating.
    const DetectorConfig& detector_config() const { return config_; }

    // Takes every knock the detector judged, in time order.
    void hear(const Knock& knock);
    // Returns the next finished burst. A burst finishes kBurstGap after its last
    // knock, so call this every loop with the current time.
    std::optional<Result> next(double now);

    KnockKind kind() const { return kind_; }
    int count() const { return static_cast<int>(peaks_.size()); }
    bool done() const { return count() >= kWanted; }
    double seconds_left(double now) const;
    double weakest() const;
    const std::vector<double>& peaks() const { return peaks_; }
    const std::vector<double>& speeds() const { return speeds_; } // approach speeds, m/s

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
    std::vector<Knock> burst_;
    bool burst_quiet_ = false;     // nothing came in the kQuietBefore before the burst
    double last_heard_ = -1e9;     // the last knock of the previous burst
    std::vector<Result> results_;  // finished bursts, oldest first

    void finish_burst();
    std::vector<double> peaks_, speeds_;
};

} // namespace knockthru
