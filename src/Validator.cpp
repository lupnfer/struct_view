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

static bool isStructBlock(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structDecls()) if (d.first == s) return true;
    return false;
}

// Walk the struct-block -> subRecipe -> referenced structblock graph; detect cycles.
// Edge: struct block X depends on struct block Y if recipe(X.subRecipe) references Y.
static bool hasCycle(const NameRegistry& names,
                     const std::unordered_map<std::string, const RecipeAst*>& byName) {
    std::unordered_set<std::string> visiting, visited;
    // returns true if a cycle is reachable from struct block `blockName`
    std::function<bool(const std::string&)> dfs = [&](const std::string& blockName) -> bool {
        if (visiting.count(blockName)) return true;
        if (visited.count(blockName)) return false;
        visiting.insert(blockName);
        // find this structblock's subRecipe name
        std::string sub;
        for (auto& d : names.structDecls()) if (d.first == blockName) sub = d.second.subRecipeName;
        if (!sub.empty()) {
            auto it = byName.find(sub);
            if (it != byName.end()) {
                for (const auto& seg : it->second->segments) {
                    if (seg.isRef && isStructBlock(names, seg.text)) {
                        if (dfs(seg.text)) { visiting.erase(blockName); visited.insert(blockName); return true; }
                    }
                }
            }
        }
        visiting.erase(blockName);
        visited.insert(blockName);
        return false;
    };
    for (auto& d : names.structDecls()) if (dfs(d.first)) return true;
    return false;
}

std::vector<ConfigError> Validator::validate(const ConfigAst& ast,
                                             const NameRegistry& names,
                                             const ConnectorLib& connectors) {
    std::vector<ConfigError> errs;
    auto byName = indexRecipes(ast);

    // 1. each referenced name resolves to a field/device provider, a connector,
    //    or a struct block (struct blocks live in structDecls, not entries_).
    for (const auto& r : ast.recipes) {
        for (const auto& seg : r.segments) {
            if (!seg.isRef) continue;
            if (!names.lookup(seg.text) && !connectors.get(seg.text) && !isStructBlock(names, seg.text)) {
                errs.push_back({r.name, "unknown name: " + seg.text, 0});
            }
        }
    }

    // 2. each struct block's subRecipe exists as a recipe
    for (auto& d : names.structDecls()) {
        if (!byName.count(d.second.subRecipeName)) {
            errs.push_back({d.first, "struct block subRecipe not found: " + d.second.subRecipeName, 0});
        }
    }

    // 3. no cyclic struct-block dependencies
    if (hasCycle(names, byName)) {
        errs.push_back({"", "cycle detected in struct-block -> subRecipe dependencies", 0});
    }

    return errs;
}

} // namespace sv
