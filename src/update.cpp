#include "update.hpp"

#include "http.hpp"

#include <json.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
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

struct Response {
    int status = 0;
    std::uintmax_t length = 0;
};

// The status code and Content-Length of the last response in a curl header dump, after
// redirects, or 0 for each. curl writes a Content-Length for file:// URLs too.
Response last_response(const fs::path& headers) {
    std::ifstream stream(headers);
    Response response;
    std::string block;
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind("HTTP/", 0) == 0) {
            std::istringstream fields(line);
            std::string version;
            fields >> version >> response.status;
            block.clear();
        }
        block += line + "\n";
    }
    response.length = content_length(block).value_or(0);
    return response;
}

// Downloads `url` to `path` with curl, and turns failures into readable messages.
// The HTTP status comes from the response headers, because curl's exit code for
// an HTTP error differs between versions: 22 on Arch Linux's curl, 56 on the Frame's.
// While curl runs, `progress` follows the file's size against the Content-Length,
// scaled from 0 to `scale`. Without a Content-Length it stays where it is.
void download(const std::string& url, const fs::path& path, int seconds, const std::string& missing,
    const std::atomic<bool>* cancel, std::atomic<float>* progress = nullptr, float scale = 1) {
    const auto headers = path.string() + ".headers";
    const auto follow = [&] {
        if (!progress) return;
        std::error_code error;
        const auto length = last_response(headers).length;
        const auto size = fs::file_size(path, error);
        if (length > 0 && !error)
            *progress = scale
                * static_cast<float>(std::min(1.0, static_cast<double>(size) / static_cast<double>(length)));
    };
    // Redirects stay on https, so a hijacked redirect cannot serve a ZIP over plain http.
    // file is allowed for the first URL only, which tests and local update trials use.
    const int code = run_program(
        {"curl", "--silent", "--location", "--proto", "=https,file", "--proto-redir", "=https", "--max-time",
            std::to_string(seconds), "--dump-header", headers, "--output", path.string(), url},
        cancel, follow);
    if (code == -2) throw Canceled("Download canceled");
    const int status = last_response(headers).status;
    std::error_code ignored;
    fs::remove(headers, ignored);
    if (status == 404) throw std::runtime_error(missing);
    if (status >= 400) throw std::runtime_error("GitHub answered with HTTP " + std::to_string(status));
    if (code == 0) return; // file:// URLs have no HTTP status, so 0 passes too
    if (code == -1 || code == 127) throw std::runtime_error("curl is missing on this system");
    if (code == 6 || code == 7) throw std::runtime_error("Cannot reach GitHub: check the Wi-Fi");
    if (code == 28) throw std::runtime_error("GitHub took too long to answer");
    throw std::runtime_error("Download failed, curl exit code " + std::to_string(code));
}

} // namespace

int compare_versions(const std::string& a, const std::string& b) {
    const auto split = [](const std::string& version) {
        const auto dash = version.find('-');
        return std::make_pair(
            version.substr(0, dash), dash == std::string::npos ? "" : version.substr(dash + 1));
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

std::vector<VersionNotes> parse_whats_new(const std::string& text) {
    std::vector<VersionNotes> out;
    std::stringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("## ", 0) == 0)
            out.push_back({line.substr(3), {}});
        else if (line.rfind("- ", 0) == 0 && !out.empty())
            out.back().notes.push_back(line.substr(2));
    }
    return out;
}

std::vector<VersionNotes> notes_between(
    const std::vector<VersionNotes>& all, const std::string& from, const std::string& to) {
    std::vector<VersionNotes> out;
    for (const auto& version : all)
        if (compare_versions(version.version, to) <= 0
            && (from.empty() || compare_versions(version.version, from) > 0) && !version.notes.empty())
            out.push_back(version);
    std::stable_sort(out.begin(), out.end(),
        [](const auto& a, const auto& b) { return compare_versions(a.version, b.version) > 0; });
    return out;
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

int run_program(const std::vector<std::string>& args, const std::atomic<bool>* cancel,
    const std::function<void()>& on_poll) {
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
    if (cancel) {
        // Polls instead of blocking, so a cancel can stop the program.
        for (;;) {
            const pid_t done = ::waitpid(pid, &status, WNOHANG);
            if (done == pid) break;
            if (done < 0 && errno != EINTR) return -1;
            if (*cancel) {
                ::kill(pid, SIGTERM);
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                return -2;
            }
            if (on_poll) on_poll();
            ::usleep(50000);
        }
    } else {
        while (::waitpid(pid, &status, 0) < 0)
            if (errno != EINTR) return -1;
    }
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

Release fetch_latest_release(const fs::path& work, const std::atomic<bool>* cancel) {
    fs::remove_all(work);
    fs::create_directories(work);
    const auto path = work / "knockthru.framedrop.json";
    const char* override_url = std::getenv("KNOCKTHRU_UPDATE_MANIFEST");
    const std::string url = override_url && *override_url ? override_url : kLatestManifestUrl;
    try {
        download(url, path, 20, "No KnockThru release is published on GitHub yet", cancel);
        const auto release = parse_manifest(read_file(path), url.rfind("file://", 0) == 0);
        fs::remove_all(work);
        return release;
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(work, ignored);
        throw;
    }
}

void install_release(const Release& release, const fs::path& install_directory, const fs::path& work,
    const std::atomic<bool>* cancel, std::atomic<float>* progress) {
    const auto reach = [&](float value) {
        if (progress) *progress = value;
    };
    reach(0);
    fs::remove_all(work);
    fs::create_directories(work);
    try {
        const auto zip = work / "release.zip";
        download(release.url, zip, 180, "The release ZIP is missing on GitHub", cancel, progress, 0.9f);
        reach(0.9f);
        // Runs a system tool and returns its exit code, unless it is missing or the install was canceled.
        const auto tool = [&](const std::vector<std::string>& args) {
            const int code = run_program(args, cancel);
            if (code == -2) throw Canceled("Install canceled");
            if (code == -1 || code == 127) throw std::runtime_error(args[0] + " is missing on this system");
            return code;
        };
        // sha256sum reads the hash to expect from a file of "hash  path" lines.
        const auto sums = work / "release.zip.sha256";
        std::ofstream(sums) << release.sha256 << "  " << zip.string() << "\n";
        if (tool({"sha256sum", "--status", "--check", sums.string()}) != 0)
            throw std::runtime_error("The download did not match its checksum. Nothing changed");
        const auto staging = work / "files";
        fs::create_directories(staging);
        if (const int code = tool({"unzip", "-q", "-o", zip.string(), "-d", staging.string()}))
            throw std::runtime_error(
                "Cannot unpack the release ZIP, unzip exit code " + std::to_string(code));
        reach(0.95f);
        replace_files(staging, install_directory);
        reach(1);
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
    for (auto it = fs::recursive_directory_iterator(staging); it != fs::recursive_directory_iterator();
        ++it) {
        if (it->is_symlink())
            throw std::runtime_error("The release contains a symlink: " + it->path().string());
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
            written.push_back(temp);
            fs::copy_file(staging / file, temp, fs::copy_options::overwrite_existing);
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
