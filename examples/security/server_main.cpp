#include <cstring>
#include <iostream>
#include "event.h"
#include "device_ctx.h"
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/Server.hpp>
#include "registry_gen.cpp"

int main() {
    sv::NameRegistry reg;
    registerStructViewNames(reg);
    sv::ConnectorLib lib;
    lib.add("dash", "-");
    lib.add("pipe", "|");

    sv::Engine engine(reg, lib);
    // Load initial recipe
    engine.loadConfig(R"({"recipes":[
        {"name":"r_alarm_line","desc":"告警行","template":"${camera}:${time}|${person}@${rect}|${feat_ids}|${boxes}"}]})");

    // Sample data for preview
    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event sampleEvent{
        1717171717, &p,
        {100, 200, 300, 400, {7, 8}},
        {11, 22, 33, 0, 0, 0, 0, 0},
        {{100,200,300,400,{1,2}}, {110,210,310,410,{3,4}},
         {0,0,0,0,{0,0}}, {0,0,0,0,{0,0}}}
    };
    MyDeviceCtx sampleCtx;

    std::cout << "Recipe Builder UI: http://localhost:8080\n";
    sv::Server server(reg, lib, engine, &sampleEvent, sizeof(Event), sampleCtx);
    server.listen(8080);
    return 0;
}
