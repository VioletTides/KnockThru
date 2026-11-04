#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace knockthru {

std::optional<std::size_t> content_length(std::string headers) {
    std::transform(
        headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto at = ("\n" + headers).find("\ncontent-length:");
    if (at == std::string::npos) return std::nullopt;
    return std::strtoull(headers.c_str() + at + 15, nullptr, 10);
}

} // namespace knockthru
