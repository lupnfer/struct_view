#pragma once
#include "ConfigAst.hpp"
#include "Errors.hpp"
#include "NameRegistry.hpp"
#include "ConnectorLib.hpp"
#include "RecipeStore.hpp"
#include "Recipe.hpp"

namespace sv {

class Builder {
public:
    struct Result {
        bool ok = false;
        std::vector<ConfigError> errors;
        RecipeStore<RecipeB> store;
    };
    // Assumes ast already passed Validator. Compiles each recipe, binds struct
    // blocks, publishes all. Returns ok=true with store populated on success.
    static Result compile(const ConfigAst& ast,
                          const NameRegistry& names,
                          const ConnectorLib& connectors);
};

} // namespace sv
