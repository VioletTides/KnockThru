#include "perf_overlay.hpp"

#include "steamvr.hpp"

#include <openvr.h>

namespace knockthru {
namespace {

constexpr const char* kName = "perfcheck.perfGraphInHMD";

} // namespace

bool read_perf_overlay() {
    vr::EVRSettingsError error = vr::VRSettingsError_None;
    const bool shown =
        vr::VRSettings()->GetBool(vr::k_pch_Perf_Section, vr::k_pch_Perf_PerfGraphInHMD_Bool, &error);
    check_setting(error, kName);
    return shown;
}

bool toggle_perf_overlay() {
    const bool target = !read_perf_overlay();
    set_setting(kName, target, read_perf_overlay, [](bool shown, vr::EVRSettingsError* error) {
        vr::VRSettings()->SetBool(vr::k_pch_Perf_Section, vr::k_pch_Perf_PerfGraphInHMD_Bool, shown, error);
    });
    return target;
}

} // namespace knockthru
