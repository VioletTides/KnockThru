#pragma once

#include "config.hpp"
#include "knock_detector.hpp"

#include <optional>
#include <vector>

namespace knockthru {

// Collects knocks of one kind with lowered thresholds, then sets two
// thresholds for that kind:
// - The spike threshold is 60 percent of the weakest knock's spike. Floors
//   come from replaying tests/data: surface below 50 m/s^2 splits real
//   doubles, and together or headset below 20 lets rebounds pass the
//   fast_gap_s check and turn doubles into triples.
// - The approach threshold is 55 percent of the second slowest recorded
//   approach. Approach speed varies less than the spike between similar
//   knocks, and slow movement falls below it.
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
// A burst of this kind whose spike falls under floor() is TooLight and does not
// count, since it could never trigger an action. These rules count no knocks
// in the 90 s of play and noise.
class Calibrator {
public:
    static constexpr int kWanted = 10;
    static constexpr double kSeconds = 90;
    // s after the start when knocks do not count yet. On the Frame, the hand
    // coming down from pressing Start counted as a knock 1 s in.
    static constexpr double kGetReady = 3;
    static constexpr double kBurstGap = 0.25;   // s, knocks closer than this are one burst
    static constexpr double kQuietBefore = 0.6; // s without another knock before a burst
    static constexpr double kMinApproach = 0.25; // m/s
    static constexpr double kStillSpeed = 0.6;  // m/s, mean speed before a surface swing

    // TooLight is a deliberate knock of this kind whose spike falls under floor().
    enum class Heard { Counted, OtherKind, TooLight, Ignored };
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

    // The spike threshold for recorded spikes, in m/s^2.
    static double peak_threshold(KnockKind kind, const std::vector<double>& peaks);
    // The lowest spike threshold calibration sets for a kind.
    static double floor(KnockKind kind);
    // The approach threshold for recorded approach speeds, in m/s.
    static double approach_threshold(const std::vector<double>& speeds);

    // This calibration. Call after done().
    KindCalibration result() const;

    // The app's detector settings with this kind's thresholds, for trying a
    // calibration before saving it.
    DetectorConfig trial_config(const DetectorConfig& base) const;

    // Saves this kind to calibration.json and returns what it saved.
    KindCalibration save() const;

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
