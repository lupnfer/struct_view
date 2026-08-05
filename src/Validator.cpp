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

static bool isStructArray(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structArrayDecls()) if (d.first == s) return true;
    return false;
}

// Walk the (struct-block ∪ struct-array) -> subRecipe -> referenced-node graph; detect cycles.
// Edge: node X depends on node Y if recipe(X.subRecipe) references Y (Y being a struct block or struct array).
static bool hasCycle(const NameRegistry& names,
                     const std::unordered_map<std::string, const RecipeAst*>& byName) {
    std::unordered_set<std::string> visiting, visited;
    // Look up a node's subRecipe name (searches both struct blocks and struct arrays).
    auto subRecipeOf = [&](const std::string& nm) -> std::string {
        for (auto& d : names.structDecls())      if (d.first == nm) return d.second.subRecipeName;
        for (auto& d : names.structArrayDecls()) if (d.first == nm) return d.second.subRecipeName;
        return "";
    };
    auto isDepNode = [&](const std::string& nm) -> bool {
        return isStructBlock(names, nm) || isStructArray(names, nm);
    };
    std::function<bool(const std::string&)> dfs = [&](const std::string& nm) -> bool {
        if (visiting.count(nm)) return true;
        if (visited.count(nm)) return false;
        visiting.insert(nm);
        std::string sub = subRecipeOf(nm);
        if (!sub.empty()) {
            auto it = byName.find(sub);
            if (it != byName.end()) {
                for (const auto& seg : it->second->segments) {
                    if (seg.isRef && isDepNode(seg.text)) {
                        if (dfs(seg.text)) { visiting.erase(nm); visited.insert(nm); return true; }
                    }
                }
            }
        }
        visiting.erase(nm);
        visited.insert(nm);
        return false;
    };
    for (auto& d : names.structDecls())      if (dfs(d.first)) return true;
    for (auto& d : names.structArrayDecls()) if (dfs(d.first)) return true;
    return false;
}

std::vector<ConfigError> Validator::validate(const ConfigAst& ast,
                                             const NameRegistry& names,
                                             const ConnectorLib& connectors) {
    std::vector<ConfigError> errs;
    auto byName = indexRecipes(ast);

    // 1. each referenced name resolves to a field/device provider, a connector,
    //    a struct block, or a struct array.
    for (const auto& r : ast.recipes) {
        for (const auto& seg : r.segments) {
            if (!seg.isRef) continue;
            if (!names.lookup(seg.text) && !connectors.get(seg.text)
                && !isStructBlock(names, seg.text) && !isStructArray(names, seg.text)) {
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

    // 2b. each struct array's subRecipe exists as a recipe
    for (auto& d : names.structArrayDecls()) {
        if (!byName.count(d.second.subRecipeName)) {
            errs.push_back({d.first, "struct array subRecipe not found: " + d.second.subRecipeName, 0});
        }
    }

    // 3. no cyclic struct-block/struct-array dependencies
    if (hasCycle(names, byName)) {
        errs.push_back({"", "cycle detected in struct-block/struct-array -> subRecipe dependencies", 0});
    }

    return errs;
}

} // namespace sv
