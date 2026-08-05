#include <struct_view/NameRegistry.hpp>
namespace sv {
void NameRegistry::registerProvider(std::string name, std::unique_ptr<ValueProvider> vp) {
    entries_[std::move(name)] = std::move(vp);
}
void NameRegistry::registerStruct(std::string name, Navigator nav, std::string subRecipeName) {
    auto up = std::make_unique<StructBlockProvider>(nav, subRecipeName);
    StructBlockProvider* raw = up.get();
    entries_[name] = std::move(up);
    structBlocks_.emplace_back(std::move(name), raw);
}
const ValueProvider* NameRegistry::lookup(const std::string& name) const {
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : it->second.get();
}
const std::vector<std::pair<std::string, StructBlockProvider*>>& NameRegistry::structBlocks() const {
    return structBlocks_;
}
} // namespace sv
