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
struct Event { uint64_t timestamp; };
struct MyDeviceCtx : sv::DeviceCtx { std::string cameraId() const { return "C"; } };

static std::string cfg(const std::string& sep) {
    return std::string("{\"recipes\":[{\"name\":\"r\",\"template\":\"${time}") + sep + "${time}\"}]}";
}
}

TEST_CASE("Hot-reload: concurrent render while republishing never crashes") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(cfg("|")).ok);

    Event e{99};
    MyDeviceCtx ctx;
    std::atomic<bool> stop{false};
    std::atomic<long> renders{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]{
            while (!stop.load(std::memory_order_relaxed)) {
                (void)engine.render("r", &e, ctx);
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
    // final render reflects the last loaded config; loop's last iteration
    // i=199 is odd, so the last separator published is "|" -> "99|99".
    CHECK(engine.render("r", &e, ctx) == "99|99");
}
