#include <struct_view/NameRegistry.hpp>
namespace sv {
void NameRegistry::registerProvider(std::string name, std::unique_ptr<ValueProvider> vp, std::string desc) {
    if (!desc.empty()) descs_[name] = desc;          // store desc first (before any move)
    entries_[std::move(name)] = std::move(vp);
}
void NameRegistry::registerStruct(std::string name, Navigator nav,
                                  std::vector<std::string> fieldNames, std::string sep, std::string desc) {
    // Store only the declaration (nav + fieldNames + sep + desc). Builder renders
    // the block's fields per compiled recipe (spec §3.4a Rule 1, §4.1).
    if (!desc.empty()) descs_[name] = desc;          // store desc first (before move into decl)
    StructBlockDecl decl{nav, std::move(fieldNames), std::move(sep), std::move(desc)};
    structDecls_.emplace_back(std::move(name), std::move(decl));
}
const ValueProvider* NameRegistry::lookup(const std::string& name) const {
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : it->second.get();
}
const std::string& NameRegistry::describe(const std::string& name) const {
    auto it = descs_.find(name);
    static const std::string EMPTY;
    return it == descs_.end() ? EMPTY : it->second;
}
const std::vector<std::pair<std::string, StructBlockDecl>>& NameRegistry::structDecls() const {
    return structDecls_;
}
void NameRegistry::registerStructArray(std::string name, IndexedNavigator nav,
                                       std::vector<std::string> fieldNames, std::size_t count,
                                       std::string sep, std::string arraySep, std::string desc) {
    if (!desc.empty()) descs_[name] = desc;          // store desc first (before move into decl)
    StructArrayDecl decl{nav, std::move(fieldNames), count, std::move(sep), std::move(arraySep), std::move(desc)};
    structArrayDecls_.emplace_back(std::move(name), std::move(decl));
}
const std::vector<std::pair<std::string, StructArrayDecl>>& NameRegistry::structArrayDecls() const {
    return structArrayDecls_;
}
bool NameRegistry::isStructBlock(const std::string& name) const {
    return findStructBlockDecl(name) != nullptr;
}
bool NameRegistry::isStructArray(const std::string& name) const {
    return findStructArrayDecl(name) != nullptr;
}
const StructBlockDecl* NameRegistry::findStructBlockDecl(const std::string& name) const {
    for (const auto& d : structDecls_) if (d.first == name) return &d.second;
    return nullptr;
}
const StructArrayDecl* NameRegistry::findStructArrayDecl(const std::string& name) const {
    for (const auto& d : structArrayDecls_) if (d.first == name) return &d.second;
    return nullptr;
}
void NameRegistry::registerScalarArray(std::string name,
                                       std::function<std::string(const void*, std::size_t)> elemFn,
                                       std::size_t count, std::string sep, std::string desc) {
    if (!desc.empty()) descs_[name] = desc;
    ScalarArrayDecl decl{std::move(elemFn), count, std::move(sep), std::move(desc)};
    scalarArrayDecls_.emplace_back(std::move(name), std::move(decl));
}
const std::vector<std::pair<std::string, ScalarArrayDecl>>& NameRegistry::scalarArrayDecls() const {
    return scalarArrayDecls_;
}
bool NameRegistry::isScalarArray(const std::string& name) const {
    return findScalarArrayDecl(name) != nullptr;
}
const ScalarArrayDecl* NameRegistry::findScalarArrayDecl(const std::string& name) const {
    for (const auto& d : scalarArrayDecls_) if (d.first == name) return &d.second;
    return nullptr;
}
std::vector<NameInfo> NameRegistry::exportNames() const {
    std::vector<NameInfo> out;
    for (const auto& [name, _] : entries_) {
        NameInfo info;
        info.name = name;
        info.type = "field";
        info.desc = describe(name);
        out.push_back(std::move(info));
    }
    for (const auto& [name, decl] : structDecls_) {
        NameInfo info;
        info.name = name;
        info.type = "struct_block";
        info.desc = decl.desc;
        info.canSep = true;
        info.defaultSep = decl.sep;
        out.push_back(std::move(info));
    }
    for (const auto& [name, decl] : structArrayDecls_) {
        NameInfo info;
        info.name = name;
        info.type = "struct_array";
        info.desc = decl.desc;
        info.canSep = true;
        info.canIsep = true;
        info.defaultSep = decl.arraySep;
        info.defaultIsep = decl.sep;
        out.push_back(std::move(info));
    }
    for (const auto& [name, decl] : scalarArrayDecls_) {
        NameInfo info;
        info.name = name;
        info.type = "scalar_array";
        info.desc = decl.desc;
        info.canSep = true;
        info.defaultSep = decl.sep;
        out.push_back(std::move(info));
    }
    return out;
}
} // namespace sv
