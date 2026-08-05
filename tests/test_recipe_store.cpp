#include <doctest/doctest.h>
#include <struct_view/RecipeStore.hpp>
#include <struct_view/Recipe.hpp>
#include <struct_view/ValueProvider.hpp>  // RecipeB owns unique_ptr<ValueProvider>
#include <unordered_map>
#include <utility>

TEST_CASE("RecipeStore: publish then snapshot") {
    sv::RecipeStore<sv::RecipeB> store;
    sv::RecipeB rb; rb.name = "alarm";
    auto r = std::make_shared<const sv::RecipeB>(std::move(rb));
    store.publish("alarm", r);
    auto snap = store.snapshot("alarm");
    REQUIRE(snap);
    CHECK(snap->name == "alarm");
}

TEST_CASE("RecipeStore: republish swaps atomically") {
    sv::RecipeStore<sv::RecipeB> store;
    sv::RecipeB rb1; rb1.name = "v1";
    auto r1 = std::make_shared<const sv::RecipeB>(std::move(rb1));
    store.publish("r", r1);
    sv::RecipeB rb2; rb2.name = "v2";
    auto r2 = std::make_shared<const sv::RecipeB>(std::move(rb2));
    store.publish("r", r2);
    CHECK(store.snapshot("r")->name == "v2");
}

TEST_CASE("RecipeStore: missing recipe snapshot returns null") {
    sv::RecipeStore<sv::RecipeB> store;
    CHECK_FALSE(store.snapshot("nope"));
}

TEST_CASE("RecipeStore: publishAll atomically swaps the whole map (no half-publish)") {
    sv::RecipeStore<sv::RecipeB> store;
    sv::RecipeB a; a.name = "a";
    store.publish("a", std::make_shared<const sv::RecipeB>(std::move(a)));
    REQUIRE(store.snapshot("a"));

    // publishAll replaces the entire map under one write-lock; stale entries gone.
    std::unordered_map<std::string, std::shared_ptr<const sv::RecipeB>> newMap;
    sv::RecipeB b; b.name = "b";
    newMap["b"] = std::make_shared<const sv::RecipeB>(std::move(b));
    store.publishAll(std::move(newMap));

    CHECK_FALSE(store.snapshot("a"));          // stale recipe dropped wholesale
    REQUIRE(store.snapshot("b"));
    CHECK(store.snapshot("b")->name == "b");
}
