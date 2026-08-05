#pragma once
#include <memory>
#include <string>
#include <vector>

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

} // namespace sv
