#include <doctest/doctest.h>
#include <cstring>
#include <cstdio>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/ValueProvider.hpp>

namespace {
struct RouteArrBox { int x, y; int tags[2]; };
struct RouteArrEv {
    uint64_t timestamp;
    RouteArrBox boxes[4];
};
struct RouteDeviceCtx : sv::DeviceCtx { std::string cameraId() const { return "CAM001"; } };

static std::string routeArrCfg() {
    // boxes is a struct array with fields=["x","y","tags"], sep="", arraySep="|"
    // so each element renders as x+y+tags (no separator between them), joined by "|"
    return R"({"recipes":[
      {"name":"r_alarm_line","template":"${camera}:${time}|${boxes}"}]})";
}
}

TEST_CASE("Route A: renderA matches Route B output (with arrays)") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const RouteArrEv* e = static_cast<const RouteArrEv*>(p);
        char b[32]; std::snprintf(b,sizeof(b),"%llu",(unsigned long long)e->timestamp); return b;
    }));
    reg.registerProvider("camera", sv::makeProvider([](const void*, const sv::DeviceCtx& base) -> std::string {
        return static_cast<const RouteDeviceCtx&>(base).cameraId();
    }));
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const RouteArrEv* e = static_cast<const RouteArrEv*>(p); return &e->boxes[i];
        }), {"x", "tags"}, 4, "", "|");
    reg.registerProvider("x", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const RouteArrBox* b = static_cast<const RouteArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->x); return buf;
    }));
    reg.registerProvider("y", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const RouteArrBox* b = static_cast<const RouteArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->y); return buf;
    }));
    reg.registerProvider("tags", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {
            const RouteArrBox* b = static_cast<const RouteArrBox*>(p);
            std::string out; char buf[16];
            for (std::size_t i = 0; i < 2; ++i) { if (i) out += "-"; std::snprintf(buf,sizeof(buf),"%d",b->tags[i]); out += buf; }
            return out;
        }));
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    std::string cfg = routeArrCfg();
    REQUIRE(engine.loadConfig(cfg).ok);
    REQUIRE(engine.loadConfigA(cfg).ok);

    RouteArrEv ev{1717171717, {{1,2,{7,8}}, {3,4,{9,10}}, {5,6,{11,12}}, {7,8,{13,14}}}};
    RouteDeviceCtx ctx;
    std::string b = engine.render("r_alarm_line", &ev, ctx);
    std::string a = engine.renderA("r_alarm_line", &ev, ctx);
    CHECK(a == b);
    // boxes: fields x,y,tags sep="" → "1"+"2"+"7-8"="17-8" per element, joined by "|"
    CHECK(a == "CAM001:1717171717|17-8|39-10|511-12|713-14");
}
