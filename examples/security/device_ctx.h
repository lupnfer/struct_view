#pragma once
#include <struct_view/DeviceCtx.hpp>
#include <string>
struct MyDeviceCtx : sv::DeviceCtx {
    std::string cameraId() const { return "CAM001"; }
};
