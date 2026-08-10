#include <doctest/doctest.h>
#include <httplib/httplib.h>
#include <struct_view/Server.hpp>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/ValueProvider.hpp>
#include <cstdio>
#include <thread>
#include <chrono>

namespace {
struct SrvEvent { uint64_t timestamp; };
struct SrvCtx : sv::DeviceCtx { std::string cameraId() const { return "CAM001"; } };

bool serverStarted = false;
std::thread serverThread;

void startServer() {
    if (serverStarted) return;
    static sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const SrvEvent* e = static_cast<const SrvEvent*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }), "时间戳");
    static sv::ConnectorLib lib;
    static sv::Engine engine(reg, lib);
    static SrvEvent sampleEvent{1717171717};
    static SrvCtx sampleCtx;

    static sv::Server server(reg, lib, engine, &sampleEvent, sizeof(SrvEvent), sampleCtx);
    serverThread = std::thread([&]() { server.listen(8089); });
    serverThread.detach();   // otherwise ~thread() terminates on exit (still joinable)
    serverStarted = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}
}

TEST_CASE("Server: GET /api/names returns name list") {
    startServer();
    httplib::Client cli("localhost", 8089);
    auto res = cli.Get("/api/names");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("time") != std::string::npos);
    CHECK(res->body.find("时间戳") != std::string::npos);
}

TEST_CASE("Server: POST /api/preview returns rendered output") {
    startServer();
    httplib::Client cli("localhost", 8089);
    auto res = cli.Post("/api/preview",
        R"({"recipes":[{"name":"r_preview","template":"${time}"}],"recipeName":"r_preview"})",
        "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("1717171717") != std::string::npos);
    CHECK(res->body.find("\"ok\":true") != std::string::npos);
}

TEST_CASE("Server: POST /api/deploy returns ok on valid recipe") {
    startServer();
    httplib::Client cli("localhost", 8089);
    auto res = cli.Post("/api/deploy",
        R"({"recipes":[{"name":"r_test","template":"${time}"}]})",
        "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("\"ok\":true") != std::string::npos);
}
