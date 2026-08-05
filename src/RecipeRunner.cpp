#include <struct_view/RecipeRunner.hpp>
#include <struct_view/Recipe.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <string>

namespace sv {

std::string runRecipeB(const RecipeB& r, const void* structPtr, const DeviceCtx& ctx) {
    std::string out;
    out.reserve(64);
    for (const StepB& s : r.steps) {
        if (s.provider) out += s.provider->get(structPtr, ctx);
    }
    return out;
}

} // namespace sv
