#include <struct_view/Engine.hpp>
#include <struct_view/ConfigLoader.hpp>
#include <struct_view/Validator.hpp>
#include <struct_view/Builder.hpp>
#include <struct_view/RecipeRunner.hpp>

namespace sv {

Engine::Engine(NameRegistry& names, ConnectorLib& connectors)
    : names_(names), connectors_(connectors) {}

LoadResult Engine::loadConfig(std::string_view jsonText) {
    LoadResult lr;
    auto parsed = ConfigLoader::parse(jsonText);
    if (!parsed.ok) {
        lr.ok = false;
        lr.errors.push_back({"", "parse error: " + parsed.parseError, 0});
        return lr;
    }
    auto verrors = Validator::validate(parsed.ast, names_, connectors_);
    if (!verrors.empty()) {
        lr.ok = false;
        lr.errors = std::move(verrors);
        return lr;
    }
    auto built = Builder::compile(parsed.ast, names_, connectors_);
    if (!built.ok) {
        lr.ok = false;
        lr.errors = std::move(built.errors);
        return lr;
    }
    // Success: swap in the new store. (Store owns recipes via shared_ptr.)
    store_ = std::move(built.store);
    lr.ok = true;
    return lr;
}

std::string Engine::render(const std::string& recipeName,
                           const void* structPtr,
                           const DeviceCtx& ctx) const {
    auto snap = store_.snapshot(recipeName);
    if (!snap) return "";
    return runRecipeB(*snap, structPtr, ctx);
}

} // namespace sv
