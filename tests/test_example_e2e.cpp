#include <doctest/doctest.h>
#include <cstring>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include "event.h"
#include "device_ctx.h"
#include "registry_gen.cpp"

TEST_CASE("E2E: security r_alarm_line matches expected string") {
    sv::NameRegistry reg; registerStructViewNames(reg);
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(R"({"recipes":[
        {"name":"r_alarm_line","desc":"告警行","template":"${camera}:${time}|${person}@${rect}|${feat_ids}|${boxes}"}]})").ok);

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{
        1717171717, &p,
        {100, 200, 300, 400, {7, 8}},
        {11, 22, 33, 0, 0, 0, 0, 0},
        {{100,200,300,400,{1,2}}, {110,210,310,410,{3,4}},
         {0,0,0,0,{0,0}}, {0,0,0,0,{0,0}}}
    };
    MyDeviceCtx ctx;
    // person: name-age sep="-" → Alice-30
    // rect: x,y,w,h,tags sep="," → 100,200,300,400,7-8 (tags array sep="-")
    // feat_ids: 11-22-33-0-0-0-0-0
    // boxes: 4 elements each x,y,w,h,tags sep=",", elements sep="|"
    //   → 100,200,300,400,1-2|110,210,310,410,3-4|0,0,0,0,0-0|0,0,0,0,0-0
    CHECK(engine.render("r_alarm_line", &ev, ctx) ==
        "CAM001:1717171717|Alice-30@100,200,300,400,7-8|11-22-33-0-0-0-0-0|100,200,300,400,1-2|110,210,310,410,3-4|0,0,0,0,0-0|0,0,0,0,0-0");
}
