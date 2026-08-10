#include <struct_view/BuilderA.hpp>
#include <struct_view/ValueProvider.hpp>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <variant>

namespace sv {

// Same resolveFieldProvider as Builder.cpp — resolves scalar fields/devices
// via lookup, scalar arrays via ScalarArrayProvider creation.
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

BuilderA::Result BuilderA::compile(const ConfigAst& ast,
                                   const NameRegistry& names,
                                   const ConnectorLib& connectors) {
    Result r;

    // Pass 1: compile each recipe; defer struct-block/array/scalar-array + sub-recipe refs to Pass 2.
    struct PendingBind {
        std::shared_ptr<RecipeA> ra;
        std::size_t stepIdx;
        std::string name;
        std::string sepOverride;
        std::string isepOverride;
    };
    std::unordered_map<std::string, std::shared_ptr<RecipeA>> byName;
    std::vector<PendingBind> pending;        // single struct blocks (SubRecipeBinding)
    std::vector<PendingBind> pendingArray;   // struct arrays (FieldBinding)
    std::vector<PendingBind> pendingScalar;  // scalar arrays (FieldBinding + ScalarArrayProvider)
    std::vector<PendingBind> pendingSub;     // user sub-recipe refs

    for (const auto& raAst : ast.recipes) {
        auto ra = std::make_shared<RecipeA>();
        ra->name = raAst.name;
        for (const auto& seg : raAst.segments) {
            StepA step;
            if (seg.isRef) {
                if (names.isScalarArray(seg.text)) {
                    pendingScalar.push_back({ra, ra->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
                    step.kind = StepKind::Field;
                    step.binding = FieldBinding{nullptr};
                } else if (const ValueProvider* vp = names.lookup(seg.text)) {
                    step.kind = StepKind::Field;
                    step.binding = FieldBinding{vp};
                } else if (names.isStructBlock(seg.text)) {
                    pending.push_back({ra, ra->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
                    step.kind = StepKind::Field;
                    step.binding = FieldBinding{nullptr};
                } else if (names.isStructArray(seg.text)) {
                    pendingArray.push_back({ra, ra->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
                    step.kind = StepKind::Field;
                    step.binding = FieldBinding{nullptr};
                } else if (auto lit = connectors.get(seg.text)) {
                    step.kind = StepKind::Connector;
                    step.binding = ConnectorBinding{*lit};
                } else {
                    pendingSub.push_back({ra, ra->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
                    step.kind = StepKind::Field;
                    step.binding = FieldBinding{nullptr};
                }
            } else {
                step.kind = StepKind::Connector;
                step.binding = ConnectorBinding{seg.text};
            }
            ra->steps.push_back(step);
        }
        byName[raAst.name] = std::move(ra);
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 2: struct blocks — SubRecipeBinding with sep override.
    for (auto& p : pending) {
        const StructBlockDecl* decl = names.findStructBlockDecl(p.name);
        if (!decl) {
            r.errors.push_back({p.ra->name, "unknown struct block: " + p.name, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = resolveFieldProvider(names, fn, p.ra->ownedProviders);
            if (!vp) {
                r.errors.push_back({p.ra->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        std::string sep = p.sepOverride.empty() ? decl->sep : p.sepOverride;
        StepA step;
        step.kind = StepKind::SubRecipe;
        step.binding = SubRecipeBinding{decl->nav, std::move(fields), sep};
        p.ra->steps[p.stepIdx] = std::move(step);
    }

    // Pass 2b: struct arrays — ArrayStructBlockProviderA with isep/asep override.
    for (auto& pa : pendingArray) {
        const StructArrayDecl* decl = names.findStructArrayDecl(pa.name);
        if (!decl) {
            r.errors.push_back({pa.ra->name, "unknown struct array: " + pa.name, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = resolveFieldProvider(names, fn, pa.ra->ownedProviders);
            if (!vp) {
                r.errors.push_back({pa.ra->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        std::string isep = pa.isepOverride.empty() ? decl->sep : pa.isepOverride;
        std::string asep = pa.sepOverride.empty() ? decl->arraySep : pa.sepOverride;
        auto asbp = std::make_unique<ArrayStructBlockProviderA>(
            decl->nav, std::move(fields), decl->count, isep, asep);
        StepA step;
        step.kind = StepKind::Field;
        step.binding = FieldBinding{asbp.get()};
        pa.ra->steps[pa.stepIdx] = std::move(step);
        pa.ra->ownedProviders.push_back(std::move(asbp));
    }

    // Pass 2c: scalar arrays — ScalarArrayProvider with sep override.
    for (auto& ps : pendingScalar) {
        const ScalarArrayDecl* decl = names.findScalarArrayDecl(ps.name);
        if (!decl) {
            r.errors.push_back({ps.ra->name, "unknown scalar array: " + ps.name, 0});
            continue;
        }
        std::string sep = ps.sepOverride.empty() ? decl->sep : ps.sepOverride;
        auto sap = std::make_unique<ScalarArrayProvider>(decl->elemFn, decl->count, sep);
        StepA step;
        step.kind = StepKind::Field;
        step.binding = FieldBinding{sap.get()};
        ps.ra->steps[ps.stepIdx] = std::move(step);
        ps.ra->ownedProviders.push_back(std::move(sap));
    }

    // Pass 2d: user sub-recipe refs — SubRecipeProviderA wraps compiled recipe.
    for (auto& ps : pendingSub) {
        auto it = byName.find(ps.name);
        if (it == byName.end()) {
            r.errors.push_back({ps.ra->name, "unresolved name at bind: " + ps.name, 0});
            continue;
        }
        auto srp = std::make_unique<SubRecipeProviderA>(it->second);
        StepA step;
        step.kind = StepKind::Field;
        step.binding = FieldBinding{srp.get()};
        ps.ra->steps[ps.stepIdx] = std::move(step);
        ps.ra->ownedProviders.push_back(std::move(srp));
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 3: hand out immutable recipes.
    for (auto& [name, ra] : byName) {
        r.recipes[name] = std::move(ra);
    }
    r.ok = true;
    return r;
}

} // namespace sv
