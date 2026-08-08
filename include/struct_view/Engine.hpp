#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include "Errors.hpp"
#include "NameRegistry.hpp"
#include "ConnectorLib.hpp"
#include "RecipeStore.hpp"
#include "Recipe.hpp"
#include "DeviceCtx.hpp"

namespace sv {

class Engine {
    NameRegistry& names_;
    ConnectorLib& connectors_;
    RecipeStore<RecipeB> store_;
    RecipeStore<RecipeA> storeA_;
    std::unordered_map<std::string, std::string> recipeDescs_;   // recipe name → desc
public:
    Engine(NameRegistry& names, ConnectorLib& connectors);

    // Load/reload config: parse -> validate -> compile -> publish atomically.
    // On any failure returns errors and leaves existing recipes in place.
    LoadResult loadConfig(std::string_view jsonText);

    // Hot path: read-only snapshot, straight-line walk. Never throws.
    std::string render(const std::string& recipeName,
                       const void* structPtr,
                       const DeviceCtx& ctx) const;

    // Route A (explicit four-kind Step) — alternative representation for
    // benchmarking (spec §3.2 dual-track). Shares parse+validate with Route B;
    // differs only in Builder (BuilderA) and store (storeA_).
    LoadResult loadConfigA(std::string_view jsonText);
    std::string renderA(const std::string& recipeName,
                        const void* structPtr,
                        const DeviceCtx& ctx) const;

    // Returns the desc registered for a recipe, or empty if none (spec §3.2).
    const std::string& describeRecipe(const std::string& recipeName) const;
};

} // namespace sv
