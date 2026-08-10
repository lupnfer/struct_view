#include <struct_view/Validator.hpp>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace sv {

static std::unordered_map<std::string, const RecipeAst*> indexRecipes(const ConfigAst& ast) {
    std::unordered_map<std::string, const RecipeAst*> m;
    for (const auto& r : ast.recipes) m[r.name] = &r;
    return m;
}

// Walk the user-sub-recipe -> referenced-name -> user-sub-recipe graph; detect cycles.
// Struct blocks/fields are leaves (they don't reference sub-recipes anymore —
// block expansion rule is in the schema, spec §4). Only user sub-recipes (r_ names
// in byName) can form cycles.
static bool hasCycle(const std::unordered_map<std::string, const RecipeAst*>& byName) {
    std::unordered_set<std::string> visiting, visited;
    std::function<bool(const std::string&)> dfs = [&](const std::string& nm) -> bool {
        if (visiting.count(nm)) return true;
        if (visited.count(nm)) return false;
        visiting.insert(nm);
        auto it = byName.find(nm);
        if (it != byName.end()) {
            for (const auto& seg : it->second->segments) {
                if (seg.isRef && byName.count(seg.text)) {   // seg.text is another user sub-recipe?
                    if (dfs(seg.text)) { visiting.erase(nm); visited.insert(nm); return true; }
                }
            }
        }
        visiting.erase(nm);
        visited.insert(nm);
        return false;
    };
    for (const auto& [nm, _] : byName) if (dfs(nm)) return true;
    return false;
}

std::vector<ConfigError> Validator::validate(const ConfigAst& ast,
                                             const NameRegistry& names,
                                             const ConnectorLib& connectors) {
    std::vector<ConfigError> errs;
    auto byName = indexRecipes(ast);

    // 0. recipe names must start with r_; block names must not start with r_.
    for (const auto& r : ast.recipes) {
        if (r.name.rfind("r_", 0) != 0) {
            errs.push_back({r.name, "recipe name must start with 'r_': " + r.name, 0});
        }
    }
    for (const auto& d : names.structDecls()) {
        if (d.first.rfind("r_", 0) == 0) {
            errs.push_back({d.first, "block name must not start with 'r_' (reserved for recipes): " + d.first, 0});
        }
    }
    for (const auto& d : names.structArrayDecls()) {
        if (d.first.rfind("r_", 0) == 0) {
            errs.push_back({d.first, "block name must not start with 'r_' (reserved for recipes): " + d.first, 0});
        }
    }

    // 1. each referenced name resolves to a field/device provider, a connector,
    //    a struct block, a struct array, a scalar array, or a user sub-recipe (byName).
    for (const auto& r : ast.recipes) {
        for (const auto& seg : r.segments) {
            if (!seg.isRef) continue;
            if (!names.lookup(seg.text) && !connectors.get(seg.text)
                && !names.isStructBlock(seg.text) && !names.isStructArray(seg.text)
                && !names.isScalarArray(seg.text) && !byName.count(seg.text)) {
                errs.push_back({r.name, "unknown name: " + seg.text, 0});
            }
        }
    }

    // 1b. :sep=/:isep= overrides: validate they're on separable names.
    for (const auto& r : ast.recipes) {
        for (const auto& seg : r.segments) {
            if (!seg.isRef) continue;
            bool isScalarField = names.lookup(seg.text) != nullptr;   // scalar field/device
            if (!seg.sepOverride.empty() && isScalarField) {
                errs.push_back({r.name, "sep override on non-separable name: " + seg.text, 0});
            }
            if (!seg.isepOverride.empty() && !names.isStructArray(seg.text)) {
                errs.push_back({r.name, "isep override on non-struct-array name: " + seg.text, 0});
            }
        }
    }

    // 2. each struct block's fieldNames must resolve to a registered provider or scalar array.
    for (const auto& d : names.structDecls()) {
        for (const auto& fn : d.second.fieldNames) {
            if (!names.lookup(fn) && !names.isScalarArray(fn)) {
                errs.push_back({d.first, "block field not found in registry: " + fn, 0});
            }
        }
    }
    for (const auto& d : names.structArrayDecls()) {
        for (const auto& fn : d.second.fieldNames) {
            if (!names.lookup(fn) && !names.isScalarArray(fn)) {
                errs.push_back({d.first, "block field not found in registry: " + fn, 0});
            }
        }
    }

    // 3. no cyclic user-sub-recipe dependencies
    if (hasCycle(byName)) {
        errs.push_back({"", "cycle detected in user sub-recipe dependencies", 0});
    }

    return errs;
}

} // namespace sv
