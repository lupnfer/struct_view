#include <struct_view/ConfigLoader.hpp>
#include <nlohmann/json.hpp>

namespace sv {

static std::vector<Segment> tokenize(std::string_view tpl) {
    std::vector<Segment> segs;
    std::string literal;
    size_t i = 0;
    while (i < tpl.size()) {
        if (tpl[i] == '$' && i + 1 < tpl.size() && tpl[i+1] == '{') {
            if (!literal.empty()) { segs.push_back({false, std::move(literal), "", ""}); literal.clear(); }
            size_t end = tpl.find('}', i + 2);
            if (end == std::string_view::npos) {
                // unterminated: treat rest as literal
                literal.append(tpl.substr(i));
                break;
            }
            std::string content(tpl.substr(i + 2, end - (i + 2)));
            Segment seg{true, "", "", ""};
            size_t colon = content.find(':');
            if (colon == std::string::npos) {
                seg.text = std::move(content);
            } else {
                seg.text = content.substr(0, colon);
                std::string params = content.substr(colon + 1);
                size_t pos = 0;
                while (pos < params.size()) {
                    size_t nextColon = params.find(':', pos);
                    std::string param = params.substr(pos, nextColon == std::string::npos ? std::string::npos : nextColon - pos);
                    size_t eq = param.find('=');
                    if (eq != std::string::npos) {
                        std::string key = param.substr(0, eq);
                        std::string val = param.substr(eq + 1);
                        if (key == "sep") seg.sepOverride = val;
                        else if (key == "isep") seg.isepOverride = val;
                    }
                    if (nextColon == std::string::npos) break;
                    pos = nextColon + 1;
                }
            }
            segs.push_back(std::move(seg));
            i = end + 1;
        } else {
            literal += tpl[i];
            ++i;
        }
    }
    if (!literal.empty()) segs.push_back({false, std::move(literal), "", ""});
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
