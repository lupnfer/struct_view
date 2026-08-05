#pragma once
#include <memory>
#include <string>
#include <string_view>
#include "DeviceCtx.hpp"

namespace sv {

struct RecipeB;  // forward — defined in Recipe.hpp

// Type-erased value source. Hot path calls get() per step.
class ValueProvider {
public:
    virtual ~ValueProvider() = default;
    virtual std::string get(const void* structPtr, const DeviceCtx& ctx) const = 0;
};

// Wraps a lambda: (const void*, const DeviceCtx&) -> std::string.
// Used by codegen for fields and device getters.
template <typename F>
class LambdaProvider : public ValueProvider {
    F fn_;
public:
    explicit LambdaProvider(F fn) : fn_(std::move(fn)) {}
    std::string get(const void* p, const DeviceCtx& ctx) const override { return fn_(p, ctx); }
};

template <typename F>
std::unique_ptr<ValueProvider> makeProvider(F fn) {
    return std::make_unique<LambdaProvider<F>>(std::move(fn));
}

// From a parent struct pointer, yield a child struct pointer.
// Constructed from a stateless lambda converting to const void*(*)(const void*).
class Navigator {
    const void* (*fn_)(const void*);
public:
    explicit Navigator(const void* (*f)(const void*)) : fn_(f) {}
    const void* navigate(const void* parent) const { return fn_(parent); }
};

// Route B connector/literal holder.
class ConnectorProvider : public ValueProvider {
    std::string literal_;
public:
    explicit ConnectorProvider(std::string_view lit) : literal_(lit) {}
    std::string get(const void*, const DeviceCtx&) const override { return literal_; }
};

// A whole key struct block: navigate to child, run sub-recipe, return its string.
// sub_ is bound at construction with a shared_ptr to the OWNING recipe's sub-recipe
// (recipe-private, immutable after publish — spec §3.4a Rule 1/3). Each compiled
// recipe version owns its own StructBlockProvider instances; there is no shared,
// re-bound provider. Lifetime: owned by RecipeB::ownedProviders; the shared_ptr sub
// keeps the sub-recipe graph alive for any in-flight render snapshotting the parent.
class StructBlockProvider : public ValueProvider {
    Navigator nav_;
    std::shared_ptr<const RecipeB> sub_;
public:
    StructBlockProvider(Navigator nav, std::shared_ptr<const RecipeB> sub)
        : nav_(std::move(nav)), sub_(std::move(sub)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

} // namespace sv
