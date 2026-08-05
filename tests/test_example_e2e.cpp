#include <doctest/doctest.h>
#include <cstring>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include "event.h"
#include "device_ctx.h"
#include "registry_gen.cpp"

TEST_CASE("E2E: security alarm_line matches expected string") {
    sv::NameRegistry reg; registerStructViewNames(reg);
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(R"({"recipes":[
        {"name":"alarm_line","template":"${camera}:${time}|${person}@${rect}"},
        {"name":"person","template":"${name}-${age}"},
        {"name":"rect","template":"${x},${y},${w},${h}"}]})").ok);

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{1717171717, &p, {100, 200, 300, 400}};
    MyDeviceCtx ctx;
    CHECK(engine.render("alarm_line", &ev, ctx) == "CAM001:1717171717|Alice-30@100,200,300,400");
}
