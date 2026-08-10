#include <struct_view/Server.hpp>
#include <struct_view/ConfigLoader.hpp>
#include <struct_view/Validator.hpp>
#include <struct_view/Builder.hpp>
#include <struct_view/RecipeRunner.hpp>
#include <httplib/httplib.h>
#include <nlohmann/json.hpp>

namespace sv {

Server::Server(NameRegistry& names, ConnectorLib& connectors, Engine& engine,
               const void* sampleStructPtr, std::size_t sampleStructSize,
               const DeviceCtx& sampleCtx)
    : names_(names), connectors_(connectors), engine_(engine),
      sampleStructPtr_(sampleStructPtr), sampleStructSize_(sampleStructSize),
      sampleCtx_(sampleCtx) {}

void Server::listen(int port) {
    httplib::Server svr;

    // GET /api/names — flat name list with type/desc/sep metadata (spec §5.1).
    svr.Get("/api/names", [this](const httplib::Request&, httplib::Response& res) {
        nlohmann::json j;
        nlohmann::json namesArr = nlohmann::json::array();
        for (const auto& n : names_.exportNames()) {
            namesArr.push_back({
                {"name", n.name}, {"type", n.type}, {"desc", n.desc},
                {"canSep", n.canSep}, {"canIsep", n.canIsep},
                {"defaultSep", n.defaultSep}, {"defaultIsep", n.defaultIsep}
            });
        }
        j["names"] = namesArr;
        j["recipes"] = nlohmann::json::array();
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/preview — temp compile + render against the sample struct.
    // Does NOT replace the Engine's store (spec §4 preview semantics).
    svr.Post("/api/preview", [this](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json reqJson;
        try { reqJson = nlohmann::json::parse(req.body); }
        catch (...) {
            res.set_content(R"({"ok":false,"errors":[{"recipe":"","message":"invalid JSON"}]})", "application/json");
            return;
        }
        std::string recipeName = reqJson.value("recipeName", "");

        // Pass the whole body — ConfigLoader reads only "recipes" and ignores
        // extra fields like "recipeName". Re-dumping just the array would lose
        // the {"recipes":[...]} wrapper that parse() expects.
        auto parsed = ConfigLoader::parse(req.body);
        if (!parsed.ok) {
            nlohmann::json err; err["ok"] = false;
            err["errors"] = nlohmann::json::array({{{"recipe",""},{"message","parse error: " + parsed.parseError}}});
            res.set_content(err.dump(), "application/json"); return;
        }
        auto verrors = Validator::validate(parsed.ast, names_, connectors_);
        if (!verrors.empty()) {
            nlohmann::json err; err["ok"] = false;
            nlohmann::json errArr = nlohmann::json::array();
            for (auto& e : verrors) errArr.push_back({{"recipe", e.recipe}, {"message", e.message}});
            err["errors"] = errArr;
            res.set_content(err.dump(), "application/json"); return;
        }
        auto built = Builder::compile(parsed.ast, names_, connectors_);
        if (!built.ok) {
            nlohmann::json err; err["ok"] = false;
            nlohmann::json errArr = nlohmann::json::array();
            for (auto& e : built.errors) errArr.push_back({{"recipe", e.recipe}, {"message", e.message}});
            err["errors"] = errArr;
            res.set_content(err.dump(), "application/json"); return;
        }
        auto it = built.recipes.find(recipeName);
        if (it == built.recipes.end()) {
            nlohmann::json err; err["ok"] = false;
            err["errors"] = nlohmann::json::array({{{"recipe",recipeName},{"message","recipe not found"}}});
            res.set_content(err.dump(), "application/json"); return;
        }
        std::string output = runRecipeB(*it->second, sampleStructPtr_, sampleCtx_);
        nlohmann::json ok; ok["ok"] = true; ok["output"] = output;
        res.set_content(ok.dump(), "application/json");
    });

    // POST /api/deploy — hot-reload: parse+validate+compile+publish (spec §4).
    svr.Post("/api/deploy", [this](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json reqJson;
        try { reqJson = nlohmann::json::parse(req.body); }
        catch (...) {
            res.set_content(R"({"ok":false,"errors":[{"recipe":"","message":"invalid JSON"}]})", "application/json");
            return;
        }
        // Body is already {"recipes":[...]} — the format loadConfig expects.
        auto lr = engine_.loadConfig(req.body);
        nlohmann::json j; j["ok"] = lr.ok;
        if (!lr.ok) {
            nlohmann::json errArr = nlohmann::json::array();
            for (auto& e : lr.errors) errArr.push_back({{"recipe", e.recipe}, {"message", e.message}});
            j["errors"] = errArr;
        }
        res.set_content(j.dump(), "application/json");
    });

    // Serve static frontend assets from web/ (Task 3+ populates this dir).
    svr.set_mount_point("/", "web");

    svr.listen("0.0.0.0", port);
}

void Server::stop() {}

} // namespace sv
