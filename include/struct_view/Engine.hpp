#pragma once
#include <string>
#include <string_view>
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
public:
    Engine(NameRegistry& names, ConnectorLib& connectors);

    // Load/reload config: parse -> validate -> compile -> publish atomically.
    // On any failure returns errors and leaves existing recipes in place.
    LoadResult loadConfig(std::string_view jsonText);

    // Hot path: read-only snapshot, straight-line walk. Never throws.
    std::string render(const std::string& recipeName,
                       const void* structPtr,
                       const DeviceCtx& ctx) const;
};

} // namespace sv
