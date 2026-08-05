#pragma once
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace sv {

// Owns recipes by name. publish() takes a unique lock; snapshot() takes a
// shared lock (readers don't block each other). C++17: no atomic shared_ptr,
// so a shared_mutex is the pragmatic near-lock-free choice (spec section 7).
// Internals are pimpl'd so the store itself is movable (shared_mutex is not
// movable) — Engine move-assigns the store when swapping in a freshly built one.
template <typename RecipeT>
class RecipeStore {
    struct Impl {
        std::unordered_map<std::string, std::shared_ptr<const RecipeT>> map;
        mutable std::shared_mutex mtx;
    };
    std::unique_ptr<Impl> pimpl_;
public:
    RecipeStore() : pimpl_(std::make_unique<Impl>()) {}
    RecipeStore(const RecipeStore&) = delete;
    RecipeStore& operator=(const RecipeStore&) = delete;
    RecipeStore(RecipeStore&&) noexcept = default;
    RecipeStore& operator=(RecipeStore&&) noexcept = default;
    ~RecipeStore() = default;

    void publish(std::string name, std::shared_ptr<const RecipeT> recipe) {
        std::unique_lock<std::shared_mutex> lk(pimpl_->mtx);
        pimpl_->map[std::move(name)] = std::move(recipe);
    }
    std::shared_ptr<const RecipeT> snapshot(const std::string& name) const {
        std::shared_lock<std::shared_mutex> lk(pimpl_->mtx);
        auto it = pimpl_->map.find(name);
        if (it == pimpl_->map.end()) return nullptr;
        return it->second;  // copy bumps refcount; recipe stays alive after unlock
    }
};

} // namespace sv
