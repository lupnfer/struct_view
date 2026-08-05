#pragma once
#include <string>
#include <vector>

namespace sv {

struct Segment {
    bool isRef;         // true => text is a name to resolve; false => literal
    std::string text;
};

struct RecipeAst {
    std::string name;
    std::vector<Segment> segments;
};

struct ConfigAst {
    std::vector<RecipeAst> recipes;
};

} // namespace sv
