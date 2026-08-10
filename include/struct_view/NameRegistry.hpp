#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "ValueProvider.hpp"

namespace sv {

// Struct-block declaration stored in NameRegistry: navigation + block field
// expansion order + separator + optional description. Used by Validator
// (existence/cycle checks) and Builder (to render the block's fields). The
// registry does NOT own a bound StructBlockProvider — binding is recipe-private
// (spec §3.4a Rule 1). spec §4.1 StructBlockDecl fields+sep, §3.2 desc storage.
struct StructBlockDecl {
    Navigator nav;
    std::vector<std::string> fieldNames;   // block field expansion order (replaces subRecipeName)
    std::string sep;                        // separator between block fields
    std::string desc;                       // optional description (for client display)
};

// Struct-array declaration: indexed nav + per-element field expansion order +
// count + element-internal field separator + element-between separator + optional
// description. Same pattern as StructBlockDecl — declaration only, no bound
// provider here (binding is recipe-private, spec §3.4a Rule 1).
struct StructArrayDecl {
    IndexedNavigator nav;
    std::vector<std::string> fieldNames;   // per-element field expansion order
    std::size_t count;
    std::string sep;                        // element-internal field separator
    std::string arraySep;                   // element-between separator
    std::string desc;
};

// Scalar-array declaration: element formatter + count + default sep + desc.
// Builder creates recipe-private ScalarArrayProvider per compiled recipe (sep
// can be overridden via ${name:sep=xxx}, spec §5 of sep-override spec).
struct ScalarArrayDecl {
    std::function<std::string(const void*, std::size_t)> elemFn;
    std::size_t count;
    std::string sep;     // default sep (fallback when no :sep= override)
    std::string desc;
};

class NameRegistry {
    // Scalar fields / device getters (codegen emits makeProvider lambdas).
    std::unordered_map<std::string, std::unique_ptr<ValueProvider>> entries_;
    // Struct-block declarations (nav + fieldNames + sep + desc); no bound providers here.
    std::vector<std::pair<std::string, StructBlockDecl>> structDecls_;
    // Struct-array declarations (indexed nav + fieldNames + count + sep + arraySep + desc).
    std::vector<std::pair<std::string, StructArrayDecl>> structArrayDecls_;
    // Scalar-array declarations (elemFn + count + default sep + desc).
    std::vector<std::pair<std::string, ScalarArrayDecl>> scalarArrayDecls_;
    // name → desc for all registered things (fields/blocks/devices). spec §3.2.
    std::unordered_map<std::string, std::string> descs_;
public:
    // For fields and device getters (codegen emits makeProvider lambdas).
    // desc is optional (default empty for backward compat). spec §8.1.
    void registerProvider(std::string name, std::unique_ptr<ValueProvider> vp, std::string desc = "");
    // For key struct blocks: declare navigator + block field expansion order +
    // separator + optional desc (resolved per recipe by Builder, not stored bound
    // here). spec §4.1 StructBlockDecl fields+sep, §8.1 register signatures.
    void registerStruct(std::string name, Navigator nav,
                        std::vector<std::string> fieldNames, std::string sep, std::string desc = "");

    // Scalar fields / device getters only (struct blocks are NOT in entries_).
    const ValueProvider* lookup(const std::string& name) const;

    // Returns the desc registered for `name`, or an empty string if none. spec §3.2.
    const std::string& describe(const std::string& name) const;

    // For Validator (existence/cycle checks) and Builder (instantiate per-recipe
    // StructBlockProviders).
    const std::vector<std::pair<std::string, StructBlockDecl>>& structDecls() const;

    // For struct arrays (codegen emits registerStructArray). Declaration only.
    // sep = element-internal field separator; arraySep = element-between separator.
    // spec §8.1 register signatures.
    void registerStructArray(std::string name, IndexedNavigator nav,
                             std::vector<std::string> fieldNames, std::size_t count,
                             std::string sep, std::string arraySep, std::string desc = "");
    // For Validator (existence/cycle) and Builder (instantiate per-recipe
    // ArrayStructBlockProviders).
    const std::vector<std::pair<std::string, StructArrayDecl>>& structArrayDecls() const;

    // For scalar arrays (codegen emits registerScalarArray). Declaration only.
    // Builder creates recipe-private ScalarArrayProvider (sep overridable, spec §5).
    void registerScalarArray(std::string name,
                             std::function<std::string(const void*, std::size_t)> elemFn,
                             std::size_t count, std::string sep, std::string desc = "");
    const std::vector<std::pair<std::string, ScalarArrayDecl>>& scalarArrayDecls() const;

    // --- Lookups (single source of truth; used by Validator + Builder/BuilderA
    //     to avoid duplicating these helpers across 3 files). ---
    bool isStructBlock(const std::string& name) const;   // is `name` a declared struct block?
    bool isStructArray(const std::string& name) const;   // is `name` a declared struct array?
    bool isScalarArray(const std::string& name) const;   // is `name` a declared scalar array?
    const StructBlockDecl* findStructBlockDecl(const std::string& name) const;  // nullptr if absent
    const StructArrayDecl* findStructArrayDecl(const std::string& name) const;  // nullptr if absent
    const ScalarArrayDecl* findScalarArrayDecl(const std::string& name) const;  // nullptr if absent
};

} // namespace sv
