#include <struct_view/ConnectorLib.hpp>
namespace sv {
void ConnectorLib::add(std::string name, std::string_view literal) {
    entries_[std::move(name)] = std::string(literal);
}
std::optional<std::string> ConnectorLib::get(const std::string& name) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}
} // namespace sv
