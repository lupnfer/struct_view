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

// Route A Builder: compiles a ConfigAst to RecipeA (explicit four-kind steps).
// Mirrors Builder (Route B)'s 3-pass structure, but emits StepA with the
// connector literal pre-extracted (no ConnectorProvider heap alloc) and the
// struct-block binding inlined as a SubRecipeBinding (no StructBlockProvider
// virtual indirection). RCU §3.4a: each SubRecipeBinding holds a shared_ptr to
// its sub-recipe (recipe-private, bound once, immutable after publish).
class BuilderA {
public:
    struct Result {
        bool ok = false;
        std::vector<ConfigError> errors;
        std::unordered_map<std::string, std::shared_ptr<const RecipeA>> recipes;
    };
    // Assumes ast already passed Validator. Compiles each recipe to RecipeA
    // (explicit four-kind steps), recipe-private SubRecipeBindings (RCU §3.4a).
    static Result compile(const ConfigAst& ast,
                          const NameRegistry& names,
                          const ConnectorLib& connectors);
};

} // namespace sv
