#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "DeviceCtx.hpp"

namespace sv {

struct RecipeB;  // forward — defined in Recipe.hpp
struct RecipeA;  // forward — defined in Recipe.hpp

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

// A whole key struct block: navigate to child, then join its fields with sep.
// No sub-recipe — the field order + sep live in the schema (spec §4 of
// name-convention spec). fieldProviders_ are raw ptrs into NameRegistry-owned
// field providers (longer-lived, read-only). Lifetime: owned by RecipeB::ownedProviders.
class StructBlockProvider : public ValueProvider {
    Navigator nav_;
    std::vector<const ValueProvider*> fieldProviders_;   // child struct fields (in order)
    std::string sep_;
public:
    StructBlockProvider(Navigator nav, std::vector<const ValueProvider*> fields, std::string sep)
        : nav_(std::move(nav)), fieldProviders_(std::move(fields)), sep_(std::move(sep)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

// A user-composed sub-recipe referenced by ${r_name}. Wraps the compiled sub-recipe
// (shared_ptr<const RecipeB>, RCU co-ownership) as a ValueProvider so it can be
// slotted into a StepB. get() runs the sub-recipe on the same structPtr (spec §5).
class SubRecipeProvider : public ValueProvider {
    std::shared_ptr<const RecipeB> sub_;
public:
    explicit SubRecipeProvider(std::shared_ptr<const RecipeB> sub) : sub_(std::move(sub)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

// Route A variant: wraps shared_ptr<const RecipeA> and runs runRecipeA (spec §5).
class SubRecipeProviderA : public ValueProvider {
    std::shared_ptr<const RecipeA> sub_;
public:
    explicit SubRecipeProviderA(std::shared_ptr<const RecipeA> sub) : sub_(std::move(sub)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

// Indexed navigator: parent + index i -> child element pointer.
// Used by ArrayStructBlockProvider to iterate struct arrays. Mirrors Navigator
// but takes a per-element index (spec section 4.1 of array-support spec).
class IndexedNavigator {
    const void* (*fn_)(const void*, std::size_t);
public:
    explicit IndexedNavigator(const void* (*f)(const void*, std::size_t)) : fn_(f) {}
    const void* navigate(const void* parent, std::size_t i) const { return fn_(parent, i); }
};

// A struct array: for each of count_ elements, navigate to &s->arr[i], then
// join the element's fields with sep_ (element-internal) and join elements with
// arraySep_. No sub-recipe — field order + seps live in the schema (spec §4).
// Recipe-private: owned by RecipeB::ownedProviders.
class ArrayStructBlockProvider : public ValueProvider {
    IndexedNavigator nav_;
    std::vector<const ValueProvider*> fieldProviders_;   // per-element fields (in order)
    std::size_t count_;
    std::string sep_;        // element-internal field separator
    std::string arraySep_;   // element-between separator
public:
    ArrayStructBlockProvider(IndexedNavigator nav, std::vector<const ValueProvider*> fields,
                             std::size_t count, std::string sep, std::string arraySep)
        : nav_(std::move(nav)), fieldProviders_(std::move(fields)),
          count_(count), sep_(std::move(sep)), arraySep_(std::move(arraySep)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

// Route A variant: identical logic to ArrayStructBlockProvider (field-list
// traversal, no runRecipeA). Route A reuses it via FieldBinding (spec §5).
// Kept as a separate class for Route A ownership (RecipeA::ownedProviders).
class ArrayStructBlockProviderA : public ValueProvider {
    IndexedNavigator nav_;
    std::vector<const ValueProvider*> fieldProviders_;
    std::size_t count_;
    std::string sep_;
    std::string arraySep_;
public:
    ArrayStructBlockProviderA(IndexedNavigator nav, std::vector<const ValueProvider*> fields,
                              std::size_t count, std::string sep, std::string arraySep)
        : nav_(std::move(nav)), fieldProviders_(std::move(fields)),
          count_(count), sep_(std::move(sep)), arraySep_(std::move(arraySep)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

} // namespace sv
