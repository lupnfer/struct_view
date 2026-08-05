#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "ValueProvider.hpp"

namespace sv {

class NameRegistry {
    std::unordered_map<std::string, std::unique_ptr<ValueProvider>> entries_;
    // Raw pointers into entries_ for struct blocks (lifetime tied to entries_).
    std::vector<std::pair<std::string, StructBlockProvider*>> structBlocks_;
public:
    // For fields and device getters (codegen emits makeProvider lambdas).
    void registerProvider(std::string name, std::unique_ptr<ValueProvider> vp);
    // For key struct blocks: navigator + sub-recipe name (resolved by Builder).
    void registerStruct(std::string name, Navigator nav, std::string subRecipeName);

    const ValueProvider* lookup(const std::string& name) const;

    // For Builder binding and Validator cycle/exists checks.
    const std::vector<std::pair<std::string, StructBlockProvider*>>& structBlocks() const;
};

} // namespace sv
