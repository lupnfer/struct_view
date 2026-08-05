#pragma once
#include <memory>
#include <unordered_map>
#include <vector>
#include "ConfigAst.hpp"
#include "Errors.hpp"
#include "NameRegistry.hpp"
#include "ConnectorLib.hpp"
#include "Recipe.hpp"

namespace sv {

class Builder {
public:
    struct Result {
        bool ok = false;
        std::vector<ConfigError> errors;
        // Compiled, immutable recipes by name. Engine publishes them into its
        // stable RecipeStore via publishAll (spec §3.4a Rule 2 / §4.3). Each
        // recipe is a self-contained graph: struct-block providers it owns hold
        // shared_ptr<const RecipeB> to their sub-recipes (Rule 1/3).
        std::unordered_map<std::string, std::shared_ptr<const RecipeB>> recipes;
    };
    // Assumes ast already passed Validator. Compiles each recipe, instantiates
    // recipe-private struct-block providers (sub bound once at compile time),
    // returns the recipe map on success.
    static Result compile(const ConfigAst& ast,
                          const NameRegistry& names,
                          const ConnectorLib& connectors);
};

} // namespace sv
