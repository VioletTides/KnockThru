// Update tests: version order, manifest parsing, SHA-256, the file swap, and the issue link,
// in a scratch directory. The install test reads a local ZIP through curl's file:// support.
#include "report.hpp"
#include "report_server.hpp"
#include "update.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace knockthru;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::trunc | std::ios::binary) << text;
}

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

template <typename F> bool throws(F f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

std::string manifest(const std::string& url, const std::string& hash) {
    return R"({"schema": "framedrop.install/v1", "name": "KnockThru", "files": [{"url": ")" + url +
           R"(", "sha256": ")" + hash + R"("}]})";
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path() / ("knockthru-update-test-" + std::to_string(getpid()));
    fs::remove_all(root);

    check(compare_versions("0.2.0", "0.1.0") > 0, "0.2.0 is newer than 0.1.0");
    check(compare_versions("0.10.0", "0.9.9") > 0, "0.10.0 is newer than 0.9.9");
    check(compare_versions("1.0", "1.0.0") == 0, "1.0 equals 1.0.0");
    check(compare_versions("0.2.0-rc1", "0.2.0") < 0, "0.2.0-rc1 is older than 0.2.0");
    check(compare_versions("0.2.0-rc1", "0.1.0") > 0, "0.2.0-rc1 is newer than 0.1.0");

    const std::string hash(64, 'a');
    const auto url = "https://github.com/VioletTides/KnockThru/releases/download/v0.2.0/knockthru-0.2.0-linux-arm64.zip";
    const auto release = parse_manifest(manifest(url, hash));
    check(release.version == "0.2.0" && release.url == url && release.sha256 == hash, "manifest gives version, URL, and hash");
    check(throws([&] { parse_manifest(manifest("http://example.com/knockthru-0.2.0-linux-arm64.zip", hash)); }),
        "manifest rejects a plain HTTP URL");
    check(throws([&] { parse_manifest(manifest(url, "ABC")); }), "manifest rejects a malformed hash");
    const auto local_zip = "file:///tmp/knockthru-0.2.0-linux-arm64.zip";
    check(throws([&] { parse_manifest(manifest(local_zip, hash)); }), "manifest rejects a file:// ZIP by default");
    check(parse_manifest(manifest(local_zip, hash), true).version == "0.2.0", "manifest accepts a file:// ZIP for tests");
    check(throws([] { parse_manifest("not json"); }), "manifest rejects invalid JSON");

    check(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
    check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    check(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "SHA-256 across a block boundary");

    check(url_encode("a b/é") == "a%20b%2F%C3%A9", "url_encode escapes spaces, slashes, and UTF-8");
    check(issue_url({"0.1.0", "2.18.2", "on", ""}) ==
              "https://github.com/VioletTides/KnockThru/issues/new?template=bug.yml&version=0.1.0&steamvr=2.18.2&passthrough=on",
        "issue_url fills the bug form");
    check(issue_url({"0.1.0", "2.18.2", "on", "Style not changed"}).find("&last-error=Style%20not%20changed") !=
              std::string::npos,
        "issue_url adds the last error");
    {
        std::string big = "KnockThru: 0.1.0\n";
        for (int i = 0; i < 2000; ++i) big += "2026-10-15 21:50:36  log line " + std::to_string(i) + " with some padding text\n";
        const auto fitted = fit_report(big, 20000);
        check(fit_report("short\n") == "short\n", "fit_report keeps a short report");
        check(fitted.size() <= 20000, "fit_report stays under the limit: " + std::to_string(fitted.size()));
        check(fitted.rfind("KnockThru: 0.1.0\n", 0) == 0 && fitted.find("log line 1999 with") != std::string::npos &&
                  fitted.find("characters of older log lines cut") != std::string::npos,
            "fit_report keeps the start and the newest lines, and marks the cut");
    }
    const auto long_url = issue_url({"0.1.0", "2.18.2", std::string(300, 'x'), ""});
    check(long_url.find(std::string(100, 'x')) != std::string::npos &&
              long_url.find(std::string(101, 'x')) == std::string::npos,
        "issue_url cuts long fields to 100 characters");

    {
        // The report server answers its page and file, and nothing else. curl fetches them.
        ReportServer server("line one\n<b>log</b>\n", issue_url({"0.1.0", "2.18.2", "on", ""}));
        const auto page = root / "page.html", file = root / "report.txt", other = root / "other";
        fs::create_directories(root);
        const auto get = [](const std::string& url, const fs::path& out) {
            return run_program({"curl", "--fail", "--silent", "--max-time", "5", "--output", out.string(), url});
        };
        check(server.url().rfind("http://", 0) == 0, "report server has an http link: " + server.url());
        check(get(server.url(), page) == 0 && read(page).find("&lt;b&gt;log&lt;/b&gt;") != std::string::npos,
            "report server serves the page with the report escaped");
        check(read(page).find("const link = \"https://github.com/VioletTides/KnockThru/issues/new?template=bug.yml&version=") !=
                  std::string::npos,
            "report page opens the prefilled GitHub form");
        check(get(server.url() + "/report/knockthru-report.txt", file) == 0 && read(file) == "line one\n<b>log</b>\n",
            "report server serves the report file");
        const auto base = server.url().substr(0, server.url().rfind('/'));
        check(get(base + "/0000000000000000", other) == 22, "report server refuses a wrong token");
        check(read(page).find("knock recording") == std::string::npos, "report page has no recording link without one");
        check(get(server.url() + "/report/knockthru-knocks.csv.gz", other) == 22, "report server has no recording yet");
        server.set_report("newer report\n");
        const auto recording = root / "knocks.csv.gz";
        write(recording, "gz bytes");
        server.set_recording(recording);
        check(get(server.url(), page) == 0 && read(page).find("Download the knock recording") != std::string::npos &&
                  read(page).find("newer report") != std::string::npos,
            "report page shows a new report and the recording link");
        check(get(server.url() + "/report/knockthru-knocks.csv.gz", other) == 0 && read(other) == "gz bytes",
            "report server serves the recording");
    }

    check(run_program({"true"}) == 0 && run_program({"false"}) == 1, "run_program returns exit codes");
    check(run_program({"knockthru-no-such-program"}) == -1, "run_program returns -1 for a missing program");
    {
        // start_detached runs the program in a new session without the caller's descriptors.
        fs::create_directories(root);
        const auto out = root / "detached.txt";
        // Fd 50, because ls itself opens a low descriptor to read /proc/self/fd. Not close-on-exec.
        const int opened = ::open((root / "held").c_str(), O_CREAT | O_WRONLY, 0644);
        const int held = ::dup2(opened, 50);
        ::close(opened);
        start_detached({"/bin/sh", "-c", "ls /proc/self/fd > '" + out.string() + "'; ps -o sid= -p $$ >> '" +
                                             out.string() + "'"});
        for (int i = 0; i < 50 && read(out).find('\n') == std::string::npos; ++i) usleep(20000);
        usleep(100000);
        const auto listing = read(out);
        check(listing.find(std::to_string(held) + "\n") == std::string::npos,
            "start_detached closes inherited descriptors");
        ::close(held);
    }

    // The file swap: new files land, extra files stay, modes carry over.
    const auto staging = root / "staging", install = root / "install";
    write(staging / "knockthru", "new binary");
    write(staging / "knockthru-ctl", "new binary");
    write(staging / "fonts" / "a.ttf", "new font");
    fs::permissions(staging / "knockthru", fs::perms(0755));
    write(install / "knockthru", "old binary");
    write(install / "calibration-notes.txt", "keep me");
    replace_files(staging, install);
    check(read(install / "knockthru") == "new binary" && read(install / "knockthru-ctl") == "new binary",
        "replace_files puts in both programs");
    check(read(install / "fonts" / "a.ttf") == "new font", "replace_files makes missing folders");
    check(read(install / "calibration-notes.txt") == "keep me", "replace_files keeps other files");
    check((fs::status(install / "knockthru").permissions() & fs::perms::owner_exec) != fs::perms::none,
        "replace_files keeps the executable bit");
    check(!fs::exists(install / ".knockthru.new"), "replace_files leaves no temporary files");

    write(staging / "knockthru-ctl", "different");
    check(throws([&] { replace_files(staging, install); }), "replace_files rejects a knockthru-ctl that differs");
    check(read(install / "knockthru") == "new binary", "a rejected release changes nothing");

    // A full install from a local ZIP. python3 builds the ZIP, the way a release has it.
    write(staging / "knockthru-ctl", "new binary");
    write(staging / "knockthru", "newer binary");
    write(staging / "knockthru-ctl", "newer binary");
    const auto zip = root / "knockthru-0.3.0-linux-arm64.zip";
    if (run_program({"python3", "-c",
            "import sys, zipfile, pathlib\n"
            "root = pathlib.Path(sys.argv[2])\n"
            "with zipfile.ZipFile(sys.argv[1], 'w') as z:\n"
            "    for p in sorted(root.rglob('*')):\n"
            "        if p.is_file(): z.write(p, p.relative_to(root).as_posix())\n",
            zip.string(), staging.string()}) != 0) {
        check(false, "python3 builds a test ZIP");
    } else {
        Release local{"0.3.0", "file://" + zip.string(), sha256(read(zip))};
        Release bad = local;
        bad.sha256 = std::string(64, '0');
        check(throws([&] { install_release(bad, install, root / "work"); }), "install_release rejects a SHA-256 mismatch");
        check(read(install / "knockthru") == "new binary", "a mismatched ZIP changes nothing");
        check(!fs::exists(root / "work"), "install_release deletes its download after a failure");
        install_release(local, install, root / "work");
        check(read(install / "knockthru") == "newer binary", "install_release installs a matching ZIP");
        check(!fs::exists(root / "work"), "install_release deletes its download");
    }

    fs::remove_all(root);
    if (failures) std::cout << failures << " update checks failed\n";
    return failures ? 1 : 0;
}
