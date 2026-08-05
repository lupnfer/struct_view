#pragma once
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace sv {

// Owns recipes by name. publish()/publishAll() take a unique lock; snapshot()
// takes a shared lock (readers don't block each other). C++17: no atomic
// shared_ptr, so a shared_mutex is the pragmatic near-lock-free choice (spec §7).
//
// The store is a STABLE, long-lived object owned by Engine: its shared_mutex and
// internal map are NEVER destroyed during a reload. Move-replacing the store
// would destroy the mutex readers hold shared_locks on — spec §3.4a Rule 2
// forbids this. The pimpl makes the store movable (shared_mutex is not movable)
// only for Engine construction convenience; Engine never reassigns store_ after
// init — it calls publishAll/publish to swap contents in place.
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

    // Publish one recipe (atomic swap of that name's shared_ptr under write-lock).
    void publish(std::string name, std::shared_ptr<const RecipeT> recipe) {
        std::unique_lock<std::shared_mutex> lk(pimpl_->mtx);
        pimpl_->map[std::move(name)] = std::move(recipe);
    }
    // Publish a full set atomically: ONE write-lock, swap the whole map. Readers
    // see either the old map or the new map — never a mix (no half-publish,
    // spec §5). Stale recipes are dropped wholesale (replaced, not left behind).
    // This is the RCU publish path used by Engine::loadConfig (spec §3.4a Rule 2).
    void publishAll(std::unordered_map<std::string, std::shared_ptr<const RecipeT>> newMap) {
        std::unique_lock<std::shared_mutex> lk(pimpl_->mtx);
        pimpl_->map = std::move(newMap);
    }
    std::shared_ptr<const RecipeT> snapshot(const std::string& name) const {
        std::shared_lock<std::shared_mutex> lk(pimpl_->mtx);
        auto it = pimpl_->map.find(name);
        if (it == pimpl_->map.end()) return nullptr;
        return it->second;  // copy bumps refcount; recipe stays alive after unlock
    }
};

} // namespace sv
