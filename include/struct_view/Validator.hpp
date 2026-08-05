#pragma once
#include <vector>
#include "ConfigAst.hpp"
#include "Errors.hpp"
#include "NameRegistry.hpp"
#include "ConnectorLib.hpp"

namespace sv {

class Validator {
public:
    // Returns ALL errors found (never stops at first). Empty == valid.
    static std::vector<ConfigError> validate(const ConfigAst& ast,
                                             const NameRegistry& names,
                                             const ConnectorLib& connectors);
};

} // namespace sv
