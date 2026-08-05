#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sv {

// Service-provided connector library: name -> literal/format string.
class ConnectorLib {
    std::unordered_map<std::string, std::string> entries_;
public:
    void add(std::string name, std::string_view literal);
    std::optional<std::string> get(const std::string& name) const;
};

} // namespace sv
