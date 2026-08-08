#include <doctest/doctest.h>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <struct_view/ValueProvider.hpp>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>
#include <cstdio>

namespace {
struct HrBox { int v; };
struct HrEv { uint64_t timestamp; HrBox boxes[4]; };
struct MyDeviceCtx : sv::DeviceCtx { std::string cameraId() const { return "C"; } };

static std::string cfg(const std::string& sep) {
    // recipe "r_test": ${time}${sep}${boxes} (boxes is a struct array, fields=["v"])
    return std::string("{\"recipes\":[{\"name\":\"r_test\",\"template\":\"${time}") + sep +
           "${boxes}\"}]}";
}
}

TEST_CASE("Hot-reload: concurrent render while republishing never crashes") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const HrEv* e = static_cast<const HrEv*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const HrEv* e = static_cast<const HrEv*>(p); return &e->boxes[i];
        }), {"v"}, 4, "", "|");
    reg.registerProvider("v", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const HrBox* b = static_cast<const HrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->v); return buf;
    }));
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(cfg("|")).ok);

    HrEv e{99, {{1},{2},{3},{4}} };
    MyDeviceCtx ctx;
    std::atomic<bool> stop{false};
    std::atomic<long> renders{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]{
            while (!stop.load(std::memory_order_relaxed)) {
                (void)engine.render("r_test", &e, ctx);
                renders.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // repeatedly hot-reload with different separator
    for (int i = 0; i < 200; ++i) {
        engine.loadConfig(cfg(i % 2 ? "|" : "-"));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();
    CHECK(renders.load() > 0);
    // final render reflects last loaded config. Loop's last iteration i=199 (odd) -> sep="|".
    // recipe "r_test" = ${time}|${boxes} -> "99" + "|" + "1|2|3|4" = "99|1|2|3|4"
    CHECK(engine.render("r_test", &e, ctx) == "99|1|2|3|4");
}

TEST_CASE("Hot-reload (Route A): concurrent renderA while republishing never crashes") {
    // M-4: Route A's storeA_ + ArrayStructBlockProviderA under concurrent renderA + reload.
    // Same RCU structure as Route B (RecipeStore<RecipeA> publishAll/snapshot, recipe-private
    // providers in RecipeA::ownedProviders). TSan must report 0 races.
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const HrEv* e = static_cast<const HrEv*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const HrEv* e = static_cast<const HrEv*>(p); return &e->boxes[i];
        }), {"v"}, 4, "", "|");
    reg.registerProvider("v", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const HrBox* b = static_cast<const HrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->v); return buf;
    }));
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfigA(cfg("|")).ok);

    HrEv e{99, {{1},{2},{3},{4}} };
    MyDeviceCtx ctx;
    std::atomic<bool> stop{false};
    std::atomic<long> renders{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]{
            while (!stop.load(std::memory_order_relaxed)) {
                (void)engine.renderA("r_test", &e, ctx);
                renders.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        engine.loadConfigA(cfg(i % 2 ? "|" : "-"));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();
    CHECK(renders.load() > 0);
    // Parity with Route B: same recipe, same data -> same final output.
    CHECK(engine.renderA("r_test", &e, ctx) == "99|1|2|3|4");
}
