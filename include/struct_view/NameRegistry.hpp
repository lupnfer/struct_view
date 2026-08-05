#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "ValueProvider.hpp"

namespace sv {

// Struct-block declaration stored in NameRegistry: navigation + sub-recipe name.
// Used by Validator (existence/cycle checks) and Builder (to instantiate a
// recipe-private StructBlockProvider per compiled recipe). The registry does NOT
// own a bound StructBlockProvider — binding is recipe-private (spec §3.4a Rule 1).
struct StructBlockDecl {
    Navigator nav;
    std::string subRecipeName;
};

// Struct-array declaration: indexed nav + sub-recipe name + count + sep.
// Same pattern as StructBlockDecl — declaration only, no bound provider here
// (binding is recipe-private, spec §3.4a Rule 1).
struct StructArrayDecl {
    IndexedNavigator nav;
    std::string subRecipeName;
    std::size_t count;
    std::string sep;
};

class NameRegistry {
    // Scalar fields / device getters (codegen emits makeProvider lambdas).
    std::unordered_map<std::string, std::unique_ptr<ValueProvider>> entries_;
    // Struct-block declarations (nav + subRecipe name); no bound providers here.
    std::vector<std::pair<std::string, StructBlockDecl>> structDecls_;
    // Struct-array declarations (indexed nav + subRecipe + count + sep).
    std::vector<std::pair<std::string, StructArrayDecl>> structArrayDecls_;
public:
    // For fields and device getters (codegen emits makeProvider lambdas).
    void registerProvider(std::string name, std::unique_ptr<ValueProvider> vp);
    // For key struct blocks: declare navigator + sub-recipe name (resolved per
    // recipe by Builder, not stored bound here).
    void registerStruct(std::string name, Navigator nav, std::string subRecipeName);

    // Scalar fields / device getters only (struct blocks are NOT in entries_).
    const ValueProvider* lookup(const std::string& name) const;

    // For Validator (existence/cycle checks) and Builder (instantiate per-recipe
    // StructBlockProviders).
    const std::vector<std::pair<std::string, StructBlockDecl>>& structDecls() const;

    // For struct arrays (codegen emits registerStructArray). Declaration only.
    void registerStructArray(std::string name, IndexedNavigator nav,
                             std::string subRecipeName, std::size_t count, std::string sep);
    // For Validator (existence/cycle) and Builder (instantiate per-recipe
    // ArrayStructBlockProviders).
    const std::vector<std::pair<std::string, StructArrayDecl>>& structArrayDecls() const;
};

} // namespace sv
