// Update tests: version order, manifest parsing, SHA-256, and the file swap,
// in a scratch directory. The install test reads a local ZIP through curl's file:// support.
#include "update.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

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

    check(run_program({"true"}) == 0 && run_program({"false"}) == 1, "run_program returns exit codes");
    check(run_program({"knockthru-no-such-program"}) == -1, "run_program returns -1 for a missing program");

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
