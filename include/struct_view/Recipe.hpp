#pragma once
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include "ValueProvider.hpp"

namespace sv {

class ValueProvider;  // forward

// Route B step: every segment is a ValueProvider.
struct StepB {
    const ValueProvider* provider = nullptr;
};

struct RecipeB {
    std::string name;
    std::vector<StepB> steps;
    // Owns ConnectorProviders (literals/connectors) AND this recipe's private
    // StructBlockProvider instances (each holding a shared_ptr<const RecipeB> to
    // its sub-recipe). Recipe-private struct-block binding = spec §3.4a Rule 1.
    // Field/device providers are owned by NameRegistry (longer-lived, read-only).
    std::vector<std::unique_ptr<ValueProvider>> ownedProviders;
};

// ---- Route A: explicit four-kind step (no virtual call for connectors) ----
// Alternative representation for benchmarking (spec §3.2 dual-track). Coexists
// with Route B; both compile from the same ConfigAst via different Builders.
// Route A's win: ConnectorBinding holds the literal by value (no virtual call,
// no heap) and SubRecipeBinding inlines nav + sub-recipe ptr (the sub-recipe
// walk is a direct function call, not through a virtual StructBlockProvider).
enum class StepKind { Field, Connector, Device, SubRecipe };

struct FieldBinding     { const ValueProvider* provider; };  // scalar field (one virtual call)
struct ConnectorBinding { std::string literal; };            // pre-extracted literal (no virtual)
struct DeviceBinding    { const ValueProvider* provider; };  // device getter (one virtual call)
struct RecipeA;  // forward for SubRecipeBinding
struct SubRecipeBinding {
    Navigator nav;
    std::vector<const ValueProvider*> fieldProviders;   // block fields (in order)
    std::string sep;                                     // field separator
};

struct StepA {
    StepKind kind;
    std::variant<FieldBinding, ConnectorBinding, DeviceBinding, SubRecipeBinding> binding;
};

struct RecipeA {
    std::string name;
    std::vector<StepA> steps;
    // Owns ArrayStructBlockProviderA instances for struct-array steps (Route A
    // reuses the provider via FieldBinding, spec §5). Field/Device bindings hold
    // raw ptrs into NameRegistry-owned providers (longer-lived, read-only);
    // ConnectorBinding holds the literal by value (no heap); SubRecipeBinding
    // holds the block's field provider list + sep (no sub-recipe, spec §4).
    std::vector<std::unique_ptr<ValueProvider>> ownedProviders;
};

} // namespace sv
