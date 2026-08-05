#pragma once
#include <string>
namespace sv {
struct RecipeB;
struct RecipeA;
class DeviceCtx;
std::string runRecipeB(const RecipeB& r, const void* structPtr, const DeviceCtx& ctx);
std::string runRecipeA(const RecipeA& r, const void* structPtr, const DeviceCtx& ctx);
} // namespace sv
