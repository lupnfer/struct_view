#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include "event.h"
#include "device_ctx.h"
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include "registry_gen.cpp"   // brings registerStructViewNames

int main() {
    sv::NameRegistry reg;
    registerStructViewNames(reg);
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);

    std::ifstream f("examples/security/recipes.json");
    if (!f) { std::cerr << "cannot open recipes.json (run from repo root)\n"; return 1; }
    std::stringstream ss; ss << f.rdbuf();
    auto lr = engine.loadConfig(ss.str());
    if (!lr.ok) {
        std::cerr << "loadConfig failed:\n";
        for (auto& e : lr.errors) std::cerr << "  [" << e.recipe << "] " << e.message << "\n";
        return 1;
    }

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{1717171717, &p, {100, 200, 300, 400}};
    MyDeviceCtx ctx;
    std::cout << engine.render("alarm_line", &ev, ctx) << "\n";
    return 0;
}
