#pragma once
#include <string>
namespace sv {
struct RecipeB;
class DeviceCtx;
std::string runRecipeB(const RecipeB& r, const void* structPtr, const DeviceCtx& ctx);
} // namespace sv
