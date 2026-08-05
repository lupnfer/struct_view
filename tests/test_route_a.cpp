#include <doctest/doctest.h>
#include <cstring>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/ValueProvider.hpp>
#include "event.h"
#include "device_ctx.h"
#include "registry_gen.cpp"

TEST_CASE("Route A: renderA matches Route B output") {
    sv::NameRegistry reg; registerStructViewNames(reg);
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    const char* cfg =
        R"({"recipes":[
          {"name":"alarm_line","template":"${camera}:${time}|${person}@${rect}"},
          {"name":"person","template":"${name}-${age}"},
          {"name":"rect","template":"${x},${y},${w},${h}"}]})";
    REQUIRE(engine.loadConfig(cfg).ok);     // Route B
    REQUIRE(engine.loadConfigA(cfg).ok);    // Route A

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{1717171717, &p, {100, 200, 300, 400}};
    MyDeviceCtx ctx;
    std::string b = engine.render("alarm_line", &ev, ctx);    // Route B
    std::string a = engine.renderA("alarm_line", &ev, ctx);   // Route A
    CHECK(a == b);
    CHECK(a == "CAM001:1717171717|Alice-30@100,200,300,400");
}
