#include <struct_view/ValueProvider.hpp>
#include <struct_view/Recipe.hpp>
#include <struct_view/RecipeRunner.hpp>

namespace sv {

std::string StructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    // sub_ must be bound by Builder; if not bound, emit nothing (defensive).
    if (!sub_) return {};
    const void* child = nav_.navigate(structPtr);
    return runRecipeB(*sub_, child, ctx);
}

} // namespace sv
