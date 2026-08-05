#include <doctest/doctest.h>
#include <struct_view/DeviceCtx.hpp>
#include <struct_view/Errors.hpp>

namespace { struct MyDeviceCtx : sv::DeviceCtx {
    std::string cameraId() const { return "CAM001"; }
}; }

TEST_CASE("DeviceCtx: subclass usable as base ref") {
    MyDeviceCtx ctx;
    const sv::DeviceCtx& base = ctx;
    CHECK(static_cast<const MyDeviceCtx&>(base).cameraId() == "CAM001");
}

TEST_CASE("Errors: LoadResult defaults to not-ok with no errors") {
    sv::LoadResult r;
    CHECK_FALSE(r.ok);
    CHECK(r.errors.empty());
}
