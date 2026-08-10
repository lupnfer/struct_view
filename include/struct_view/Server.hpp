#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include "NameRegistry.hpp"
#include "ConnectorLib.hpp"
#include "Engine.hpp"
#include "DeviceCtx.hpp"

namespace sv {

// Embedded HTTP server for the Recipe Builder UI (spec §4). Exposes three
// JSON endpoints over a cpp-httplib instance:
//   GET  /api/names   — flat list of registered names + metadata
//   POST /api/preview — temp compile + render against the sample struct (no store mutation)
//   POST /api/deploy  — hot-reload config into the Engine
// Also mounts web/ as a static-file root for the frontend assets.
class Server {
    NameRegistry& names_;
    ConnectorLib& connectors_;
    Engine& engine_;
    const void* sampleStructPtr_;
    std::size_t sampleStructSize_;
    const DeviceCtx& sampleCtx_;
public:
    Server(NameRegistry& names, ConnectorLib& connectors, Engine& engine,
           const void* sampleStructPtr, std::size_t sampleStructSize,
           const DeviceCtx& sampleCtx);
    // Blocks the calling thread until stop(). Run on a dedicated thread.
    void listen(int port);
    void stop();
};

} // namespace sv
