#include <struct_view/Builder.hpp>
#include <struct_view/ValueProvider.hpp>
#include <cstddef>
#include <unordered_map>

namespace sv {

// Resolve a block field name to a ValueProvider. Scalar fields/devices are in
// entries_ (lookup). Scalar arrays are in scalarArrayDecls_ — create a
// recipe-private ScalarArrayProvider with the default sep (block fields don't
// support :sep= override; only the block reference itself does).
// Returns nullptr if not found. Created providers are pushed into `owned`.
static const ValueProvider* resolveFieldProvider(
        const NameRegistry& names, const std::string& fn,
        std::vector<std::unique_ptr<ValueProvider>>& owned) {
    if (const ValueProvider* vp = names.lookup(fn)) return vp;
    if (const ScalarArrayDecl* d = names.findScalarArrayDecl(fn)) {
        auto sap = std::make_unique<ScalarArrayProvider>(d->elemFn, d->count, d->sep);
        const ValueProvider* raw = sap.get();
        owned.push_back(std::move(sap));
        return raw;
    }
    return nullptr;
}

Builder::Result Builder::compile(const ConfigAst& ast,
                                 const NameRegistry& names,
                                 const ConnectorLib& connectors) {
    Result r;

    // Pass 1: compile each recipe. Struct-block/struct-array/scalar-array refs
    // + user-sub-recipe refs are deferred to Pass 2 (forward refs ok).
    struct PendingBind {
        std::shared_ptr<RecipeB> rb;
        std::size_t stepIdx;
        std::string name;
        std::string sepOverride;   // from :sep=, empty = use default
        std::string isepOverride;  // from :isep=, empty = use default (struct arrays)
    };
    std::unordered_map<std::string, std::shared_ptr<RecipeB>> byName;
    std::vector<PendingBind> pending;          // single struct blocks
    std::vector<PendingBind> pendingArray;     // struct arrays
    std::vector<PendingBind> pendingScalar;    // scalar arrays
    std::vector<PendingBind> pendingSub;       // user sub-recipe refs

    for (const auto& ra : ast.recipes) {
        auto rb = std::make_shared<RecipeB>();
        rb->name = ra.name;
        for (const auto& seg : ra.segments) {
            StepB step;
            if (seg.isRef) {
                if (names.isScalarArray(seg.text)) {
                    pendingScalar.push_back({rb, rb->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
                } else if (const ValueProvider* vp = names.lookup(seg.text)) {
                    step.provider = vp;                      // field/device (NameRegistry-owned)
                } else if (names.isStructBlock(seg.text)) {
                    pending.push_back({rb, rb->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
                } else if (names.isStructArray(seg.text)) {
                    pendingArray.push_back({rb, rb->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
                } else if (auto lit = connectors.get(seg.text)) {
                    auto cp = std::make_unique<ConnectorProvider>(*lit);
                    step.provider = cp.get();
                    rb->ownedProviders.push_back(std::move(cp));
                } else {
                    pendingSub.push_back({rb, rb->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
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

    // Pass 2: struct blocks — field-list provider with sep override.
    for (auto& p : pending) {
        const StructBlockDecl* decl = names.findStructBlockDecl(p.name);
        if (!decl) {
            r.errors.push_back({p.rb->name, "unknown struct block: " + p.name, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = resolveFieldProvider(names, fn, p.rb->ownedProviders);
            if (!vp) {
                r.errors.push_back({p.rb->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        std::string sep = p.sepOverride.empty() ? decl->sep : p.sepOverride;
        auto sbp = std::make_unique<StructBlockProvider>(decl->nav, std::move(fields), sep);
        p.rb->steps[p.stepIdx].provider = sbp.get();
        p.rb->ownedProviders.push_back(std::move(sbp));
    }

    // Pass 2b: struct arrays — field-list provider with isep/asep override.
    for (auto& pa : pendingArray) {
        const StructArrayDecl* decl = names.findStructArrayDecl(pa.name);
        if (!decl) {
            r.errors.push_back({pa.rb->name, "unknown struct array: " + pa.name, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = resolveFieldProvider(names, fn, pa.rb->ownedProviders);
            if (!vp) {
                r.errors.push_back({pa.rb->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        std::string isep = pa.isepOverride.empty() ? decl->sep : pa.isepOverride;      // element-internal
        std::string asep = pa.sepOverride.empty() ? decl->arraySep : pa.sepOverride;   // element-between
        auto asbp = std::make_unique<ArrayStructBlockProvider>(
            decl->nav, std::move(fields), decl->count, isep, asep);
        pa.rb->steps[pa.stepIdx].provider = asbp.get();
        pa.rb->ownedProviders.push_back(std::move(asbp));
    }

    // Pass 2c: scalar arrays — ScalarArrayProvider with sep override.
    for (auto& ps : pendingScalar) {
        const ScalarArrayDecl* decl = names.findScalarArrayDecl(ps.name);
        if (!decl) {
            r.errors.push_back({ps.rb->name, "unknown scalar array: " + ps.name, 0});
            continue;
        }
        std::string sep = ps.sepOverride.empty() ? decl->sep : ps.sepOverride;
        auto sap = std::make_unique<ScalarArrayProvider>(decl->elemFn, decl->count, sep);
        ps.rb->steps[ps.stepIdx].provider = sap.get();
        ps.rb->ownedProviders.push_back(std::move(sap));
    }

    // Pass 2d: user sub-recipe refs — bind to compiled recipe (RCU co-ownership).
    for (auto& ps : pendingSub) {
        auto it = byName.find(ps.name);
        if (it == byName.end()) {
            r.errors.push_back({ps.rb->name, "unresolved name at bind: " + ps.name, 0});
            continue;
        }
        auto srp = std::make_unique<SubRecipeProvider>(it->second);
        ps.rb->steps[ps.stepIdx].provider = srp.get();
        ps.rb->ownedProviders.push_back(std::move(srp));
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 3: hand out immutable recipes.
    for (auto& [name, rb] : byName) {
        r.recipes[name] = std::move(rb);
    }
    r.ok = true;
    return r;
}

} // namespace sv
