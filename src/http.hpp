#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace knockthru {

// The Content-Length in a block of header lines, or nothing without one.
std::optional<std::size_t> content_length(std::string headers);

} // namespace knockthru
