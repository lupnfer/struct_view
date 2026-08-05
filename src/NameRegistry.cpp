#include <struct_view/NameRegistry.hpp>
namespace sv {
void NameRegistry::registerProvider(std::string name, std::unique_ptr<ValueProvider> vp) {
    entries_[std::move(name)] = std::move(vp);
}
void NameRegistry::registerStruct(std::string name, Navigator nav, std::string subRecipeName) {
    // Store only the declaration (nav + subRecipeName). Builder instantiates a
    // recipe-private StructBlockProvider per compiled recipe (spec §3.4a Rule 1).
    structDecls_.emplace_back(std::move(name),
                              StructBlockDecl{std::move(nav), std::move(subRecipeName)});
}
const ValueProvider* NameRegistry::lookup(const std::string& name) const {
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : it->second.get();
}
const std::vector<std::pair<std::string, StructBlockDecl>>& NameRegistry::structDecls() const {
    return structDecls_;
}
void NameRegistry::registerStructArray(std::string name, IndexedNavigator nav,
                                       std::string subRecipeName, std::size_t count, std::string sep) {
    StructArrayDecl decl{nav, subRecipeName, count, sep};
    structArrayDecls_.emplace_back(std::move(name), std::move(decl));
}
const std::vector<std::pair<std::string, StructArrayDecl>>& NameRegistry::structArrayDecls() const {
    return structArrayDecls_;
}
} // namespace sv
