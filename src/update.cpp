#include "update.hpp"

#include <json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <regex>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace knockthru {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::vector<long> numbers(const std::string& core) {
    std::vector<long> parts;
    std::stringstream stream(core);
    for (std::string part; std::getline(stream, part, '.');) {
        try {
            parts.push_back(std::stol(part));
        } catch (const std::exception&) {
            parts.push_back(0);
        }
    }
    return parts;
}

// SHA-256 as in FIPS 180-4.
class Sha256 {
public:
    void add(const unsigned char* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            block_[used_++] = data[i];
            if (used_ == 64) {
                compress();
                used_ = 0;
            }
        }
        length_ += size;
    }

    std::string hex() {
        const std::uint64_t bits = length_ * 8;
        const unsigned char one = 0x80, zero = 0;
        add(&one, 1);
        while (used_ != 56) add(&zero, 1);
        for (int shift = 56; shift >= 0; shift -= 8) {
            const auto byte = static_cast<unsigned char>(bits >> shift);
            add(&byte, 1);
        }
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (const auto word : state_)
            for (int shift = 28; shift >= 0; shift -= 4) out += digits[(word >> shift) & 0xF];
        return out;
    }

private:
    static std::uint32_t rotate(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress() {
        static constexpr std::array<std::uint32_t, 64> k{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t{block_[4 * i]} << 24) | (std::uint32_t{block_[4 * i + 1]} << 16) |
                   (std::uint32_t{block_[4 * i + 2]} << 8) | std::uint32_t{block_[4 * i + 3]};
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = state_;
        for (int i = 0; i < 64; ++i) {
            const auto t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const auto t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        const std::array<std::uint32_t, 8> add{a, b, c, d, e, f, g, h};
        for (int i = 0; i < 8; ++i) state_[i] += add[i];
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t length_ = 0;
};

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::vector<char*> argv_of(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    return argv;
}

// Downloads `url` to `path` with curl. curl's exit codes become readable messages.
void download(const std::string& url, const fs::path& path, int seconds, const std::string& missing) {
    const int code = run_program({"curl", "--fail", "--silent", "--show-error", "--location", "--max-time",
                                  std::to_string(seconds), "--output", path.string(), url});
    if (code == 0) return;
    if (code == -1 || code == 127) throw std::runtime_error("curl is missing on this system");
    if (code == 6 || code == 7) throw std::runtime_error("Cannot reach GitHub. Check the Wi-Fi connection.");
    if (code == 28) throw std::runtime_error("GitHub took too long to answer");
    if (code == 22) throw std::runtime_error(missing);
    throw std::runtime_error("Download failed, curl exit code " + std::to_string(code));
}

} // namespace

int compare_versions(const std::string& a, const std::string& b) {
    const auto split = [](const std::string& version) {
        const auto dash = version.find('-');
        return std::make_pair(version.substr(0, dash), dash == std::string::npos ? "" : version.substr(dash + 1));
    };
    const auto [core_a, suffix_a] = split(a);
    const auto [core_b, suffix_b] = split(b);
    const auto na = numbers(core_a), nb = numbers(core_b);
    for (std::size_t i = 0; i < std::max(na.size(), nb.size()); ++i) {
        const long x = i < na.size() ? na[i] : 0, y = i < nb.size() ? nb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (suffix_a == suffix_b) return 0;
    if (suffix_a.empty()) return 1;
    if (suffix_b.empty()) return -1;
    return suffix_a < suffix_b ? -1 : 1;
}

Release parse_manifest(const std::string& text, bool allow_local) {
    json manifest;
    try {
        manifest = json::parse(text);
    } catch (const std::exception&) {
        throw std::runtime_error("The release manifest is not valid JSON");
    }
    if (!manifest.is_object() || !manifest.contains("files") || !manifest["files"].is_array())
        throw std::runtime_error("The release manifest has no files");
    static const std::regex zip_name(R"(/knockthru-([0-9][0-9A-Za-z.-]*)-linux-arm64\.zip$)");
    for (const auto& file : manifest["files"]) {
        if (!file.is_object()) continue;
        const auto url = file.value("url", "");
        const auto hash = file.value("sha256", "");
        std::smatch match;
        const bool secure = url.rfind("https://", 0) == 0 || (allow_local && url.rfind("file://", 0) == 0);
        if (!secure || !std::regex_search(url, match, zip_name)) continue;
        if (!std::regex_match(hash, std::regex("[0-9a-f]{64}"))) continue;
        return {match[1], url, hash};
    }
    throw std::runtime_error("The release manifest has no Steam Frame ZIP");
}

std::string sha256(const std::string& data) {
    Sha256 hash;
    hash.add(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return hash.hex();
}

std::string sha256_file(const fs::path& path) { return sha256(read_file(path)); }

int run_program(const std::vector<std::string>& args) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    auto argv = argv_of(args);
    pid_t pid = 0;
    const int error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void start_detached(const std::vector<std::string>& args) {
    // fork instead of posix_spawn, so the child can close every descriptor it
    // inherited, such as the caller's SteamVR connection, before it runs.
    auto argv = argv_of(args);
    const long max_fd = std::max(::sysconf(_SC_OPEN_MAX), 1024L);
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("Cannot start " + args[0]);
    if (pid == 0) {
        // Only async-signal-safe calls from here, because the parent has threads.
        ::setsid();
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            ::dup2(null, STDIN_FILENO);
            ::dup2(null, STDOUT_FILENO);
            ::dup2(null, STDERR_FILENO);
        }
        for (long fd = 3; fd < max_fd; ++fd) ::close(static_cast<int>(fd));
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    // The child runs on after this process exits. Its exit status is never read.
}

Release fetch_latest_release(const fs::path& work) {
    fs::remove_all(work);
    fs::create_directories(work);
    const auto path = work / "knockthru.framedrop.json";
    const char* override_url = std::getenv("KNOCKTHRU_UPDATE_MANIFEST");
    const std::string url = override_url && *override_url ? override_url : kLatestManifestUrl;
    try {
        download(url, path, 20, "No KnockThru release is published on GitHub yet");
        const auto release = parse_manifest(read_file(path), url.rfind("file://", 0) == 0);
        fs::remove_all(work);
        return release;
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(work, ignored);
        throw;
    }
}

void install_release(const Release& release, const fs::path& install_directory, const fs::path& work) {
    fs::remove_all(work);
    fs::create_directories(work);
    try {
        const auto zip = work / "release.zip";
        download(release.url, zip, 180, "The release ZIP is missing on GitHub");
        if (sha256_file(zip) != release.sha256)
            throw std::runtime_error("The downloaded ZIP does not match its SHA-256. Nothing changed.");
        const auto staging = work / "files";
        fs::create_directories(staging);
        const int code = run_program({"unzip", "-q", "-o", zip.string(), "-d", staging.string()});
        if (code == -1 || code == 127) throw std::runtime_error("unzip is missing on this system");
        if (code != 0) throw std::runtime_error("Cannot unpack the release ZIP, unzip exit code " + std::to_string(code));
        replace_files(staging, install_directory);
        fs::remove_all(work);
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(work, ignored);
        throw;
    }
}

void replace_files(const fs::path& staging, const fs::path& install) {
    const auto app = staging / "knockthru", ctl = staging / "knockthru-ctl";
    if (!fs::is_regular_file(app)) throw std::runtime_error("The release has no knockthru program");
    if (!fs::is_regular_file(ctl) || read_file(app) != read_file(ctl))
        throw std::runtime_error("The release's knockthru-ctl is not a copy of knockthru");

    // The programs go last, so a failure partway leaves the old app with at most new assets.
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(staging); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_symlink()) throw std::runtime_error("The release contains a symlink: " + it->path().string());
        if (it->is_regular_file()) files.push_back(fs::relative(it->path(), staging));
    }
    std::stable_partition(files.begin(), files.end(),
        [](const fs::path& file) { return file != "knockthru" && file != "knockthru-ctl"; });

    const auto temporary = [&](const fs::path& file) {
        const auto target = install / file;
        return target.parent_path() / ("." + target.filename().string() + ".new");
    };
    std::vector<fs::path> written;
    try {
        for (const auto& file : files) {
            const auto target = install / file;
            fs::create_directories(target.parent_path());
            const auto temp = temporary(file);
            fs::copy_file(staging / file, temp, fs::copy_options::overwrite_existing);
            written.push_back(temp);
            fs::permissions(temp, fs::status(staging / file).permissions());
        }
    } catch (...) {
        std::error_code ignored;
        for (const auto& temp : written) fs::remove(temp, ignored);
        throw;
    }
    for (const auto& file : files) fs::rename(temporary(file), install / file);
}

} // namespace knockthru
