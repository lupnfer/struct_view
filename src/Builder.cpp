#include <struct_view/Builder.hpp>
#include <struct_view/ValueProvider.hpp>
#include <cstddef>
#include <unordered_map>

namespace sv {

namespace {
bool isStructBlock(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structDecls()) if (d.first == s) return true;
    return false;
}
const StructBlockDecl* findDecl(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structDecls()) if (d.first == s) return &d.second;
    return nullptr;
}
} // namespace

Builder::Result Builder::compile(const ConfigAst& ast,
                                 const NameRegistry& names,
                                 const ConnectorLib& connectors) {
    Result r;

    // Pass 1: compile each recipe. Struct-block refs are deferred to Pass 2 —
    // their recipe-private StructBlockProvider needs a shared_ptr to the
    // sub-recipe, which may be compiled later in this loop (forward refs ok).
    struct PendingBind {
        std::shared_ptr<RecipeB> rb;
        std::size_t stepIdx;
        std::string blockName;
    };
    std::unordered_map<std::string, std::shared_ptr<RecipeB>> byName;
    std::vector<PendingBind> pending;

    for (const auto& ra : ast.recipes) {
        auto rb = std::make_shared<RecipeB>();
        rb->name = ra.name;
        for (const auto& seg : ra.segments) {
            StepB step;
            if (seg.isRef) {
                if (const ValueProvider* vp = names.lookup(seg.text)) {
                    step.provider = vp;                      // field/device (NameRegistry-owned)
                } else if (isStructBlock(names, seg.text)) {
                    // Recipe-private StructBlockProvider created in Pass 2.
                    pending.push_back({rb, rb->steps.size(), seg.text});
                    // step.provider left nullptr; fixed in Pass 2.
                } else if (auto lit = connectors.get(seg.text)) {
                    auto cp = std::make_unique<ConnectorProvider>(*lit);
                    step.provider = cp.get();
                    rb->ownedProviders.push_back(std::move(cp));
                } else {
                    // Validator should have caught this; defensive.
                    r.errors.push_back({ra.name, "unresolved name at compile: " + seg.text, 0});
                    continue;
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

    // Pass 2: instantiate recipe-private StructBlockProviders, each holding a
    // shared_ptr to its own sub-recipe (bound once, immutable after publish).
    // Different recipe versions own different provider instances — no shared
    // mutable sub_ re-binding (spec §3.4a Rule 1). The shared_ptr keeps the
    // sub-recipe graph alive for any reader snapshotting the parent (Rule 3).
    for (auto& p : pending) {
        const StructBlockDecl* decl = findDecl(names, p.blockName);
        if (!decl) {  // Validator should have caught this; defensive.
            r.errors.push_back({p.rb->name, "unknown struct block: " + p.blockName, 0});
            continue;
        }
        auto it = byName.find(decl->subRecipeName);
        if (it == byName.end()) {
            r.errors.push_back({p.rb->name, "subRecipe missing at bind: " + decl->subRecipeName, 0});
            continue;
        }
        // Copy of it->second (shared_ptr<RecipeB> -> shared_ptr<const RecipeB>)
        // bumps refcount: parent recipe now co-owns its sub-recipe.
        auto sbp = std::make_unique<StructBlockProvider>(decl->nav, it->second);
        p.rb->steps[p.stepIdx].provider = sbp.get();
        p.rb->ownedProviders.push_back(std::move(sbp));
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
