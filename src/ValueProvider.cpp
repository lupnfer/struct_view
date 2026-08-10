#include <struct_view/ValueProvider.hpp>
#include <struct_view/Recipe.hpp>
#include <struct_view/RecipeRunner.hpp>

namespace sv {

std::string ScalarArrayProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += sep_;
        out += elemFn_(structPtr, i);
    }
    return out;
}

std::string SubRecipeProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    return runRecipeB(*sub_, structPtr, ctx);
}

std::string SubRecipeProviderA::get(const void* structPtr, const DeviceCtx& ctx) const {
    return runRecipeA(*sub_, structPtr, ctx);
}

std::string StructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    if (fieldProviders_.empty()) return {};            // defensive
    const void* child = nav_.navigate(structPtr);
    std::string out;
    for (std::size_t i = 0; i < fieldProviders_.size(); ++i) {
        if (i) out += sep_;                             // join semantics (skip before first)
        out += fieldProviders_[i]->get(child, ctx);
    }
    return out;
}

std::string ArrayStructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    if (fieldProviders_.empty()) return {};
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += arraySep_;                        // element-between
        const void* child = nav_.navigate(structPtr, i);
        for (std::size_t j = 0; j < fieldProviders_.size(); ++j) {
            if (j) out += sep_;                         // element-internal field-between
            out += fieldProviders_[j]->get(child, ctx);
        }
    }
    return out;
}

std::string ArrayStructBlockProviderA::get(const void* structPtr, const DeviceCtx& ctx) const {
    // Identical to ArrayStructBlockProvider::get (field-list traversal, no runRecipeA).
    if (fieldProviders_.empty()) return {};
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += arraySep_;
        const void* child = nav_.navigate(structPtr, i);
        for (std::size_t j = 0; j < fieldProviders_.size(); ++j) {
            if (j) out += sep_;
            out += fieldProviders_[j]->get(child, ctx);
        }
    }
    return out;
}

} // namespace sv
