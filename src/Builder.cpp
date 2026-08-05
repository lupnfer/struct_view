#include <struct_view/Builder.hpp>
#include <struct_view/ValueProvider.hpp>
#include <unordered_map>

namespace sv {

Builder::Result Builder::compile(const ConfigAst& ast,
                                 const NameRegistry& names,
                                 const ConnectorLib& connectors) {
    Result r;

    // Pass 1: compile each recipe into a RecipeB (structblock sub_ left null).
    std::unordered_map<std::string, std::shared_ptr<const RecipeB>> byName;
    for (const auto& ra : ast.recipes) {
        auto rb = std::make_shared<RecipeB>();
        rb->name = ra.name;
        for (const auto& seg : ra.segments) {
            StepB step;
            if (seg.isRef) {
                if (const ValueProvider* vp = names.lookup(seg.text)) {
                    step.provider = vp;                      // field/device/structblock (NameRegistry-owned)
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
        byName[ra.name] = std::move(rb);  // shared_ptr<RecipeB> -> shared_ptr<const RecipeB>
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 2: bind struct-block sub-recipe pointers.
    for (auto& b : names.structBlocks()) {
        auto it = byName.find(b.second->subRecipeName());
        if (it == byName.end()) {
            r.errors.push_back({b.first, "subRecipe missing at bind: " + b.second->subRecipeName(), 0});
            continue;
        }
        b.second->bind(it->second.get());
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 3: publish.
    for (auto& [name, rb] : byName) {
        r.store.publish(name, rb);
    }
    r.ok = true;
    return r;
}

} // namespace sv
