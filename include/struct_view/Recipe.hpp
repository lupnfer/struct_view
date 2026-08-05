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
    // Owns ConnectorProviders created for this recipe's literals/connectors.
    // Field/device/structblock providers are owned by NameRegistry (longer-lived).
    std::vector<std::unique_ptr<ValueProvider>> ownedProviders;
};

} // namespace sv
