#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace knockthru {

// The latest release's FrameDrop manifest. GitHub redirects it to the newest v* release.
constexpr const char* kLatestManifestUrl =
    "https://github.com/VioletTides/KnockThru/releases/latest/download/knockthru.framedrop.json";

// A published release, from its FrameDrop manifest.
struct Release {
    std::string version; // from the ZIP name, knockthru-X.Y.Z-linux-arm64.zip
    std::string url;     // the release ZIP
    std::string sha256;  // lowercase hex
};

// Compares two versions such as 0.2.0 and 0.10.1-rc1, number by number.
// A version with a suffix sorts before the same version without one.
// Returns a negative number, zero, or a positive number, like strcmp.
int compare_versions(const std::string& a, const std::string& b);

// Reads a knockthru.framedrop.json manifest. Throws if it has no usable ZIP
// entry. ZIP URLs must use HTTPS, or file:// with `allow_local`, for tests.
Release parse_manifest(const std::string& text, bool allow_local = false);

// The SHA-256 of a file's contents, in lowercase hex.
std::string sha256_file(const std::filesystem::path& path);
std::string sha256(const std::string& data);

// Thrown when a download or unpack stops because `cancel` was set.
struct Canceled : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Runs a program found on PATH, without a shell, and waits for it. Its output
// goes to /dev/null. Returns the exit code, or -1 if it could not start. When
// `cancel` becomes true, stops the program within about 50 ms and returns -2.
// With `cancel`, `on_poll` runs about every 50 ms while the program runs.
int run_program(const std::vector<std::string>& args, const std::atomic<bool>* cancel = nullptr,
    const std::function<void()>& on_poll = {});

// Starts a program in its own session and returns without waiting, so it
// outlives this process. Throws if it could not start.
void start_detached(const std::vector<std::string>& args);

// Downloads the latest manifest with curl into `work`, which is deleted after.
// Throws a readable message on failure. The environment variable
// KNOCKTHRU_UPDATE_MANIFEST replaces the GitHub URL, for testing a release
// before it is published. A file:// manifest may then point at a file:// ZIP.
Release fetch_latest_release(const std::filesystem::path& work, const std::atomic<bool>* cancel = nullptr);

// Downloads the release ZIP, checks its SHA-256, unpacks it, and replaces the
// files in `install_directory`. `work` holds the download and is deleted after.
// `cancel` stops the download and the unpack, which throw Canceled and change
// nothing. The file swap after them is never canceled, so it never stops halfway.
// `progress` goes from 0 to 1. The download fills the first 90 percent.
void install_release(const Release& release, const std::filesystem::path& install_directory,
    const std::filesystem::path& work, const std::atomic<bool>* cancel = nullptr,
    std::atomic<float>* progress = nullptr);

// Copies every file in `staging` into `install`, keeping the folder layout. Each
// file is first written beside its target under a temporary name, then renamed
// over it, so a running binary keeps its old file and a failed copy changes
// nothing. Files in `install` that are not in `staging` stay. Throws if
// `staging` lacks knockthru, or if knockthru-ctl is not an identical copy of it.
void replace_files(const std::filesystem::path& staging, const std::filesystem::path& install);

} // namespace knockthru
