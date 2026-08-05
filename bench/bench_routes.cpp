#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include "event.h"
#include "device_ctx.h"
#include "registry_gen.cpp"
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>

int main() {
    sv::NameRegistry reg; registerStructViewNames(reg);
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    const char* cfg =
        R"({"recipes":[
          {"name":"alarm_line","template":"${camera}:${time}|${person}@${rect}"},
          {"name":"person","template":"${name}-${age}"},
          {"name":"rect","template":"${x},${y},${w},${h}"}]})";
    if (!engine.loadConfig(cfg).ok)  { std::cerr << "loadB failed\n"; return 1; }
    if (!engine.loadConfigA(cfg).ok) { std::cerr << "loadA failed\n"; return 1; }

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{1717171717, &p, {100, 200, 300, 400}};
    MyDeviceCtx ctx;

    const long N = 2000000;
    // Sink that prevents the compiler from eliding the render calls. We feed the
    // rendered string's length into a volatile size_t; the length depends on
    // runtime snprintf formatting, so the compiler cannot compute it without
    // actually constructing the result string. (A volatile std::string would not
    // compile: std::string::operator= is not volatile-qualified under libc++.)
    volatile std::size_t sink = 0;

    // Warm up (ensure both stores are hot, branch prediction warm).
    for (long i = 0; i < 10000; ++i) { sink = engine.render("alarm_line", &ev, ctx).size(); sink = engine.renderA("alarm_line", &ev, ctx).size(); }

    auto t0 = std::chrono::high_resolution_clock::now();
    for (long i = 0; i < N; ++i) sink = engine.render("alarm_line", &ev, ctx).size();
    auto t1 = std::chrono::high_resolution_clock::now();
    for (long i = 0; i < N; ++i) sink = engine.renderA("alarm_line", &ev, ctx).size();
    auto t2 = std::chrono::high_resolution_clock::now();

    double b_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double a_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::cout << "Route B (ValueProvider*): " << b_ms << " ms  (" << (b_ms / N * 1000) << " us/render)\n";
    std::cout << "Route A (four-kind Step): " << a_ms << " ms  (" << (a_ms / N * 1000) << " us/render)\n";
    std::cout << "Speedup A/B: " << (b_ms / a_ms) << "x\n";
    return 0;
}
