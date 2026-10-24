#pragma once

#include "poses.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace knockthru {

// The bug report files go in the home folder, outside ~/.config, so a person can find them to attach.
std::filesystem::path report_path();
std::filesystem::path recording_path();

// 20 s of headset and controller motion, in the tests/data CSV format, for a knock bug report.
struct KnockRecording {
    static constexpr double kSeconds = 20;
    double start = 0;
    std::vector<PoseThread::Sample> samples;
    std::vector<std::string> heard; // one line per knock and action, for the report
};

// Writes the recording as ~/knockthru-knocks.csv.gz and its summary as knocks.txt.
// The angular velocity columns are zero, because the detector does not read them.
void write_recording(const KnockRecording& recording);

// A bug report for a GitHub issue: versions, SteamVR state, settings, and the
// end of the logs. `vr` is false when SteamVR is not running, and `room_view`
// describes what the caller read from Room View.
std::string report_text(bool vr, const std::string& room_view);
std::filesystem::path write_report(bool vr, const std::string& room_view);

} // namespace knockthru
