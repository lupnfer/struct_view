#pragma once
#include <string>
#include <vector>

namespace sv {

struct ConfigError {
    std::string recipe;   // recipe name the error is about (empty if global)
    std::string message;  // human-readable
    size_t line = 0;      // 0 if unknown
};

struct LoadResult {
    bool ok = false;
    std::vector<ConfigError> errors;  // empty when ok
};

} // namespace sv
