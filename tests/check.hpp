#pragma once

// The pass and fail report and the file and exception helpers the test programs share.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

inline int failures = 0;

inline void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

inline std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// Replaces the file's contents, making its folder first.
inline void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::trunc | std::ios::binary) << text;
}

// True if f throws a std::exception whose message contains `substring`.
template <typename F> bool expect_throw(F f, const std::string& substring = "") {
    try {
        f();
    } catch (const std::exception& error) {
        return std::string(error.what()).find(substring) != std::string::npos;
    }
    return false;
}

// Prints the summary. Returns the exit code for main().
inline int finish() {
    std::cout << (failures ? std::to_string(failures) + " failed\n" : "all passed\n");
    return failures ? 1 : 0;
}
