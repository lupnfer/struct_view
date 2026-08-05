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
    // R"js(...)js" delimiter: the box template contains "(${tags})" whose `)`
    // sequence would terminate a plain R"(...)" raw string.
    REQUIRE(engine.loadConfig(R"js({"recipes":[
        {"name":"alarm_line","template":"${camera}:${time}|${person}@${rect}|${feat_ids}|${boxes}"},
        {"name":"person","template":"${name}-${age}"},
        {"name":"box","template":"${x},${y},${w},${h}(${tags})"}]})js").ok);

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{
        1717171717, &p,
        {100, 200, 300, 400, {7, 8}},
        {11, 22, 33, 0, 0, 0, 0, 0},
        {{100,200,300,400,{1,2}}, {110,210,310,410,{3,4}},
         {0,0,0,0,{0,0}}, {0,0,0,0,{0,0}}}
    };
    MyDeviceCtx ctx;
    CHECK(engine.render("alarm_line", &ev, ctx) ==
        "CAM001:1717171717|Alice-30@100,200,300,400(7-8)|11-22-33-0-0-0-0-0|100,200,300,400(1-2)|110,210,310,410(3-4)|0,0,0,0(0-0)|0,0,0,0(0-0)");
}
