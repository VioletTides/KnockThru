#include "calibrator.hpp"

#include "config.hpp"

#include <algorithm>
#include <stdexcept>

namespace knockthru {

Calibrator::Calibrator(KnockKind kind, const DetectorConfig& base, double start)
    : kind_(kind), config_(base), start_(start) {
    config_.controller_peak = std::min(config_.controller_peak, 30.0);
    config_.confirmed_peak = std::min(config_.confirmed_peak, 12.0);
    config_.partner_peak = std::min(config_.partner_peak, 8.0);
    config_.enabled = {true, true, true};
}

void Calibrator::hear(const Knock& knock) {
    if (!burst_.empty() && knock.t - burst_.back().t > kBurstGap) finish_burst();
    if (burst_.empty()) burst_quiet_ = knock.t - last_heard_ >= kQuietBefore;
    burst_.push_back(knock);
}

std::optional<Calibrator::Result> Calibrator::next(double now) {
    if (!burst_.empty() && now - burst_.back().t > kBurstGap) finish_burst();
    if (results_.empty()) return std::nullopt;
    const auto result = results_.front();
    results_.erase(results_.begin());
    return result;
}

void Calibrator::finish_burst() {
    const auto best = *std::max_element(burst_.begin(), burst_.end(),
        [](const Knock& a, const Knock& b) { return a.approach_speed < b.approach_speed; });
    const bool still = best.kind != KnockKind::Surface || burst_.front().speed_before < kStillSpeed;
    const bool deliberate = burst_quiet_ && still && best.approach_speed >= kMinApproach;
    last_heard_ = burst_.back().t;
    burst_.clear();
    Heard heard = Heard::Ignored;
    if (deliberate && !done()) heard = best.kind == kind_ ? Heard::Counted : Heard::OtherKind;
    if (heard == Heard::Counted) {
        peaks_.push_back(best.peak);
        speeds_.push_back(best.approach_speed);
    }
    results_.push_back({heard, best});
}

double Calibrator::seconds_left(double now) const { return std::max(0.0, kSeconds - (now - start_)); }

double Calibrator::weakest() const {
    if (peaks_.empty()) throw std::logic_error("No knocks recorded");
    return *std::min_element(peaks_.begin(), peaks_.end());
}

double Calibrator::floor(KnockKind kind) { return kind == KnockKind::Surface ? 50.0 : 20.0; }

double Calibrator::threshold(KnockKind kind, const std::vector<double>& peaks, double sensitivity) {
    if (peaks.empty()) throw std::logic_error("No knocks recorded");
    // From 80 percent of the weakest knock at the least sensitive end to 40 percent at the most.
    const double fraction = 0.8 - 0.4 * std::clamp(sensitivity, 0.0, 1.0);
    return std::clamp(fraction * *std::min_element(peaks.begin(), peaks.end()), floor(kind), 150.0);
}

namespace {

std::optional<double>& slot(Calibration& calibration, KnockKind kind) {
    return kind == KnockKind::Surface ? calibration.surface
        : kind == KnockKind::Together ? calibration.together : calibration.headset;
}

} // namespace

DetectorConfig Calibrator::trial_config(const DetectorConfig& base, double sensitivity) const {
    DetectorConfig config = base;
    Calibration only;
    slot(only, kind_) = threshold(kind_, peaks_, sensitivity);
    apply_calibration(only, config);
    config.enabled = {true, true, true};
    return config;
}

double Calibrator::save(double sensitivity) const {
    const double value = threshold(kind_, peaks_, sensitivity);
    auto calibration = load_calibration();
    slot(calibration, kind_) = value;
    save_calibration(calibration);
    return value;
}

} // namespace knockthru
