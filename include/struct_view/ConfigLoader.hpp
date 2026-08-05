#pragma once
#include <string>
#include <string_view>
#include "ConfigAst.hpp"
#include "Errors.hpp"

namespace sv {

class ConfigLoader {
public:
    struct Result {
        bool ok = false;
        std::string parseError;
        ConfigAst ast;
    };
    static Result parse(std::string_view jsonText);
};

} // namespace sv
