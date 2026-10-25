#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace knockthru {

// GitHub redirects this to the newest v* release's manifest.
constexpr const char* kLatestManifestUrl =
    "https://github.com/VioletTides/KnockThru/releases/latest/download/knockthru.framedrop.json";

struct Release {
    std::string version; // from the ZIP name, knockthru-X.Y.Z-linux-arm64.zip
    std::string url;
    std::string sha256;
};

// Compares number by number, like strcmp: 0.2.0 < 0.10.1-rc1 < 0.10.1.
int compare_versions(const std::string& a, const std::string& b);

// Throws without a usable ZIP entry. ZIP URLs must be HTTPS, or file:// with `allow_local`.
Release parse_manifest(const std::string& text, bool allow_local = false);

// Lowercase hex.
std::string sha256_file(const std::filesystem::path& path);
std::string sha256(const std::string& data);

struct Canceled : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Runs a program from PATH without a shell, output to /dev/null, and returns its exit
// code, -1 if it never started, or -2 if `cancel` stopped it. With `cancel`, it polls
// every 50 ms and calls `on_poll` each time.
int run_program(const std::vector<std::string>& args, const std::atomic<bool>* cancel = nullptr,
    const std::function<void()>& on_poll = {});

// In its own session, so it outlives this process.
void start_detached(const std::vector<std::string>& args);

// Fetches the manifest with curl. KNOCKTHRU_UPDATE_MANIFEST replaces the GitHub URL,
// to test a release before it is published, and may point at file:// manifests and ZIPs.
Release fetch_latest_release(const std::filesystem::path& work, const std::atomic<bool>* cancel = nullptr);

// Downloads, checks the SHA-256, unpacks, then calls replace_files. `cancel` can stop the
// download and unpack (Canceled, nothing changed) but never the swap, so it cannot stop
// halfway. `progress` runs 0 to 1, and the download is the first 0.9.
void install_release(const Release& release, const std::filesystem::path& install_directory,
    const std::filesystem::path& work, const std::atomic<bool>* cancel = nullptr,
    std::atomic<float>* progress = nullptr);

// Writes each file beside its target under a temporary name and renames it over, so a
// running binary keeps its old inode and a failed copy changes nothing. Extra files in
// `install` stay. Throws unless knockthru-ctl is a byte-identical copy of knockthru.
void replace_files(const std::filesystem::path& staging, const std::filesystem::path& install);

} // namespace knockthru
