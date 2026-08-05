#include <struct_view/ValueProvider.hpp>
#include <struct_view/Recipe.hpp>
#include <struct_view/RecipeRunner.hpp>

namespace sv {

std::string StructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    // sub_ is bound at construction (recipe-private); if null (defensive), emit nothing.
    if (!sub_) return {};
    const void* child = nav_.navigate(structPtr);
    return runRecipeB(*sub_, child, ctx);
}

std::string ArrayStructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += sep_;                         // join semantics (skip before first)
        const void* child = nav_.navigate(structPtr, i);
        out += runRecipeB(*sub_, child, ctx);       // run sub-recipe per element
    }
    return out;
}

std::string ArrayStructBlockProviderA::get(const void* structPtr, const DeviceCtx& ctx) const {
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += sep_;                         // join semantics (skip before first)
        const void* child = nav_.navigate(structPtr, i);
        out += runRecipeA(*sub_, child, ctx);       // Route A runner per element
    }
    return out;
}

} // namespace sv
