#include <doctest/doctest.h>
#include <struct_view/RecipeStore.hpp>
#include <struct_view/Recipe.hpp>
#include <struct_view/ValueProvider.hpp>  // RecipeB owns unique_ptr<ValueProvider>

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
