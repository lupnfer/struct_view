#include <struct_view/Engine.hpp>
#include <struct_view/ConfigLoader.hpp>
#include <struct_view/Validator.hpp>
#include <struct_view/Builder.hpp>
#include <struct_view/BuilderA.hpp>
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
    // Success: atomically swap the store's entire recipe map under ONE write-lock.
    // store_ itself is never moved/replaced — its shared_mutex + internal map are
    // stable for the Engine's lifetime. Readers holding a shared_lock on the old
    // map keep their snapshot alive via shared_ptr refcount (RCU); they see either
    // the old map or the new map, never a mix. spec §3.4a Rule 2 / §5 (no half-publish).
    store_.publishAll(std::move(built.recipes));
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

LoadResult Engine::loadConfigA(std::string_view jsonText) {
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
    auto built = BuilderA::compile(parsed.ast, names_, connectors_);
    if (!built.ok) {
        lr.ok = false;
        lr.errors = std::move(built.errors);
        return lr;
    }
    // RCU publish: storeA_ is stable for the Engine's lifetime (never moved);
    // publishAll swaps the whole map under one write-lock. Readers snapshotting
    // the old map keep their recipes alive via shared_ptr refcount (spec §3.4a
    // Rule 2 / §5 — no half-publish). Same RCU safety as Route B.
    storeA_.publishAll(std::move(built.recipes));
    lr.ok = true;
    return lr;
}

std::string Engine::renderA(const std::string& recipeName,
                            const void* structPtr,
                            const DeviceCtx& ctx) const {
    auto snap = storeA_.snapshot(recipeName);
    if (!snap) return "";
    return runRecipeA(*snap, structPtr, ctx);
}

} // namespace sv
