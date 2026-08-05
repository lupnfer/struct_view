#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <string>

TEST_CASE("scaffold: headers vendor and build") {
    nlohmann::json j = {{"name", "alarm_line"}};
    CHECK(j["name"].get<std::string>() == "alarm_line");
}
