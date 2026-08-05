#pragma once
#include <string>

namespace sv {

// Base for user-provided device context. Users subclass and add their own
// getters; codegen emits ValueProvider lambdas that downcast to the concrete
// type (declared in schema as "deviceCtxType") and call the getter.
class DeviceCtx {
public:
    virtual ~DeviceCtx() = default;
};

} // namespace sv
