#include <struct_view/Builder.hpp>
#include <struct_view/ValueProvider.hpp>
#include <cstddef>
#include <unordered_map>

namespace sv {

Builder::Result Builder::compile(const ConfigAst& ast,
                                 const NameRegistry& names,
                                 const ConnectorLib& connectors) {
    Result r;

    // Pass 1: compile each recipe. Struct-block refs + user-sub-recipe refs are
    // deferred to Pass 2 (they reference recipes compiled later — forward refs ok).
    struct PendingBind {
        std::shared_ptr<RecipeB> rb;
        std::size_t stepIdx;
        std::string name;       // block name OR sub-recipe name
    };
    std::unordered_map<std::string, std::shared_ptr<RecipeB>> byName;
    std::vector<PendingBind> pending;        // single struct blocks
    std::vector<PendingBind> pendingArray;   // struct arrays
    std::vector<PendingBind> pendingSub;     // user sub-recipe refs (r_ names in byName)

    for (const auto& ra : ast.recipes) {
        auto rb = std::make_shared<RecipeB>();
        rb->name = ra.name;
        for (const auto& seg : ra.segments) {
            StepB step;
            if (seg.isRef) {
                if (const ValueProvider* vp = names.lookup(seg.text)) {
                    step.provider = vp;                      // field/device (NameRegistry-owned)
                } else if (names.isStructBlock(seg.text)) {
                    pending.push_back({rb, rb->steps.size(), seg.text});
                } else if (names.isStructArray(seg.text)) {
                    pendingArray.push_back({rb, rb->steps.size(), seg.text});
                } else if (auto lit = connectors.get(seg.text)) {
                    auto cp = std::make_unique<ConnectorProvider>(*lit);
                    step.provider = cp.get();
                    rb->ownedProviders.push_back(std::move(cp));
                } else {
                    // Not a field/block/array/connector — could be a user sub-recipe
                    // (resolved in Pass 2 after all recipes are compiled). Defer.
                    pendingSub.push_back({rb, rb->steps.size(), seg.text});
                }
            } else {
                auto cp = std::make_unique<ConnectorProvider>(seg.text);
                step.provider = cp.get();
                rb->ownedProviders.push_back(std::move(cp));
            }
            rb->steps.push_back(step);
        }
        byName[ra.name] = std::move(rb);
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 2: struct blocks — field-list provider (no sub-recipe, spec §4).
    for (auto& p : pending) {
        const StructBlockDecl* decl = names.findStructBlockDecl(p.name);
        if (!decl) {
            r.errors.push_back({p.rb->name, "unknown struct block: " + p.name, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = names.lookup(fn);
            if (!vp) {
                r.errors.push_back({p.rb->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        auto sbp = std::make_unique<StructBlockProvider>(decl->nav, std::move(fields), decl->sep);
        p.rb->steps[p.stepIdx].provider = sbp.get();
        p.rb->ownedProviders.push_back(std::move(sbp));
    }

    // Pass 2b: struct arrays — field-list provider (no sub-recipe, spec §4).
    for (auto& pa : pendingArray) {
        const StructArrayDecl* decl = names.findStructArrayDecl(pa.name);
        if (!decl) {
            r.errors.push_back({pa.rb->name, "unknown struct array: " + pa.name, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = names.lookup(fn);
            if (!vp) {
                r.errors.push_back({pa.rb->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        auto asbp = std::make_unique<ArrayStructBlockProvider>(
            decl->nav, std::move(fields), decl->count, decl->sep, decl->arraySep);
        pa.rb->steps[pa.stepIdx].provider = asbp.get();
        pa.rb->ownedProviders.push_back(std::move(asbp));
    }

    // Pass 2c: user sub-recipe refs — bind to the compiled recipe (RCU co-ownership).
    // A SubRecipeProvider wraps the referenced recipe as a ValueProvider.
    for (auto& ps : pendingSub) {
        auto it = byName.find(ps.name);
        if (it == byName.end()) {
            r.errors.push_back({ps.rb->name, "unresolved name at bind: " + ps.name, 0});
            continue;
        }
        // shared_ptr<RecipeB> -> shared_ptr<const RecipeB> copy bumps refcount.
        auto srp = std::make_unique<SubRecipeProvider>(it->second);
        ps.rb->steps[ps.stepIdx].provider = srp.get();
        ps.rb->ownedProviders.push_back(std::move(srp));
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 3: hand out immutable recipes. shared_ptr<RecipeB> -> shared_ptr<const
    // RecipeB> via converting move (refcount transferred). Engine does the
    // actual store publish (publishAll) — Builder no longer owns a store.
    for (auto& [name, rb] : byName) {
        r.recipes[name] = std::move(rb);
    }
    r.ok = true;
    return r;
}

} // namespace sv
