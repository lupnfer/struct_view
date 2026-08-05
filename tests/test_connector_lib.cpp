#include <doctest/doctest.h>
#include <struct_view/ConnectorLib.hpp>

TEST_CASE("ConnectorLib: add and get") {
    sv::ConnectorLib lib;
    lib.add("dash", "-");
    lib.add("pipe", "|");
    CHECK(lib.get("dash") == "-");
    CHECK(lib.get("pipe") == "|");
}

TEST_CASE("ConnectorLib: unknown connector returns nullopt") {
    sv::ConnectorLib lib;
    CHECK_FALSE(lib.get("nope").has_value());
}
