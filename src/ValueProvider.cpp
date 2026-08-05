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

} // namespace sv
