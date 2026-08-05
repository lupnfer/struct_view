#include <struct_view/BuilderA.hpp>
#include <struct_view/ValueProvider.hpp>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <variant>

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
bool isStructArray(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structArrayDecls()) if (d.first == s) return true;
    return false;
}
const StructArrayDecl* findArrayDecl(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structArrayDecls()) if (d.first == s) return &d.second;
    return nullptr;
}
} // namespace

BuilderA::Result BuilderA::compile(const ConfigAst& ast,
                                   const NameRegistry& names,
                                   const ConnectorLib& connectors) {
    Result r;

    // Pass 1: compile each recipe; defer struct-block refs to pending (binding
    // left as a FieldBinding{nullptr} placeholder, fixed in Pass 2).
    struct PendingBind {
        std::shared_ptr<RecipeA> ra;
        std::size_t stepIdx;
        std::string blockName;
    };
    std::unordered_map<std::string, std::shared_ptr<RecipeA>> byName;
    std::vector<PendingBind> pending;       // single struct blocks (SubRecipeBinding)
    std::vector<PendingBind> pendingArray;  // struct arrays (FieldBinding, spec §5)

    for (const auto& raAst : ast.recipes) {
        auto ra = std::make_shared<RecipeA>();
        ra->name = raAst.name;
        for (const auto& seg : raAst.segments) {
            StepA step;
            if (seg.isRef) {
                if (const ValueProvider* vp = names.lookup(seg.text)) {
                    // Field or device? At compile time both are just ValueProvider*
                    // from names.lookup and are indistinguishable without extra
                    // metadata. Treat all lookup hits as Field (pass structPtr).
                    // Device getters' lambdas ignore structPtr (they use ctx), so
                    // output is identical to Route B. This preserves parity; true
                    // Device separation would require codegen metadata (out of
                    // scope for Task 15; the connector/sub-recipe inlining is the
                    // main Route A win anyway).
                    step.kind = StepKind::Field;
                    step.binding = FieldBinding{vp};
                } else if (isStructBlock(names, seg.text)) {
                    pending.push_back({ra, ra->steps.size(), seg.text});
                    // placeholder; replaced in Pass 2. Use FieldBinding{nullptr}.
                    step.kind = StepKind::Field;
                    step.binding = FieldBinding{nullptr};
                } else if (isStructArray(names, seg.text)) {
                    pendingArray.push_back({ra, ra->steps.size(), seg.text});
                    // placeholder; replaced in Pass 2b. Use FieldBinding{nullptr}.
                    step.kind = StepKind::Field;
                    step.binding = FieldBinding{nullptr};
                } else if (auto lit = connectors.get(seg.text)) {
                    step.kind = StepKind::Connector;
                    step.binding = ConnectorBinding{*lit};
                } else {
                    // Validator should have caught this; defensive.
                    r.errors.push_back({raAst.name, "unresolved name at compile: " + seg.text, 0});
                    continue;
                }
            } else {
                // Literal segment: pre-extract into ConnectorBinding (no virtual,
                // no heap) — the core Route A optimization vs Route B's
                // ConnectorProvider.
                step.kind = StepKind::Connector;
                step.binding = ConnectorBinding{seg.text};
            }
            ra->steps.push_back(step);
        }
        byName[raAst.name] = std::move(ra);
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 2: bind recipe-private SubRecipeBindings (Navigator + shared_ptr to
    // sub-recipe). Each recipe owns its SubRecipeBinding's shared_ptr to its
    // sub-recipe (co-ownership, immutable after publish — spec §3.4a Rule 1/3).
    // No separate provider object: the binding is held by value in the StepA
    // variant, inlining nav + sub-recipe ptr (no virtual StructBlockProvider).
    for (auto& p : pending) {
        const StructBlockDecl* decl = findDecl(names, p.blockName);
        if (!decl) {  // Validator should have caught this; defensive.
            r.errors.push_back({p.ra->name, "unknown struct block: " + p.blockName, 0});
            continue;
        }
        auto it = byName.find(decl->subRecipeName);
        if (it == byName.end()) {
            r.errors.push_back({p.ra->name, "subRecipe missing at bind: " + decl->subRecipeName, 0});
            continue;
        }
        // Replace the placeholder step with a SubRecipe step. shared_ptr<RecipeA>
        // -> shared_ptr<const RecipeA> copy bumps refcount (recipe co-owns sub).
        StepA step;
        step.kind = StepKind::SubRecipe;
        step.binding = SubRecipeBinding{decl->nav, it->second};
        p.ra->steps[p.stepIdx] = std::move(step);
    }

    // Pass 2b: struct arrays — create ArrayStructBlockProviderA (Route A variant),
    // put it in FieldBinding. Route A does NOT inline the array loop; it reuses the
    // provider via a single virtual call (spec §5). The provider is recipe-private
    // (ownedProviders) and holds shared_ptr<const RecipeA> to its sub-recipe (RCU
    // §3.4a Rule 1/3). Note: ArrayStructBlockProvider (Task 1) is hardcoded to
    // RecipeB/runRecipeB and cannot host RecipeA sub-recipes — hence the Route A
    // variant class (option A fix for the spec §5 RecipeB/RecipeA mismatch).
    for (auto& pa : pendingArray) {
        const StructArrayDecl* decl = findArrayDecl(names, pa.blockName);
        if (!decl) {  // Validator should have caught this; defensive.
            r.errors.push_back({pa.ra->name, "unknown struct array: " + pa.blockName, 0});
            continue;
        }
        auto it = byName.find(decl->subRecipeName);
        if (it == byName.end()) {
            r.errors.push_back({pa.ra->name, "subRecipe missing at bind: " + decl->subRecipeName, 0});
            continue;
        }
        // shared_ptr<RecipeA> -> shared_ptr<const RecipeA> via ctor (refcount bump,
        // recipe co-owns sub). ArrayStructBlockProviderA is-a ValueProvider, so it
        // slots into FieldBinding; runRecipeA's Field case calls provider->get().
        auto asbp = std::make_unique<ArrayStructBlockProviderA>(
            decl->nav, it->second, decl->count, decl->sep);
        StepA step;
        step.kind = StepKind::Field;
        step.binding = FieldBinding{asbp.get()};
        pa.ra->steps[pa.stepIdx] = std::move(step);
        pa.ra->ownedProviders.push_back(std::move(asbp));
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
