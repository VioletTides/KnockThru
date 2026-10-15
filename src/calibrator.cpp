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

bool Calibrator::add(const Knock& knock) {
    if (knock.kind != kind_ || done()) return false;
    peaks_.push_back(knock.peak);
    return true;
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
