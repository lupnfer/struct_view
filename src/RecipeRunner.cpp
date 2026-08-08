#include <struct_view/RecipeRunner.hpp>
#include <struct_view/Recipe.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <string>
#include <variant>

namespace sv {

std::string runRecipeB(const RecipeB& r, const void* structPtr, const DeviceCtx& ctx) {
    std::string out;
    out.reserve(64);
    for (const StepB& s : r.steps) {
        if (s.provider) out += s.provider->get(structPtr, ctx);
    }
    return out;
}

std::string runRecipeA(const RecipeA& r, const void* structPtr, const DeviceCtx& ctx) {
    std::string out;
    out.reserve(64);
    for (const StepA& s : r.steps) {
        switch (s.kind) {
            case StepKind::Field:
                out += std::get<FieldBinding>(s.binding).provider->get(structPtr, ctx);
                break;
            case StepKind::Connector:
                out += std::get<ConnectorBinding>(s.binding).literal;
                break;
            case StepKind::Device:
                // Device getters ignore structPtr (they use ctx); pass nullptr for clarity.
                out += std::get<DeviceBinding>(s.binding).provider->get(nullptr, ctx);
                break;
            case StepKind::SubRecipe: {
                const auto& b = std::get<SubRecipeBinding>(s.binding);
                const void* child = b.nav.navigate(structPtr);
                for (std::size_t j = 0; j < b.fieldProviders.size(); ++j) {
                    if (j) out += b.sep;
                    out += b.fieldProviders[j]->get(child, ctx);
                }
            } break;
        }
    }
    return out;
}

} // namespace sv
