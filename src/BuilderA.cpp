#include <struct_view/BuilderA.hpp>
#include <struct_view/ValueProvider.hpp>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <variant>

namespace sv {

BuilderA::Result BuilderA::compile(const ConfigAst& ast,
                                   const NameRegistry& names,
                                   const ConnectorLib& connectors) {
    Result r;

    // Pass 1: compile each recipe; defer struct-block + user-sub-recipe refs to Pass 2.
    struct PendingBind {
        std::shared_ptr<RecipeA> ra;
        std::size_t stepIdx;
        std::string name;
    };
    std::unordered_map<std::string, std::shared_ptr<RecipeA>> byName;
    std::vector<PendingBind> pending;        // single struct blocks (SubRecipeBinding)
    std::vector<PendingBind> pendingArray;   // struct arrays (FieldBinding, spec §5)
    std::vector<PendingBind> pendingSub;     // user sub-recipe refs (FieldBinding + SubRecipeProvider)

    for (const auto& raAst : ast.recipes) {
        auto ra = std::make_shared<RecipeA>();
        ra->name = raAst.name;
        for (const auto& seg : raAst.segments) {
            StepA step;
            if (seg.isRef) {
                if (const ValueProvider* vp = names.lookup(seg.text)) {
                    step.kind = StepKind::Field;
                    step.binding = FieldBinding{vp};
                } else if (names.isStructBlock(seg.text)) {
                    pending.push_back({ra, ra->steps.size(), seg.text});
                    step.kind = StepKind::Field;          // placeholder
                    step.binding = FieldBinding{nullptr};
                } else if (names.isStructArray(seg.text)) {
                    pendingArray.push_back({ra, ra->steps.size(), seg.text});
                    step.kind = StepKind::Field;          // placeholder
                    step.binding = FieldBinding{nullptr};
                } else if (auto lit = connectors.get(seg.text)) {
                    step.kind = StepKind::Connector;
                    step.binding = ConnectorBinding{*lit};
                } else {
                    // Could be a user sub-recipe (resolved in Pass 2c). Defer.
                    pendingSub.push_back({ra, ra->steps.size(), seg.text});
                    step.kind = StepKind::Field;          // placeholder
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

    // Pass 2: struct blocks — SubRecipeBinding holds field list + sep (no sub-recipe).
    for (auto& p : pending) {
        const StructBlockDecl* decl = names.findStructBlockDecl(p.name);
        if (!decl) {
            r.errors.push_back({p.ra->name, "unknown struct block: " + p.name, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = names.lookup(fn);
            if (!vp) {
                r.errors.push_back({p.ra->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        StepA step;
        step.kind = StepKind::SubRecipe;
        step.binding = SubRecipeBinding{decl->nav, std::move(fields), decl->sep};
        p.ra->steps[p.stepIdx] = std::move(step);
    }

    // Pass 2b: struct arrays — ArrayStructBlockProviderA in FieldBinding (spec §5).
    for (auto& pa : pendingArray) {
        const StructArrayDecl* decl = names.findStructArrayDecl(pa.name);
        if (!decl) {
            r.errors.push_back({pa.ra->name, "unknown struct array: " + pa.name, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = names.lookup(fn);
            if (!vp) {
                r.errors.push_back({pa.ra->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        auto asbp = std::make_unique<ArrayStructBlockProviderA>(
            decl->nav, std::move(fields), decl->count, decl->sep, decl->arraySep);
        StepA step;
        step.kind = StepKind::Field;
        step.binding = FieldBinding{asbp.get()};
        pa.ra->steps[pa.stepIdx] = std::move(step);
        pa.ra->ownedProviders.push_back(std::move(asbp));
    }

    // Pass 2c: user sub-recipe refs — SubRecipeProvider wraps the compiled recipe.
    // Route A has no SubRecipeBinding for user sub-recipes (only for struct blocks);
    // user sub-recipes go through a SubRecipeProvider in FieldBinding (one virtual call,
    // same as Route B). This preserves parity.
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

    // Pass 3: hand out immutable recipes. shared_ptr<RecipeA> -> shared_ptr<const
    // RecipeA> via converting move (refcount transferred). Engine does the
    // actual store publish (publishAll) — Builder no longer owns a store.
    for (auto& [name, ra] : byName) {
        r.recipes[name] = std::move(ra);
    }
    r.ok = true;
    return r;
}

} // namespace sv
