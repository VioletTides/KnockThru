#pragma once

// The pass and fail report the test programs share.

#include <iostream>
#include <string>

inline int failures = 0;

inline void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (!ok) ++failures;
}

// Prints the summary. Returns the exit code for main().
inline int finish() {
    std::cout << (failures ? std::to_string(failures) + " failed\n" : "all passed\n");
    return failures ? 1 : 0;
}
