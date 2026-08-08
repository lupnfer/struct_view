#include <struct_view/ConfigLoader.hpp>
#include <nlohmann/json.hpp>

namespace sv {

static std::vector<Segment> tokenize(std::string_view tpl) {
    std::vector<Segment> segs;
    std::string literal;
    size_t i = 0;
    while (i < tpl.size()) {
        if (tpl[i] == '$' && i + 1 < tpl.size() && tpl[i+1] == '{') {
            if (!literal.empty()) { segs.push_back({false, std::move(literal)}); literal.clear(); }
            size_t end = tpl.find('}', i + 2);
            if (end == std::string_view::npos) {
                // unterminated: treat rest as literal
                literal.append(tpl.substr(i));
                break;
            }
            std::string name(tpl.substr(i + 2, end - (i + 2)));
            segs.push_back({true, std::move(name)});
            i = end + 1;
        } else {
            literal += tpl[i];
            ++i;
        }
    }
    if (!literal.empty()) segs.push_back({false, std::move(literal)});
    return segs;
}

ConfigLoader::Result ConfigLoader::parse(std::string_view jsonText) {
    Result r;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonText);
    } catch (const std::exception& ex) {
        r.ok = false;
        r.parseError = ex.what();
        return r;
    }
    auto recipes = j.find("recipes");
    if (recipes == j.end() || !recipes->is_array()) {
        r.ok = false;
        r.parseError = "missing top-level \"recipes\" array";
        return r;
    }
    for (const auto& rc : *recipes) {
        RecipeAst ra;
        ra.name = rc.value("name", "");
        ra.desc = rc.value("desc", "");
        ra.segments = tokenize(rc.value("template", ""));
        r.ast.recipes.push_back(std::move(ra));
    }
    r.ok = true;
    return r;
}

} // namespace sv
