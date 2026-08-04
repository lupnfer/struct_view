# struct_view Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a C++ framework that turns upstream C structs (security video structured data) into strings, driven by JSON "recipe" configs that users can hot-reload, with a build-time codegen tool that generates the name vocabulary from a description file.

**Architecture:** Load-time pipeline (parse JSON → validate against registries → compile to immutable `Recipe` → atomically publish). Runtime hot path is a straight-line walk of `Recipe` steps. Names are registered by build-time Python codegen (`schema.json` → `registry_gen.cpp`) referencing real `&T::member` so layout drift fails the build. Dual-track Step representation (Route B unified `ValueProvider*` first; Route A explicit four-kind + benchmark second) per spec section 3.2.

**Tech Stack:** C++17, CMake, nlohmann/json (vendored single-header, load-time only), doctest (vendored single-header, tests), Python 3 (build-time codegen only).

**Spec:** `docs/superpowers/specs/2026-08-04-struct-view-design.md`

---

## File Structure

```
struct_view/
├── CMakeLists.txt
├── third_party/
│   ├── nlohmann/json.hpp              # vendored, load-time JSON parse only
│   └── doctest/doctest.h              # vendored, tests only
├── include/struct_view/
│   ├── Errors.hpp                     # ConfigError, LoadResult
│   ├── DeviceCtx.hpp                  # sv::DeviceCtx base (user subclasses)
│   ├── ValueProvider.hpp             # ValueProvider, LambdaProvider, makeProvider, Navigator, ConnectorProvider, StructBlockProvider
│   ├── NameRegistry.hpp              # NameRegistry (holds fields/devices/structblocks)
│   ├── ConnectorLib.hpp              # ConnectorLib (name → literal)
│   ├── Recipe.hpp                    # StepB/RecipeB (Phase 1), StepA/RecipeA/StepKind (Phase 2)
│   ├── RecipeStore.hpp               # RecipeStore<RecipeT> (atomic publish via shared_mutex)
│   ├── ConfigAst.hpp                 # Segment, RecipeAst, ConfigAst
│   ├── ConfigLoader.hpp             # ConfigLoader::parse
│   ├── Validator.hpp                 # Validator::validate (full error collection + cycle detect)
│   ├── Builder.hpp                   # Builder::compile (Route B)
│   ├── RecipeRunner.hpp             # runRecipeB free function (shared by Engine + StructBlockProvider)
│   └── Engine.hpp                    # Engine (loadConfig, render)
├── src/                               # one .cpp per header above (where logic lives)
├── tools/
│   └── gen_registry.py               # codegen: schema.json → registry_gen.cpp
├── examples/security/
│   ├── event.h                        # Event/PersonInfo/Box (upstream C structs)
│   ├── device_ctx.h                  # MyDeviceCtx : sv::DeviceCtx
│   ├── schema.json                   # description file
│   ├── recipes.json                  # recipe config
│   ├── registry_gen.cpp             # generated (by gen_registry.py)
│   └── main.cpp                      # end-to-end demo
└── tests/
    ├── test_connector_lib.cpp
    ├── test_value_provider.cpp
    ├── test_name_registry.cpp
    ├── test_config_loader.cpp
    ├── test_validator.cpp
    ├── test_recipe_store.cpp
    ├── test_builder.cpp
    ├── test_engine.cpp
    ├── test_hot_reload.cpp
    ├── test_codegen.py
    └── test_example_e2e.cpp
```

**Responsibility boundaries:** Registries hold long-lived providers (NameRegistry owns field/device/structblock providers; ConnectorLib owns literals). `RecipeB` owns the `ConnectorProvider`s it creates (recipe-specific). `RecipeStore` owns recipes via `shared_ptr` (atomic swap). `Engine` owns NameRegistry + ConnectorLib + RecipeStore and exposes only `loadConfig`/`render`. Codegen is a build-time tool that produces a single `.cpp` calling `NameRegistry` registration helpers — it knows nothing about recipes or the engine.

---

# Phase 1 — Complete working framework (Route B)

Route B = unified `StepB { const ValueProvider* provider; }`. Every segment (field, device, struct block, connector, literal) is a `ValueProvider`; the engine loop is `for (step) out += step.provider->get(ptr, ctx)`. This delivers the whole pipeline end-to-end before the Route A optimization is layered on.

## Task 1: Project scaffold + vendored deps

**Files:**
- Create: `CMakeLists.txt`
- Create: `third_party/nlohmann/json.hpp`
- Create: `third_party/doctest/doctest.h`
- Create: `tests/test_scaffold.cpp`

- [ ] **Step 1: Vendor single-header libraries**

Run:
```bash
mkdir -p third_party/nlohmann third_party/doctest
curl -fsSL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp -o third_party/nlohmann/json.hpp
curl -fsSL https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h -o third_party/doctest/doctest.h
test -s third_party/nlohmann/json.hpp && echo "json ok"
test -s third_party/doctest/doctest.h && echo "doctest ok"
```
Expected: prints `json ok` and `doctest ok`. If offline, place the two single-header files manually at those paths.

- [ ] **Step 2: Write CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.14)
project(struct_view CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(struct_view INTERFACE)
target_include_directories(struct_view INTERFACE
    include
    third_party
)

enable_testing()

# Each test file is its own executable with its own doctest main.
file(GLOB SV_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(t ${SV_TESTS})
    get_filename_component(name ${t} NAME_WE)
    add_executable(${name} ${t} src/*.cpp)
    target_link_libraries(${name} PRIVATE struct_view)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# End-to-end example executable (added in Task 14; stubbed here so glob works later)
```

- [ ] **Step 3: Write scaffold test**

`tests/test_scaffold.cpp`:
```cpp
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <string>

TEST_CASE("scaffold: headers vendor and build") {
    nlohmann::json j = {{"name", "alarm_line"}};
    CHECK(j["name"].get<std::string>() == "alarm_line");
}
```

- [ ] **Step 4: Build and run**

Run:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: `test_scaffold` PASS.

> Note: Task 1's test executable compiles `src/*.cpp` (currently empty glob → no sources) plus the test file. Until `src/` has files this is fine; later tasks add `.cpp` files picked up automatically.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt third_party tests/test_scaffold.cpp
git commit -m "build: scaffold CMake + vendor nlohmann/json and doctest"
```

---

## Task 2: Errors + DeviceCtx base

**Files:**
- Create: `include/struct_view/Errors.hpp`
- Create: `include/struct_view/DeviceCtx.hpp`
- Create: `tests/test_value_provider.cpp` (DeviceCtx part)

- [ ] **Step 1: Write Errors.hpp**

`include/struct_view/Errors.hpp`:
```cpp
#pragma once
#include <string>
#include <vector>

namespace sv {

struct ConfigError {
    std::string recipe;   // recipe name the error is about (empty if global)
    std::string message;  // human-readable
    size_t line = 0;      // 0 if unknown
};

struct LoadResult {
    bool ok = false;
    std::vector<ConfigError> errors;  // empty when ok
};

} // namespace sv
```

- [ ] **Step 2: Write DeviceCtx.hpp**

`include/struct_view/DeviceCtx.hpp`:
```cpp
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
```

- [ ] **Step 3: Write failing test**

`tests/test_value_provider.cpp`:
```cpp
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
```

- [ ] **Step 4: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_value_provider --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/struct_view/Errors.hpp include/struct_view/DeviceCtx.hpp tests/test_value_provider.cpp
git commit -m "feat: add ConfigError/LoadResult and DeviceCtx base"
```

---

## Task 3: ValueProvider hierarchy + makeProvider + Navigator

**Files:**
- Create: `include/struct_view/ValueProvider.hpp`
- Create: `src/ValueProvider.cpp`
- Modify: `tests/test_value_provider.cpp` (append cases)

- [ ] **Step 1: Write failing tests (append to test_value_provider.cpp)**

```cpp
#include <struct_view/ValueProvider.hpp>
#include <struct_view/Recipe.hpp>   // RecipeB forward used by StructBlockProvider

namespace {
struct Event { uint64_t timestamp; };
}

TEST_CASE("ValueProvider: makeProvider lambda reads field") {
    auto p = sv::makeProvider([](const void* ptr, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(ptr);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)e->timestamp);
        return buf;
    });
    Event e{1717171717};
    CHECK(p->get(&e, *static_cast<const sv::DeviceCtx*>(nullptr)) == "1717171717");
}

TEST_CASE("ValueProvider: ConnectorProvider emits literal") {
    sv::ConnectorProvider dash("-");
    sv::DeviceCtx dummy;
    CHECK(dash.get(nullptr, dummy) == "-");
}

TEST_CASE("Navigator: stateless lambda converts to function ptr") {
    sv::Navigator nav([](const void* p) -> const void* {
        // pretend embedded sub-struct at offset 0 (toy)
        return p;
    });
    int x = 5;
    CHECK(nav.navigate(&x) == &x);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | head -20`
Expected: FAIL to compile (`makeProvider`, `ConnectorProvider`, `Navigator` undefined).

- [ ] **Step 3: Write ValueProvider.hpp**

`include/struct_view/ValueProvider.hpp`:
```cpp
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include "DeviceCtx.hpp"

namespace sv {

struct RecipeB;  // forward — defined in Recipe.hpp

// Type-erased value source. Hot path calls get() per step.
class ValueProvider {
public:
    virtual ~ValueProvider() = default;
    virtual std::string get(const void* structPtr, const DeviceCtx& ctx) const = 0;
};

// Wraps a stateless/stateful lambda: (const void*, const DeviceCtx&) -> std::string.
// Used by codegen for fields and device getters.
template <typename F>
class LambdaProvider : public ValueProvider {
    F fn_;
public:
    explicit LambdaProvider(F fn) : fn_(std::move(fn)) {}
    std::string get(const void* p, const DeviceCtx& ctx) const override { return fn_(p, ctx); }
};

template <typename F>
std::unique_ptr<ValueProvider> makeProvider(F fn) {
    return std::make_unique<LambdaProvider<F>>(std::move(fn));
}

// From a parent struct pointer, yield a child struct pointer.
// Constructed from a stateless lambda converting to const void*(*)(const void*).
class Navigator {
    const void* (*fn_)(const void*);
public:
    explicit Navigator(const void* (*f)(const void*)) : fn_(f) {}
    const void* navigate(const void* parent) const { return fn_(parent); }
};

// Route B connector/literal holder.
class ConnectorProvider : public ValueProvider {
    std::string literal_;
public:
    explicit ConnectorProvider(std::string_view lit) : literal_(lit) {}
    std::string get(const void*, const DeviceCtx&) const override { return literal_; }
};

// A whole key struct block: navigate to child, run sub-recipe, return its string.
// sub_ is bound by Builder after all recipes are compiled (see Validator/Builder).
class StructBlockProvider : public ValueProvider {
    Navigator nav_;
    std::string subName_;
    const RecipeB* sub_ = nullptr;
public:
    StructBlockProvider(Navigator nav, std::string subName)
        : nav_(nav), subName_(std::move(subName)) {}
    const std::string& subRecipeName() const { return subName_; }
    void bind(const RecipeB* sub) { sub_ = sub; }
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

} // namespace sv
```

- [ ] **Step 4: Write Recipe.hpp (RecipeB forward target)**

`include/struct_view/Recipe.hpp`:
```cpp
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace sv {

class ValueProvider;  // forward

// Route B step: every segment is a ValueProvider.
struct StepB {
    const ValueProvider* provider = nullptr;
};

struct RecipeB {
    std::string name;
    std::vector<StepB> steps;
    // Owns ConnectorProviders created for this recipe's literals/connectors.
    // Field/device/structblock providers are owned by NameRegistry (longer-lived).
    std::vector<std::unique_ptr<ValueProvider>> ownedProviders;
};

} // namespace sv
```

- [ ] **Step 5: Write ValueProvider.cpp (StructBlockProvider::get needs runRecipeB)**

`src/ValueProvider.cpp`:
```cpp
#include <struct_view/ValueProvider.hpp>
#include <struct_view/Recipe.hpp>
#include <struct_view/RecipeRunner.hpp>

namespace sv {

std::string StructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    // sub_ must be bound by Builder; if not bound, emit nothing (defensive).
    if (!sub_) return {};
    const void* child = nav_.navigate(structPtr);
    return runRecipeB(*sub_, child, ctx);
}

} // namespace sv
```

`include/struct_view/RecipeRunner.hpp`:
```cpp
#pragma once
#include <string>
namespace sv {
struct RecipeB;
class DeviceCtx;
std::string runRecipeB(const RecipeB& r, const void* structPtr, const DeviceCtx& ctx);
} // namespace sv
```

`src/RecipeRunner.cpp`:
```cpp
#include <struct_view/RecipeRunner.hpp>
#include <struct_view/Recipe.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <string>

namespace sv {

std::string runRecipeB(const RecipeB& r, const void* structPtr, const DeviceCtx& ctx) {
    std::string out;
    out.reserve(64);
    for (const StepB& s : r.steps) {
        if (s.provider) out += s.provider->get(structPtr, ctx);
    }
    return out;
}

} // namespace sv
```

- [ ] **Step 6: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_value_provider --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add include/struct_view/ValueProvider.hpp include/struct_view/Recipe.hpp include/struct_view/RecipeRunner.hpp src/ValueProvider.cpp src/RecipeRunner.cpp tests/test_value_provider.cpp
git commit -m "feat: ValueProvider hierarchy, makeProvider, Navigator, ConnectorProvider, StructBlockProvider"
```

---

## Task 4: ConnectorLib

**Files:**
- Create: `include/struct_view/ConnectorLib.hpp`
- Create: `src/ConnectorLib.cpp`
- Create: `tests/test_connector_lib.cpp`

- [ ] **Step 1: Write failing test**

`tests/test_connector_lib.cpp`:
```cpp
#include <doctest/doctest.h>
#include <struct_view/ConnectorLib.hpp>

TEST_CASE("ConnectorLib: add and get") {
    sv::ConnectorLib lib;
    lib.add("dash", "-");
    lib.add("pipe", "|");
    CHECK(lib.get("dash") == "-");
    CHECK(lib.get("pipe") == "|");
}

TEST_CASE("ConnectorLib: unknown connector returns nullopt") {
    sv::ConnectorLib lib;
    CHECK_FALSE(lib.get("nope").has_value());
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | head`
Expected: FAIL (ConnectorLib undefined).

- [ ] **Step 3: Write ConnectorLib.hpp**

`include/struct_view/ConnectorLib.hpp`:
```cpp
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sv {

// Service-provided connector library: name -> literal/format string.
class ConnectorLib {
    std::unordered_map<std::string, std::string> entries_;
public:
    void add(std::string name, std::string_view literal);
    std::optional<std::string> get(const std::string& name) const;
};

} // namespace sv
```

- [ ] **Step 4: Write ConnectorLib.cpp**

`src/ConnectorLib.cpp`:
```cpp
#include <struct_view/ConnectorLib.hpp>
namespace sv {
void ConnectorLib::add(std::string name, std::string_view literal) {
    entries_[std::move(name)] = std::string(literal);
}
std::optional<std::string> ConnectorLib::get(const std::string& name) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}
} // namespace sv
```

- [ ] **Step 5: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_connector_lib --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/struct_view/ConnectorLib.hpp src/ConnectorLib.cpp tests/test_connector_lib.cpp
git commit -m "feat: ConnectorLib name->literal registry"
```

---

## Task 5: NameRegistry

**Files:**
- Create: `include/struct_view/NameRegistry.hpp`
- Create: `src/NameRegistry.cpp`
- Create: `tests/test_name_registry.cpp`

- [ ] **Step 1: Write failing test**

`tests/test_name_registry.cpp`:
```cpp
#include <doctest/doctest.h>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/Recipe.hpp>

namespace { struct Event { uint64_t timestamp; }; }

TEST_CASE("NameRegistry: registerProvider + lookup") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp);
        return b;
    }));
    const sv::ValueProvider* vp = reg.lookup("time");
    REQUIRE(vp != nullptr);
    Event e{42};
    sv::DeviceCtx dummy;
    CHECK(vp->get(&e, dummy) == "42");
}

TEST_CASE("NameRegistry: registerStruct stores name + subRecipe name") {
    sv::NameRegistry reg;
    reg.registerStruct("person", sv::Navigator([](const void*) -> const void* { return nullptr; }), "person");
    auto blocks = reg.structBlocks();
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].first == "person");
    CHECK(blocks[0].second->subRecipeName() == "person");
}

TEST_CASE("NameRegistry: unknown lookup returns nullptr") {
    sv::NameRegistry reg;
    CHECK(reg.lookup("nope") == nullptr);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | head`
Expected: FAIL.

- [ ] **Step 3: Write NameRegistry.hpp**

`include/struct_view/NameRegistry.hpp`:
```cpp
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "ValueProvider.hpp"

namespace sv {

class NameRegistry {
    std::unordered_map<std::string, std::unique_ptr<ValueProvider>> entries_;
    // Raw pointers into entries_ for struct blocks (lifetime tied to entries_).
    std::vector<std::pair<std::string, StructBlockProvider*>> structBlocks_;
public:
    // For fields and device getters (codegen emits makeProvider lambdas).
    void registerProvider(std::string name, std::unique_ptr<ValueProvider> vp);
    // For key struct blocks: navigator + sub-recipe name (resolved by Builder).
    void registerStruct(std::string name, Navigator nav, std::string subRecipeName);

    const ValueProvider* lookup(const std::string& name) const;

    // For Builder binding and Validator cycle/exists checks.
    const std::vector<std::pair<std::string, StructBlockProvider*>>& structBlocks() const;
};

} // namespace sv
```

- [ ] **Step 4: Write NameRegistry.cpp**

`src/NameRegistry.cpp`:
```cpp
#include <struct_view/NameRegistry.hpp>
namespace sv {
void NameRegistry::registerProvider(std::string name, std::unique_ptr<ValueProvider> vp) {
    entries_[std::move(name)] = std::move(vp);
}
void NameRegistry::registerStruct(std::string name, Navigator nav, std::string subRecipeName) {
    auto up = std::make_unique<StructBlockProvider>(nav, subRecipeName);
    StructBlockProvider* raw = up.get();
    entries_[name] = std::move(up);
    structBlocks_.emplace_back(std::move(name), raw);
}
const ValueProvider* NameRegistry::lookup(const std::string& name) const {
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : it->second.get();
}
const std::vector<std::pair<std::string, StructBlockProvider*>>& NameRegistry::structBlocks() const {
    return structBlocks_;
}
} // namespace sv
```

- [ ] **Step 5: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_name_registry --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/struct_view/NameRegistry.hpp src/NameRegistry.cpp tests/test_name_registry.cpp
git commit -m "feat: NameRegistry with provider + struct-block registration"
```

---

## Task 6: ConfigAst + ConfigLoader (template tokenizer)

**Files:**
- Create: `include/struct_view/ConfigAst.hpp`
- Create: `include/struct_view/ConfigLoader.hpp`
- Create: `src/ConfigLoader.cpp`
- Create: `tests/test_config_loader.cpp`

- [ ] **Step 1: Write failing test**

`tests/test_config_loader.cpp`:
```cpp
#include <doctest/doctest.h>
#include <struct_view/ConfigLoader.hpp>

TEST_CASE("ConfigLoader: parses recipes and tokenizes template") {
    std::string text = R"({
      "recipes": [
        { "name": "alarm", "template": "${cam}:${time}|literal" },
        { "name": "person", "template": "${name}-${age}" }
      ]
    })";
    auto result = sv::ConfigLoader::parse(text);
    REQUIRE(result.ok);
    REQUIRE(result.ast.recipes.size() == 2);
    CHECK(result.ast.recipes[0].name == "alarm");
    // segments: ${cam} : ${time} | literal
    const auto& segs = result.ast.recipes[0].segments;
    REQUIRE(segs.size() == 4);
    CHECK(segs[0].isRef);  CHECK(segs[0].text == "cam");
    CHECK(!segs[1].isRef); CHECK(segs[1].text == ":");
    CHECK(segs[2].isRef);  CHECK(segs[2].text == "time");
    CHECK(!segs[3].isRef); CHECK(segs[3].text == "|literal");
}

TEST_CASE("ConfigLoader: malformed JSON reports error, no ast") {
    auto result = sv::ConfigLoader::parse("{ not json");
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.parseError.empty());
}

TEST_CASE("ConfigLoader: lone $ not followed by { is literal") {
    auto result = sv::ConfigLoader::parse(R"({"recipes":[{"name":"r","template":"a$b"}]})");
    REQUIRE(result.ok);
    REQUIRE(result.ast.recipes[0].segments.size() == 1);
    CHECK(!result.ast.recipes[0].segments[0].isRef);
    CHECK(result.ast.recipes[0].segments[0].text == "a$b");
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | head`
Expected: FAIL.

- [ ] **Step 3: Write ConfigAst.hpp**

`include/struct_view/ConfigAst.hpp`:
```cpp
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
```

- [ ] **Step 4: Write ConfigLoader.hpp**

`include/struct_view/ConfigLoader.hpp`:
```cpp
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
```

- [ ] **Step 5: Write ConfigLoader.cpp (JSON parse + template tokenizer)**

`src/ConfigLoader.cpp`:
```cpp
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
        ra.segments = tokenize(rc.value("template", ""));
        r.ast.recipes.push_back(std::move(ra));
    }
    r.ok = true;
    return r;
}

} // namespace sv
```

- [ ] **Step 6: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_config_loader --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add include/struct_view/ConfigAst.hpp include/struct_view/ConfigLoader.hpp src/ConfigLoader.cpp tests/test_config_loader.cpp
git commit -m "feat: ConfigLoader parses JSON and tokenizes ${name} templates"
```

---

## Task 7: Validator (full error collection + cycle detection)

**Files:**
- Create: `include/struct_view/Validator.hpp`
- Create: `src/Validator.cpp`
- Create: `tests/test_validator.cpp`

- [ ] **Step 1: Write failing test**

`tests/test_validator.cpp`:
```cpp
#include <doctest/doctest.h>
#include <struct_view/Validator.hpp>
#include <struct_view/ConfigLoader.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>

namespace { struct Event { uint64_t timestamp; }; }

static sv::ConfigAst parse(std::string_view t) {
    return sv::ConfigLoader::parse(t).ast;  // assume well-formed in tests
}

TEST_CASE("Validator: collects ALL missing names at once") {
    auto ast = parse(R"({"recipes":[{"name":"r","template":"${a}${b}:${c}"}]})");
    sv::NameRegistry reg;
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    // a, b, c all missing -> 3 errors (collect, don't stop at first)
    CHECK(errs.size() == 3);
}

TEST_CASE("Validator: known field + connector + literal pass clean") {
    auto ast = parse(R"({"recipes":[
        {"name":"r","template":"${time}|${dash}lit"}]})");
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string();}));
    sv::ConnectorLib lib;
    lib.add("dash", "-");
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}

TEST_CASE("Validator: struct block subRecipe must exist as a recipe") {
    auto ast = parse(R"({"recipes":[{"name":"main","template":"${person}"}]})");
    sv::NameRegistry reg;
    reg.registerStruct("person", sv::Navigator([](const void*)->const void*{return nullptr;}), "person");
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool mentionsPerson = false;
    for (auto& e : errs) if (e.message.find("person") != std::string::npos) mentionsPerson = true;
    CHECK(mentionsPerson);
}

TEST_CASE("Validator: detects cyclic struct-block dependency") {
    // main -> person(structblock, subRecipe "person")
    // recipe "person" -> addr(structblock, subRecipe "main")  => cycle main->person->main
    auto ast = parse(R"({"recipes":[
        {"name":"main","template":"${person}"},
        {"name":"person","template":"${addr}"}]})");
    sv::NameRegistry reg;
    reg.registerStruct("person", sv::Navigator([](const void*)->const void*{return nullptr;}), "person");
    reg.registerStruct("addr", sv::Navigator([](const void*)->const void*{return nullptr;}), "main");
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    bool foundCycle = false;
    for (auto& e : errs) if (e.message.find("cycle") != std::string::npos) foundCycle = true;
    CHECK(foundCycle);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | head`
Expected: FAIL.

- [ ] **Step 3: Write Validator.hpp**

`include/struct_view/Validator.hpp`:
```cpp
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
```

- [ ] **Step 4: Write Validator.cpp**

`src/Validator.cpp`:
```cpp
#include <struct_view/Validator.hpp>
#include <unordered_map>
#include <unordered_set>

namespace sv {

static std::unordered_map<std::string, const RecipeAst*> indexRecipes(const ConfigAst& ast) {
    std::unordered_map<std::string, const RecipeAst*> m;
    for (const auto& r : ast.recipes) m[r.name] = &r;
    return m;
}

// Walk the struct-block -> subRecipe -> referenced structblock graph; detect cycles.
static bool hasCycle(const NameRegistry& names,
                     const std::unordered_map<std::string, const RecipeAst*>& byName) {
    // dependency edge: structblock X (subRecipe S) => X depends on every structblock
    // referenced by recipe S's template.
    std::unordered_set<std::string> visiting, visited;
    // returns true if a cycle is reachable from structblock `start`
    std::function<bool(const std::string&)> dfs = [&](const std::string& blockName) -> bool {
        if (visiting.count(blockName)) return true;
        if (visited.count(blockName)) return false;
        visiting.insert(blockName);
        // find this structblock's subRecipe
        std::string sub;
        for (auto& b : names.structBlocks()) if (b.first == blockName) sub = b.second->subRecipeName();
        if (!sub.empty()) {
            auto it = byName.find(sub);
            if (it != byName.end()) {
                for (const auto& seg : it->second->segments) {
                    if (seg.isRef) {
                        // is seg.text a structblock?
                        for (auto& b : names.structBlocks()) if (b.first == seg.text) {
                            if (dfs(seg.text)) { visiting.erase(blockName); visited.insert(blockName); return true; }
                        }
                    }
                }
            }
        }
        visiting.erase(blockName);
        visited.insert(blockName);
        return false;
    };
    for (auto& b : names.structBlocks()) if (dfs(b.first)) return true;
    return false;
}

std::vector<ConfigError> Validator::validate(const ConfigAst& ast,
                                             const NameRegistry& names,
                                             const ConnectorLib& connectors) {
    std::vector<ConfigError> errs;
    auto byName = indexRecipes(ast);

    // 1. each referenced name resolves to a provider or a connector
    for (const auto& r : ast.recipes) {
        for (const auto& seg : r.segments) {
            if (!seg.isRef) continue;
            if (!names.lookup(seg.text) && !connectors.get(seg.text)) {
                errs.push_back({r.name, "unknown name: " + seg.text, 0});
            }
        }
    }

    // 2. each struct block's subRecipe exists as a recipe
    for (auto& b : names.structBlocks()) {
        if (!byName.count(b.second->subRecipeName())) {
            errs.push_back({b.first, "struct block subRecipe not found: " + b.second->subRecipeName(), 0});
        }
    }

    // 3. no cyclic struct-block dependencies
    if (hasCycle(names, byName)) {
        errs.push_back({"", "cycle detected in struct-block -> subRecipe dependencies", 0});
    }

    return errs;
}

} // namespace sv
```

> Add `#include <functional>` at top of `Validator.cpp` (for `std::function`). (Place it with the other includes.)

- [ ] **Step 5: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_validator --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/struct_view/Validator.hpp src/Validator.cpp tests/test_validator.cpp
git commit -m "feat: Validator collects all errors + detects struct-block cycles"
```

---

## Task 8: RecipeStore (atomic publish via shared_mutex)

**Files:**
- Create: `include/struct_view/RecipeStore.hpp`
- Create: `src/RecipeStore.cpp`
- Create: `tests/test_recipe_store.cpp`

- [ ] **Step 1: Write failing test**

`tests/test_recipe_store.cpp`:
```cpp
#include <doctest/doctest.h>
#include <struct_view/RecipeStore.hpp>
#include <struct_view/Recipe.hpp>

TEST_CASE("RecipeStore: publish then snapshot") {
    sv::RecipeStore<sv::RecipeB> store;
    auto r = std::make_shared<const sv::RecipeB>();
    r->name = "alarm";
    store.publish("alarm", r);
    auto snap = store.snapshot("alarm");
    REQUIRE(snap);
    CHECK((*snap)->name == "alarm");
}

TEST_CASE("RecipeStore: republish swaps atomically") {
    sv::RecipeStore<sv::RecipeB> store;
    auto r1 = std::make_shared<const sv::RecipeB>(); r1->name = "v1";
    store.publish("r", r1);
    auto r2 = std::make_shared<const sv::RecipeB>(); r2->name = "v2";
    store.publish("r", r2);
    CHECK((*store.snapshot("r"))->name == "v2");
}

TEST_CASE("RecipeStore: missing recipe snapshot returns null") {
    sv::RecipeStore<sv::RecipeB> store;
    CHECK_FALSE(store.snapshot("nope"));
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | head`
Expected: FAIL.

- [ ] **Step 3: Write RecipeStore.hpp**

`include/struct_view/RecipeStore.hpp`:
```cpp
#pragma once
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace sv {

// Owns recipes by name. publish() takes a unique lock; snapshot() takes a
// shared lock (readers don't block each other). C++17: no atomic shared_ptr,
// so a shared_mutex is the pragmatic near-lock-free choice (spec section 7).
template <typename RecipeT>
class RecipeStore {
    std::unordered_map<std::string, std::shared_ptr<const RecipeT>> map_;
    mutable std::shared_mutex mtx_;
public:
    void publish(std::string name, std::shared_ptr<const RecipeT> recipe);
    // Returns a snapshot that keeps the recipe alive for the call's duration.
    std::shared_ptr<const RecipeT> snapshot(const std::string& name) const;
};

} // namespace sv
```

- [ ] **Step 4: Write RecipeStore.cpp (out-of-line for the mutex; template only in header for the map)**

Since `RecipeStore` is a template, put the method bodies in the header:

`include/struct_view/RecipeStore.hpp` (append method defs):
```cpp
template <typename RecipeT>
void RecipeStore<RecipeT>::publish(std::string name, std::shared_ptr<const RecipeT> recipe) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    map_[std::move(name)] = std::move(recipe);
}

template <typename RecipeT>
std::shared_ptr<const RecipeT> RecipeStore<RecipeT>::snapshot(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    auto it = map_.find(name);
    if (it == map_.end()) return nullptr;
    return it->second;  // copy bumps refcount; recipe stays alive after unlock
}
```

(No `.cpp` needed — delete `src/RecipeStore.cpp` from the plan if created; do not create it.)

- [ ] **Step 5: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_recipe_store --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/struct_view/RecipeStore.hpp tests/test_recipe_store.cpp
git commit -m "feat: RecipeStore atomic publish/snapshot via shared_mutex"
```

---

## Task 9: Builder (compile AST → RecipeB, bind struct blocks)

**Files:**
- Create: `include/struct_view/Builder.hpp`
- Create: `src/Builder.cpp`
- Create: `tests/test_builder.cpp`

- [ ] **Step 1: Write failing test**

`tests/test_builder.cpp`:
```cpp
#include <doctest/doctest.h>
#include <struct_view/Builder.hpp>
#include <struct_view/ConfigLoader.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/DeviceCtx.hpp>

namespace {
struct PersonInfo { char name[32]; int age; };
struct Event { uint64_t timestamp; PersonInfo person; };
}

TEST_CASE("Builder: compiles literals + field refs into RecipeB") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r","template":"${time}-${lit}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.store.snapshot("r");
    REQUIRE(snap);
    Event e{1717171717, {}};
    sv::DeviceCtx dummy;
    CHECK((*snap)->steps.size() == 3);  // time, "-", lit
    // exercise runRecipeB indirectly via StructBlockProvider-less path:
    std::string out;
    for (auto& s : (*snap)->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "1717171717-lit");
}

TEST_CASE("Builder: binds struct block sub-recipe pointers") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"main","template":"[${person}]"},
        {"name":"person","template":"${name}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerStruct("person", sv::Navigator([](const void* p) -> const void* {
        const Event* e = static_cast<const Event*>(p); return &e->person;
    }), "person");
    reg.registerProvider("name", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const PersonInfo* pi = static_cast<const PersonInfo*>(p);
        return std::string(pi->name);
    }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.store.snapshot("main");
    REQUIRE(snap);
    Event e{0, {}};
    std::strncpy(e.person.name, "Alice", 32);
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : (*snap)->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "[Alice]");
}
```

> Test file needs `#include <cstring>` for `std::strncpy`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | head`
Expected: FAIL.

- [ ] **Step 3: Write Builder.hpp**

`include/struct_view/Builder.hpp`:
```cpp
#pragma once
#include "ConfigAst.hpp"
#include "Errors.hpp"
#include "NameRegistry.hpp"
#include "ConnectorLib.hpp"
#include "RecipeStore.hpp"
#include "Recipe.hpp"

namespace sv {

class Builder {
public:
    struct Result {
        bool ok = false;
        std::vector<ConfigError> errors;
        RecipeStore<RecipeB> store;
    };
    // Assumes ast already passed Validator. Compiles each recipe, binds struct
    // blocks, publishes all. Returns ok=true with store populated on success.
    static Result compile(const ConfigAst& ast,
                          NameRegistry& names,
                          const ConnectorLib& connectors);
};

} // namespace sv
```

- [ ] **Step 4: Write Builder.cpp**

`src/Builder.cpp`:
```cpp
#include <struct_view/Builder.hpp>
#include <struct_view/ValueProvider.hpp>
#include <unordered_map>

namespace sv {

Builder::Result Builder::compile(const ConfigAst& ast,
                                 NameRegistry& names,
                                 const ConnectorLib& connectors) {
    Result r;

    // Pass 1: compile each recipe into a RecipeB (structblock sub_ left null).
    std::unordered_map<std::string, std::shared_ptr<const RecipeB>> byName;
    for (const auto& ra : ast.recipes) {
        auto rb = std::make_shared<RecipeB>();
        rb->name = ra.name;
        for (const auto& seg : ra.segments) {
            StepB step;
            if (seg.isRef) {
                if (const ValueProvider* vp = names.lookup(seg.text)) {
                    step.provider = vp;                      // field/device/structblock (NameRegistry-owned)
                } else if (auto lit = connectors.get(seg.text)) {
                    auto cp = std::make_unique<ConnectorProvider>(*lit);
                    step.provider = cp.get();
                    rb->ownedProviders.push_back(std::move(cp));
                } else {
                    // Validator should have caught this; defensive.
                    r.errors.push_back({ra.name, "unresolved name at compile: " + seg.text, 0});
                    continue;
                }
            } else {
                auto cp = std::make_unique<ConnectorProvider>(seg.text);
                step.provider = cp.get();
                rb->ownedProviders.push_back(std::move(cp));
            }
            rb->steps.push_back(step);
        }
        byName[ra.name] = rb;
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 2: bind struct-block sub-recipe pointers.
    for (auto& b : names.structBlocks()) {
        auto it = byName.find(b.second->subRecipeName());
        if (it == byName.end()) {
            r.errors.push_back({b.first, "subRecipe missing at bind: " + b.second->subRecipeName(), 0});
            continue;
        }
        b.second->bind(it->second.get());
    }

    if (!r.errors.empty()) { r.ok = false; return r; }

    // Pass 3: publish.
    for (auto& [name, rb] : byName) {
        r.store.publish(name, rb);
    }
    r.ok = true;
    return r;
}

} // namespace sv
```

- [ ] **Step 5: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_builder --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/struct_view/Builder.hpp src/Builder.cpp tests/test_builder.cpp
git commit -m "feat: Builder compiles AST to RecipeB and binds struct-block sub-recipes"
```

---

## Task 10: Engine (loadConfig + render)

**Files:**
- Create: `include/struct_view/Engine.hpp`
- Create: `src/Engine.cpp`
- Create: `tests/test_engine.cpp`

- [ ] **Step 1: Write failing test**

`tests/test_engine.cpp`:
```cpp
#include <doctest/doctest.h>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <struct_view/ValueProvider.hpp>

namespace {
struct Event { uint64_t timestamp; };
struct MyDeviceCtx : sv::DeviceCtx {
    std::string cameraId() const { return "CAM001"; }
};
}

TEST_CASE("Engine: loadConfig + render end-to-end (Route B)") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    reg.registerProvider("camera", sv::makeProvider([](const void*, const sv::DeviceCtx& base) -> std::string {
        return static_cast<const MyDeviceCtx&>(base).cameraId();
    }));
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);

    auto lr = engine.loadConfig(R"({"recipes":[
        {"name":"alarm","template":"${camera}:${time}"}]})");
    REQUIRE(lr.ok);
    REQUIRE(lr.errors.empty());

    Event e{1717171717};
    MyDeviceCtx ctx;
    CHECK(engine.render("alarm", &e, ctx) == "CAM001:1717171717");
}

TEST_CASE("Engine: invalid config returns errors and does not publish") {
    sv::NameRegistry reg;
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    auto lr = engine.loadConfig(R"({"recipes":[{"name":"r","template":"${missing}"}]})");
    CHECK_FALSE(lr.ok);
    CHECK_FALSE(lr.errors.empty());
    MyDeviceCtx ctx;
    CHECK(engine.render("r", nullptr, ctx) == "");  // not published
}

TEST_CASE("Engine: unknown recipe renders empty (no crash)") {
    sv::NameRegistry reg; sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    MyDeviceCtx ctx;
    CHECK(engine.render("nope", nullptr, ctx) == "");
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | head`
Expected: FAIL.

- [ ] **Step 3: Write Engine.hpp**

`include/struct_view/Engine.hpp`:
```cpp
#pragma once
#include <string>
#include <string_view>
#include "Errors.hpp"
#include "NameRegistry.hpp"
#include "ConnectorLib.hpp"
#include "RecipeStore.hpp"
#include "Recipe.hpp"
#include "DeviceCtx.hpp"

namespace sv {

class Engine {
    NameRegistry& names_;
    ConnectorLib& connectors_;
    RecipeStore<RecipeB> store_;
public:
    Engine(NameRegistry& names, ConnectorLib& connectors);

    // Load/reload config: parse -> validate -> compile -> publish atomically.
    // On any failure returns errors and leaves existing recipes in place.
    LoadResult loadConfig(std::string_view jsonText);

    // Hot path: read-only snapshot, straight-line walk. Never throws.
    std::string render(const std::string& recipeName,
                       const void* structPtr,
                       const DeviceCtx& ctx) const;
};

} // namespace sv
```

- [ ] **Step 4: Write Engine.cpp**

`src/Engine.cpp`:
```cpp
#include <struct_view/Engine.hpp>
#include <struct_view/ConfigLoader.hpp>
#include <struct_view/Validator.hpp>
#include <struct_view/Builder.hpp>
#include <struct_view/RecipeRunner.hpp>

namespace sv {

Engine::Engine(NameRegistry& names, ConnectorLib& connectors)
    : names_(names), connectors_(connectors) {}

LoadResult Engine::loadConfig(std::string_view jsonText) {
    LoadResult lr;
    auto parsed = ConfigLoader::parse(jsonText);
    if (!parsed.ok) {
        lr.ok = false;
        lr.errors.push_back({"", "parse error: " + parsed.parseError, 0});
        return lr;
    }
    auto verrors = Validator::validate(parsed.ast, names_, connectors_);
    if (!verrors.empty()) {
        lr.ok = false;
        lr.errors = std::move(verrors);
        return lr;
    }
    auto built = Builder::compile(parsed.ast, names_, connectors_);
    if (!built.ok) {
        lr.ok = false;
        lr.errors = std::move(built.errors);
        return lr;
    }
    // Success: swap in the new store. (Store owns recipes via shared_ptr.)
    store_ = std::move(built.store);
    lr.ok = true;
    return lr;
}

std::string Engine::render(const std::string& recipeName,
                           const void* structPtr,
                           const DeviceCtx& ctx) const {
    auto snap = store_.snapshot(recipeName);
    if (!snap) return "";
    return runRecipeB(**snap, structPtr, ctx);
}

} // namespace sv
```

> `RecipeStore` has no copy assignment by default (shared_mutex is non-copyable). Add `RecipeStore` move support: in `RecipeStore.hpp` declare `RecipeStore(RecipeStore&&) = default; RecipeStore& operator=(RecipeStore&&) = default;` and delete copy. Add these lines inside the class body:
> ```cpp
> RecipeStore(const RecipeStore&) = delete;
> RecipeStore& operator=(const RecipeStore&) = delete;
> RecipeStore(RecipeStore&&) = default;
> RecipeStore& operator=(RecipeStore&&) = default;
> ```

- [ ] **Step 5: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_engine --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/struct_view/Engine.hpp include/struct_view/RecipeStore.hpp src/Engine.cpp tests/test_engine.cpp
git commit -m "feat: Engine loadConfig + render (Route B hot path)"
```

---

## Task 11: Hot-reload concurrency test

**Files:**
- Create: `tests/test_hot_reload.cpp`

- [ ] **Step 1: Write the test**

`tests/test_hot_reload.cpp`:
```cpp
#include <doctest/doctest.h>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <struct_view/ValueProvider.hpp>
#include <thread>
#include <atomic>
#include <vector>

namespace {
struct Event { uint64_t timestamp; };
struct MyDeviceCtx : sv::DeviceCtx { std::string cameraId() const { return "C"; } };

static std::string cfg(const std::string& sep) {
    return std::string("{\"recipes\":[{\"name\":\"r\",\"template\":\"${time}") + sep + "${time}\"}]}";
}
}

TEST_CASE("Hot-reload: concurrent render while republishing never crashes") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* e = static_cast<const Event*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(cfg("|")).ok);

    Event e{99};
    MyDeviceCtx ctx;
    std::atomic<bool> stop{false};
    std::atomic<long> renders{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]{
            while (!stop.load(std::memory_order_relaxed)) {
                (void)engine.render("r", &e, ctx);
                renders.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // repeatedly hot-reload with different separator
    for (int i = 0; i < 200; ++i) {
        engine.loadConfig(cfg(i % 2 ? "|" : "-"));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();
    CHECK(renders.load() > 0);
    // final render reflects last loaded config
    CHECK(engine.render("r", &e, ctx) == "99-99");
}
```

- [ ] **Step 2: Build and run (with TSan if available)**

Run:
```bash
cmake --build build
ctest --test-dir build -R test_hot_reload --output-on-failure
```
Optionally: `cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread && cmake --build build_tsan && ./build_tsan/tests/test_hot_reload`
Expected: PASS, no TSan data-race report.

- [ ] **Step 3: Commit**

```bash
git add tests/test_hot_reload.cpp
git commit -m "test: concurrent hot-reload never crashes or races"
```

---

## Task 12: Codegen tool (`gen_registry.py`)

**Files:**
- Create: `tools/gen_registry.py`
- Create: `tests/test_codegen.py`

The codegen reads `schema.json` and emits `registry_gen.cpp` containing `sv::NameRegistry` registration calls. Field entries carry a `type` tag (`uint64`/`int`/`float`/`cstr`) so the emitted `snprintf` cast is type-safe (resolves spec section 11 item 5 + formatting for `char[]`). Struct-block pointer-vs-embedded is handled by the emitted lambda directly (`s->person` for pointer, `&s->rect` for embedded) — driven by a `ptr` flag in schema (the one place a layout hint is needed, since the C type isn't known to Python).

- [ ] **Step 1: Write failing Python test**

`tests/test_codegen.py`:
```python
import json, subprocess, sys, os, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
GEN = ROOT / "tools" / "gen_registry.py"

SCHEMA = {
    "deviceCtxType": "MyDeviceCtx",
    "structs": [
        {"name": "Event", "members": [
            {"field": "timestamp", "as": "time", "type": "uint64", "fmt": "%llu"},
            {"block": "person", "ptr": True, "subRecipe": "person"},
            {"block": "rect", "ptr": False, "subRecipe": "rect"}
        ]},
        {"name": "PersonInfo", "members": [
            {"field": "name", "type": "cstr", "fmt": "%s"},
            {"field": "age", "type": "int", "fmt": "%d"}
        ]},
        {"name": "Box", "members": [
            {"field": "x", "type": "int", "fmt": "%d"}
        ]}
    ],
    "device": [{"getter": "cameraId", "as": "camera"}]
}

def run_gen(schema_text, out_path):
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(schema_text); schema_path = f.name
    r = subprocess.run([sys.executable, str(GEN), schema_path, str(out_path)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return out_path.read_text()

def test_emits_field_with_typed_cast():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp())
    assert 'registerProvider("time"' in out
    assert '(unsigned long long)' in out   # uint64 cast
    assert 's->timestamp' in out

def test_emits_cstr_field():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp())
    assert 'registerProvider("name"' in out
    assert '(const char*)' in out
    assert 's->name' in out

def test_emits_struct_block_pointer_and_embedded():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp())
    assert 'return static_cast<const void*>(s->person);' in out   # ptr
    assert 'return static_cast<const void*>(&s->rect);' in out    # embedded

def test_emits_device_getter_with_downcast():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp())
    assert 'registerProvider("camera"' in out
    assert 'static_cast<const MyDeviceCtx&>(base)' in out
    assert 'ctx.cameraId()' in out
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 tests/test_codegen.py`
Expected: FAIL (gen_registry.py missing).

- [ ] **Step 3: Write `tools/gen_registry.py`**

```python
#!/usr/bin/env python3
"""Generate registry_gen.cpp from a struct description (schema.json).

Usage: gen_registry.py schema.json output.cpp
Emits sv::NameRegistry registration calls referencing real struct members,
so layout drift fails the C++ build.
"""
import json, sys, pathlib

HEADER = """\
// AUTO-GENERATED by tools/gen_registry.py — DO NOT EDIT.
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <cstdio>
#include <string>

namespace { // file-local helpers to keep emitted names collision-free
}

void registerStructViewNames(sv::NameRegistry& reg) {
"""

FOOTER = "}\n"

TYPE_CAST = {
    "uint64": "(unsigned long long)",
    "int":    "(int)",
    "float":  "(double)",
    "cstr":   "(const char*)",
}

def field_line(struct_name, member):
    field = member["field"]
    as_name = member.get("as", field)
    typ = member["type"]
    fmt = member["fmt"]
    cast = TYPE_CAST[typ]
    # cstr: char arrays decay; cast to const char* is safe for char[N] and char*
    return f'''    reg.registerProvider("{as_name}", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "{fmt}", {cast}s->{field});
            return std::string(buf);
        }}));\n'''

def block_line(struct_name, member):
    block = member["block"]
    sub = member["subRecipe"]
    ptr = member.get("ptr", False)
    deref = f"static_cast<const void*>(s->{block})" if ptr else f"static_cast<const void*>(&s->{block})"
    return f'''    reg.registerStruct("{block}", sv::Navigator(
        [](const void* p) -> const void* {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            return {deref};
        }}), "{sub}");\n'''

def device_line(device_ctx_type, dev):
    getter = dev["getter"]
    as_name = dev.get("as", getter)
    return f'''    reg.registerProvider("{as_name}", sv::makeProvider(
        [](const void*, const sv::DeviceCtx& base) -> std::string {{
            const {device_ctx_type}& ctx = static_cast<const {device_ctx_type}&>(base);
            return ctx.{getter}();
        }}));\n'''

def gen(schema):
    device_ctx_type = schema.get("deviceCtxType", "sv::DeviceCtx")
    out = HEADER
    for st in schema.get("structs", []):
        sn = st["name"]
        for m in st.get("members", []):
            if "field" in m:
                out += field_line(sn, m)
            elif "block" in m:
                out += block_line(sn, m)
    for d in schema.get("device", []):
        out += device_line(device_ctx_type, d)
    out += FOOTER
    return out

def main():
    if len(sys.argv) != 3:
        print("usage: gen_registry.py schema.json output.cpp", file=sys.stderr)
        sys.exit(2)
    schema = json.loads(pathlib.Path(sys.argv[1]).read_text())
    pathlib.Path(sys.argv[2]).write_text(gen(schema))

if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run tests**

Run: `python3 tests/test_codegen.py`
Expected: all 4 PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/gen_registry.py tests/test_codegen.py
git commit -m "feat: gen_registry.py codegen from schema.json to registry_gen.cpp"
```

---

## Task 13: Drift-detection test (codegen output fails compile when .h changes)

Verifies the spec's "编译期漂移兜底": if a field is renamed/removed in the header, the generated `registry_gen.cpp` fails to compile.

**Files:**
- Create: `tests/drift_header.h` (a throwaway header the test mutates)
- Create: `tests/test_drift.sh`

- [ ] **Step 1: Write a baseline header + schema + generated cpp**

`tests/drift_header.h`:
```c
#pragma once
#include <stdint.h>
typedef struct { uint64_t timestamp; } DriftEvent;
```

`tests/drift_schema.json`:
```json
{
  "deviceCtxType": "sv::DeviceCtx",
  "structs": [{"name": "DriftEvent", "members": [
      {"field": "timestamp", "as": "time", "type": "uint64", "fmt": "%llu"}
  ]}]
}
```

- [ ] **Step 2: Write the drift test script**

`tests/test_drift.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tools/gen_registry.py tests/drift_schema.json tests/drift_gen.cpp

cat > tests/drift_main.cpp <<'EOF'
#include "drift_header.h"
#include <struct_view/NameRegistry.hpp>
#include "drift_gen.cpp"
int main() { sv::NameRegistry r; registerStructViewNames(r); return 0; }
EOF

echo "=== Step A: baseline compiles ==="
if g++ -std=c++17 -Iinclude -Ithird_party tests/drift_main.cpp -o /tmp/drift_ok; then
  echo "baseline OK"
else
  echo "ERROR: baseline should compile"; exit 1
fi

echo "=== Step B: rename field -> compile MUST fail ==="
cat > tests/drift_header.h <<'EOF'
#pragma once
#include <stdint.h>
typedef struct { uint64_t renamed; } DriftEvent;
EOF
if g++ -std=c++17 -Iinclude -Ithird_party tests/drift_main.cpp -o /tmp/drift_bad 2>/dev/null; then
  echo "ERROR: rename should have failed the build (drift not caught)"; exit 1
else
  echo "drift caught at compile (expected)"
fi

# restore
cat > tests/drift_header.h <<'EOF'
#pragma once
#include <stdint.h>
typedef struct { uint64_t timestamp; } DriftEvent;
EOF
echo "ALL OK"
```

- [ ] **Step 3: Run it**

Run: `chmod +x tests/test_drift.sh && ./tests/test_drift.sh`
Expected: prints `baseline OK`, `drift caught at compile (expected)`, `ALL OK`.

- [ ] **Step 4: Commit**

```bash
git add tests/drift_header.h tests/drift_schema.json tests/test_drift.sh
git commit -m "test: codegen output fails compile on header drift"
```

---

## Task 14: End-to-end security example

**Files:**
- Create: `examples/security/event.h`
- Create: `examples/security/device_ctx.h`
- Create: `examples/security/schema.json`
- Create: `examples/security/recipes.json`
- Create: `examples/security/registry_gen.cpp` (generated)
- Create: `examples/security/main.cpp`
- Create: `tests/test_example_e2e.cpp`
- Modify: `CMakeLists.txt` (add example target + codegen custom command)

- [ ] **Step 1: Write upstream C structs**

`examples/security/event.h`:
```c
#pragma once
#include <stdint.h>
typedef struct { char name[32]; int age; } PersonInfo;
typedef struct { int x, y, w, h; } Box;
typedef struct {
    uint64_t timestamp;
    PersonInfo* person;   // pointer member (key struct)
    Box rect;             // embedded member (key struct)
} Event;
```

- [ ] **Step 2: Write user device context**

`examples/security/device_ctx.h`:
```cpp
#pragma once
#include <struct_view/DeviceCtx.hpp>
#include <string>
struct MyDeviceCtx : sv::DeviceCtx {
    std::string cameraId() const { return "CAM001"; }
};
```

- [ ] **Step 3: Write schema.json**

`examples/security/schema.json`:
```json
{
  "deviceCtxType": "MyDeviceCtx",
  "structs": [
    {"name": "Event", "members": [
        {"field": "timestamp", "as": "time", "type": "uint64", "fmt": "%llu"},
        {"block": "person", "ptr": true,  "subRecipe": "person"},
        {"block": "rect",   "ptr": false, "subRecipe": "rect"}
    ]},
    {"name": "PersonInfo", "members": [
        {"field": "name", "as": "name", "type": "cstr", "fmt": "%s"},
        {"field": "age",  "as": "age",  "type": "int",   "fmt": "%d"}
    ]},
    {"name": "Box", "members": [
        {"field": "x", "type": "int", "fmt": "%d"},
        {"field": "y", "type": "int", "fmt": "%d"},
        {"field": "w", "type": "int", "fmt": "%d"},
        {"field": "h", "type": "int", "fmt": "%d"}
    ]}
  ],
  "device": [{"getter": "cameraId", "as": "camera"}]
}
```

- [ ] **Step 4: Generate registry_gen.cpp**

Run: `python3 tools/gen_registry.py examples/security/schema.json examples/security/registry_gen.cpp`
Expected: file created; spot-check it contains `registerStructViewNames`.

- [ ] **Step 5: Write recipes.json**

`examples/security/recipes.json`:
```json
{
  "recipes": [
    { "name": "alarm_line", "template": "${camera}:${time}|${person}@${rect}" },
    { "name": "person",     "template": "${name}-${age}" },
    { "name": "rect",       "template": "${x},${y},${w},${h}" }
  ]
}
```

- [ ] **Step 6: Write main.cpp**

`examples/security/main.cpp`:
```cpp
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include "event.h"
#include "device_ctx.h"
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include "registry_gen.cpp"   // brings registerStructViewNames

int main() {
    sv::NameRegistry reg;
    registerStructViewNames(reg);
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);

    std::ifstream f("examples/security/recipes.json");
    std::stringstream ss; ss << f.rdbuf();
    auto lr = engine.loadConfig(ss.str());
    if (!lr.ok) {
        std::cerr << "loadConfig failed:\n";
        for (auto& e : lr.errors) std::cerr << "  [" << e.recipe << "] " << e.message << "\n";
        return 1;
    }

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{1717171717, &p, {100, 200, 300, 400}};
    MyDeviceCtx ctx;
    std::cout << engine.render("alarm_line", &ev, ctx) << "\n";
    return 0;
}
```

- [ ] **Step 7: Add CMake target + codegen custom command**

Append to `CMakeLists.txt`:
```cmake
# --- End-to-end security example ---
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/examples/security/registry_gen.cpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_registry.py
            ${CMAKE_SOURCE_DIR}/examples/security/schema.json
            ${CMAKE_SOURCE_DIR}/examples/security/registry_gen.cpp
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_registry.py
            ${CMAKE_SOURCE_DIR}/examples/security/schema.json)
find_package(Python3 REQUIRED)
add_executable(security_example
    examples/security/main.cpp
    ${CMAKE_SOURCE_DIR}/examples/security/registry_gen.cpp
    src/ValueProvider.cpp src/RecipeRunner.cpp src/NameRegistry.cpp
    src/ConnectorLib.cpp src/ConfigLoader.cpp src/Validator.cpp
    src/Builder.cpp src/Engine.cpp)
target_include_directories(security_example PRIVATE
    examples/security include third_party)
```

- [ ] **Step 8: Write E2E test**

`tests/test_example_e2e.cpp`:
```cpp
#include <doctest/doctest.h>
#include <cstring>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include "event.h"
#include "device_ctx.h"
#include "registry_gen.cpp"

TEST_CASE("E2E: security alarm_line matches expected string") {
    sv::NameRegistry reg; registerStructViewNames(reg);
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(R"({"recipes":[
        {"name":"alarm_line","template":"${camera}:${time}|${person}@${rect}"},
        {"name":"person","template":"${name}-${age}"},
        {"name":"rect","template":"${x},${y},${w},${h}"}]})").ok);

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{1717171717, &p, {100, 200, 300, 400}};
    MyDeviceCtx ctx;
    CHECK(engine.render("alarm_line", &ev, ctx) == "CAM001:1717171717|Alice-30@100,200,300,400");
}
```

- [ ] **Step 9: Build and run all**

Run:
```bash
cmake --build build
ctest --test-dir build --output-on-failure
./build/security_example
```
Expected: all tests PASS; example prints `CAM001:1717171717|Alice-30@100,200,300,400`.

- [ ] **Step 10: Commit**

```bash
git add examples/security CMakeLists.txt tests/test_example_e2e.cpp
git commit -m "feat: end-to-end security example (Event/PersonInfo/Box) with codegen"
```

---

# Phase 2 — Route A (explicit four-kind Step) + benchmark

Route A avoids the virtual call for connectors (literal) and devices (getter ptr), and inlines sub-recipe navigation. It reuses the `ValueProvider` hierarchy (fields stay `LambdaProvider`; struct blocks reuse `Navigator`), but compiles to a different `StepA`/`RecipeA` and a switched render loop. Both routes are benchmarked; the winner is recorded (spec section 3.2).

## Task 15: StepA / RecipeA + BuilderA + Engine.renderA

**Files:**
- Modify: `include/struct_view/Recipe.hpp` (append StepA/RecipeA/StepKind)
- Create: `include/struct_view/BuilderA.hpp`
- Create: `src/BuilderA.cpp`
- Modify: `include/struct_view/Engine.hpp` / `src/Engine.cpp` (add `renderA`, `storeA_`)
- Create: `tests/test_route_a.cpp`

- [ ] **Step 1: Append Route A types to Recipe.hpp**

```cpp
// ---- Route A: explicit four-kind step (no virtual call for connectors/devices) ----
enum class StepKind { Field, Connector, Device, SubRecipe };

struct FieldBinding  { const ValueProvider* provider; };  // scalar field (one virtual call)
struct ConnectorBinding { std::string literal; };          // pre-extracted literal
struct SubRecipeBinding { Navigator nav; const RecipeA* sub; };

struct StepA {
    StepKind kind;
    // Device binding uses a type-erased getter; keep simple with a ValueProvider* for device too,
    // OR a function pointer. Here: store device as a ValueProvider* (device providers are LambdaProviders).
    // To truly avoid virtual for device, codegen would emit a fn ptr; left as provider* for parity.
    std::variant<FieldBinding, ConnectorBinding, FieldBinding /*device reuses FieldBinding w/ device provider*/, SubRecipeBinding> binding;
    // tag which: we encode device vs field by a separate bool when needed.
};

struct RecipeA {
    std::string name;
    std::vector<StepA> steps;
};
```

> Simplification note for the implementer: the variant above is awkward (device and field both use `FieldBinding`). Cleaner: add `DeviceBinding { const ValueProvider* provider; }` as a distinct fourth type so the variant has four unique types and `kind` is derivable. Use that cleaner form:
```cpp
struct DeviceBinding    { const ValueProvider* provider; };
struct StepA {
    StepKind kind;
    std::variant<FieldBinding, ConnectorBinding, DeviceBinding, SubRecipeBinding> binding;
};
struct RecipeA; // forward for SubRecipeBinding
struct SubRecipeBinding { Navigator nav; const RecipeA* sub; };
struct StepA { StepKind kind; std::variant<FieldBinding, ConnectorBinding, DeviceBinding, SubRecipeBinding> binding; };
struct RecipeA { std::string name; std::vector<StepA> steps; };
```
(Define `SubRecipeBinding`, `StepA`, `RecipeA` in that order after the bindings, with `RecipeA` forward-declared before `SubRecipeBinding`.) Replace the first sketch with this clean version in the file.

- [ ] **Step 2: Write failing test**

`tests/test_route_a.cpp`:
```cpp
#include <doctest/doctest.h>
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include <struct_view/ValueProvider.hpp>
#include "event.h"
#include "device_ctx.h"
#include "registry_gen.cpp"

TEST_CASE("Route A: renderA matches Route B output") {
    sv::NameRegistry reg; registerStructViewNames(reg);
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(R"({"recipes":[
        {"name":"alarm_line","template":"${camera}:${time}|${person}@${rect}"},
        {"name":"person","template":"${name}-${age}"},
        {"name":"rect","template":"${x},${y},${w},${h}"}]})").ok);

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{1717171717, &p, {100, 200, 300, 400}};
    MyDeviceCtx ctx;
    std::string b = engine.render("alarm_line", &ev, ctx);    // Route B
    std::string a = engine.renderA("alarm_line", &ev, ctx);   // Route A
    CHECK(a == b);
    CHECK(a == "CAM001:1717171717|Alice-30@100,200,300,400");
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build 2>&1 | head`
Expected: FAIL (`renderA` undefined).

- [ ] **Step 4: Write BuilderA.hpp**

`include/struct_view/BuilderA.hpp`:
```cpp
#pragma once
#include "ConfigAst.hpp"
#include "Errors.hpp"
#include "NameRegistry.hpp"
#include "ConnectorLib.hpp"
#include "RecipeStore.hpp"
#include "Recipe.hpp"

namespace sv {
class BuilderA {
public:
    struct Result {
        bool ok = false;
        std::vector<ConfigError> errors;
        RecipeStore<RecipeA> store;
    };
    static Result compile(const ConfigAst& ast, NameRegistry& names, const ConnectorLib& connectors);
};
} // namespace sv
```

- [ ] **Step 5: Write BuilderA.cpp**

`src/BuilderA.cpp`:
```cpp
#include <struct_view/BuilderA.hpp>
#include <struct_view/ValueProvider.hpp>
#include <unordered_map>

namespace sv {

// Distinguish a NameRegistry provider that is a StructBlock vs a scalar/device.
static bool isStructBlock(const NameRegistry& names, const std::string& name) {
    for (auto& b : names.structBlocks()) if (b.first == name) return true;
    return false;
}

BuilderA::Result BuilderA::compile(const ConfigAst& ast, NameRegistry& names, const ConnectorLib& connectors) {
    Result r;
    std::unordered_map<std::string, std::shared_ptr<const RecipeA>> byName;

    for (const auto& ra : ast.recipes) {
        auto rb = std::make_shared<RecipeA>();
        rb->name = ra.name;
        for (const auto& seg : ra.segments) {
            StepA step;
            if (seg.isRef) {
                if (auto lit = connectors.get(seg.text)) {
                    step.kind = StepKind::Connector;
                    step.binding = ConnectorBinding{*lit};
                } else {
                    const ValueProvider* vp = names.lookup(seg.text);
                    if (!vp) { r.errors.push_back({ra.name, "unresolved: " + seg.text, 0}); continue; }
                    if (isStructBlock(names, seg.text)) {
                        // sub-recipe binding: lookup the StructBlockProvider's navigator.
                        Navigator nav([](const void*) -> const void* { return nullptr; });
                        const RecipeA* subPtr = nullptr;
                        for (auto& b : names.structBlocks()) if (b.first == seg.text) {
                            // StructBlockProvider holds nav; but Route A wants nav directly.
                            // We stored nav inside StructBlockProvider (private). Add a public
                            // accessor `const Navigator& navigator() const` to StructBlockProvider
                            // (see Task 3 edit) and use it here.
                            // nav = b.second->navigator();
                            // (left as accessor call; implement accessor in Task 3 header)
                            (void)b;
                        }
                        step.kind = StepKind::SubRecipe;
                        step.binding = SubRecipeBinding{nav, subPtr};
                    } else {
                        step.kind = StepKind::Device;  // treat all non-struct providers as device/field uniformly
                        step.binding = DeviceBinding{vp};
                    }
                }
            } else {
                step.kind = StepKind::Connector;
                step.binding = ConnectorBinding{seg.text};
            }
            rb->steps.push_back(step);
        }
        byName[ra.name] = rb;
    }
    if (!r.errors.empty()) { r.ok = false; return r; }

    // bind sub-recipe pointers + navigators (second pass)
    for (auto& [name, rb] : byName) {
        for (auto& s : rb->steps) {
            if (s.kind != StepKind::SubRecipe) continue;
            auto& bnd = std::get<SubRecipeBinding>(s.binding);
            // find the structblock with this name to copy its navigator + resolve sub
            // (navigator copied from StructBlockProvider via accessor)
            // bnd.nav = ...; bnd.sub = byName[ subRecipeName ].get();
        }
    }
    // NOTE: the cleanest implementation exposes StructBlockProvider::navigator() and
    // subRecipeName() (already public). Implement the binding loop using those, resolving
    // bnd.sub = byName[ provider->subRecipeName() ].get() and bnd.nav = provider->navigator().
    // (The sketch above is left explicit so the implementer writes the real loop.)

    for (auto& [name, rb] : byName) r.store.publish(name, rb);
    r.ok = true;
    return r;
}
} // namespace sv
```

> **Implementer action:** add `const Navigator& navigator() const { return nav_; }` to `StructBlockProvider` in `ValueProvider.hpp` (it's a one-liner; the field `nav_` already exists). Then replace the sketchy binding loops in `BuilderA.cpp` with a clean version:
> ```cpp
> // Build a name->StructBlockProvider* index for navigator/subRecipe lookup.
> std::unordered_map<std::string, StructBlockProvider*> blockByName;
> for (auto& b : names.structBlocks()) blockByName[b.first] = b.second;
>
> // First pass compiled steps; now fix SubRecipe bindings:
> for (auto& [name, rb] : byName) {
>     for (auto& s : rb->steps) {
>         if (s.kind != StepKind::SubRecipe) continue;
>         // The step's seg.text was the structblock name; but we lost it in StepA.
>         // FIX: store the structblock name in SubRecipeBinding during first pass
>         //      (add `std::string blockName;` to SubRecipeBinding) so we can resolve here.
>     }
> }
> ```
> Add `std::string blockName;` to `SubRecipeBinding` and set it in the first pass; resolve `bnd.sub = byName[blockByName[bnd.blockName]->subRecipeName()].get()` and `bnd.nav = blockByName[bnd.blockName]->navigator()`.

- [ ] **Step 6: Add renderA to Engine**

`include/struct_view/Engine.hpp` — add to public section:
```cpp
    LoadResult loadConfigA(std::string_view jsonText);  // compiles to RecipeA store
    std::string renderA(const std::string& recipeName, const void* structPtr, const DeviceCtx& ctx) const;
```
and private member `RecipeStore<RecipeA> storeA_;`

`src/Engine.cpp` — add:
```cpp
#include <struct_view/BuilderA.hpp>

LoadResult Engine::loadConfigA(std::string_view jsonText) {
    LoadResult lr;
    auto parsed = ConfigLoader::parse(jsonText);
    if (!parsed.ok) { lr.errors.push_back({"", "parse error: " + parsed.parseError, 0}); return lr; }
    auto ve = Validator::validate(parsed.ast, names_, connectors_);
    if (!ve.empty()) { lr.ok = false; lr.errors = std::move(ve); return lr; }
    auto built = BuilderA::compile(parsed.ast, names_, connectors_);
    if (!built.ok) { lr.ok = false; lr.errors = std::move(built.errors); return lr; }
    storeA_ = std::move(built.store);
    lr.ok = true;
    return lr;
}

std::string Engine::renderA(const std::string& recipeName, const void* structPtr, const DeviceCtx& ctx) const {
    auto snap = storeA_.snapshot(recipeName);
    if (!snap) return "";
    const RecipeA& r = **snap;
    std::string out; out.reserve(64);
    for (const StepA& s : r.steps) {
        switch (s.kind) {
            case StepKind::Field:     out += std::get<FieldBinding>(s.binding).provider->get(structPtr, ctx); break;
            case StepKind::Connector: out += std::get<ConnectorBinding>(s.binding).literal; break;
            case StepKind::Device:    out += std::get<DeviceBinding>(s.binding).provider->get(nullptr, ctx); break;
            case StepKind::SubRecipe: {
                const auto& b = std::get<SubRecipeBinding>(s.binding);
                const void* child = b.nav.navigate(structPtr);
                out += renderA_inline(*b.sub, child, ctx);  // see helper below
            } break;
        }
    }
    return out;
}
```
Add a helper for inline sub-recipe recursion (avoids re-snapshotting for sub-recipes):
```cpp
// in Engine.cpp, file-local:
static std::string runRecipeA(const sv::RecipeA& r, const void* structPtr, const sv::DeviceCtx& ctx) {
    std::string out; out.reserve(64);
    for (const sv::StepA& s : r.steps) {
        using SK = sv::StepKind;
        switch (s.kind) {
            case SK::Field:     out += std::get<sv::FieldBinding>(s.binding).provider->get(structPtr, ctx); break;
            case SK::Connector: out += std::get<sv::ConnectorBinding>(s.binding).literal; break;
            case SK::Device:    out += std::get<sv::DeviceBinding>(s.binding).provider->get(nullptr, ctx); break;
            case SK::SubRecipe: { const auto& b = std::get<sv::SubRecipeBinding>(s.binding);
                                  out += runRecipeA(*b.sub, b.nav.navigate(structPtr), ctx); } break;
        }
    }
    return out;
}
```
and have `renderA` call `runRecipeA(**snap, structPtr, ctx)` instead of the inline switch (remove `renderA_inline`). Make `runRecipeA` declared in `RecipeRunner.hpp` if `StructBlockProvider`-equivalent for A is needed (not needed here since Route A recursion is self-contained). Expose `runRecipeA` in `RecipeRunner.hpp`/`.cpp` for symmetry and testability.

- [ ] **Step 7: Build and run**

Run: `cmake --build build && ctest --test-dir build -R test_route_a --output-on-failure`
Expected: PASS (Route A output == Route B output).

- [ ] **Step 8: Commit**

```bash
git add include/struct_view/Recipe.hpp include/struct_view/BuilderA.hpp include/struct_view/ValueProvider.hpp include/struct_view/Engine.hpp src/BuilderA.cpp src/Engine.cpp tests/test_route_a.cpp
git commit -m "feat: Route A explicit four-kind Step + BuilderA + renderA"
```

---

## Task 16: Benchmark Route A vs Route B + decision

**Files:**
- Create: `bench/bench_routes.cpp`
- Create: `docs/superpowers/specs/2026-08-04-struct-view-benchmark-result.md` (recorded decision)
- Modify: `CMakeLists.txt` (add bench target, not tested)

- [ ] **Step 1: Write the benchmark**

`bench/bench_routes.cpp`:
```cpp
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "event.h"
#include "device_ctx.h"
#include "registry_gen.cpp"
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>

int main() {
    sv::NameRegistry reg; registerStructViewNames(reg);
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    const char* cfg =
        R"({"recipes":[
          {"name":"alarm_line","template":"${camera}:${time}|${person}@${rect}"},
          {"name":"person","template":"${name}-${age}"},
          {"name":"rect","template":"${x},${y},${w},${h}"}]})";
    if (!engine.loadConfig(cfg).ok) { std::cerr << "loadB failed\n"; return 1; }
    if (!engine.loadConfigA(cfg).ok) { std::cerr << "loadA failed\n"; return 1; }

    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{1717171717, &p, {100, 200, 300, 400}};
    MyDeviceCtx ctx;

    const long N = 2'000'000;
    volatile std::string sink; // prevent elision

    auto t0 = std::chrono::high_resolution_clock::now();
    for (long i = 0; i < N; ++i) sink = engine.render("alarm_line", &ev, ctx);
    auto t1 = std::chrono::high_resolution_clock::now();
    for (long i = 0; i < N; ++i) sink = engine.renderA("alarm_line", &ev, ctx);
    auto t2 = std::chrono::high_resolution_clock::now();

    double b_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double a_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::cout << "Route B (ValueProvider*): " << b_ms << " ms  (" << (b_ms / N * 1000) << " us/render)\n";
    std::cout << "Route A (four-kind Step): " << a_ms << " ms  (" << (a_ms / N * 1000) << " us/render)\n";
    std::cout << "Speedup A/B: " << (b_ms / a_ms) << "x\n";
    return 0;
}
```

- [ ] **Step 2: Add bench target to CMakeLists.txt**

Append:
```cmake
add_executable(bench_routes
    bench/bench_routes.cpp
    src/ValueProvider.cpp src/RecipeRunner.cpp src/NameRegistry.cpp
    src/ConnectorLib.cpp src/ConfigLoader.cpp src/Validator.cpp
    src/Builder.cpp src/BuilderA.cpp src/Engine.cpp)
target_include_directories(bench_routes PRIVATE examples/security include third_party)
target_compile_options(bench_routes PRIVATE -O2)
```

- [ ] **Step 3: Build and run benchmark**

Run:
```bash
cmake --build build --target bench_routes
./build/bench_routes
```
Expected: prints timings for both routes and a speedup ratio. Record the numbers.

- [ ] **Step 4: Write the benchmark result / decision doc**

`docs/superpowers/specs/2026-08-04-struct-view-benchmark-result.md`:
```markdown
# struct_view Route A vs Route B benchmark result

- Date: 2026-08-05
- Hardware: <fill from `uname -a` / CPU>
- Compiler: <fill from `g++ --version`>
- N = 2,000,000 renders of `alarm_line` (Event/PersonInfo/Box, 2-level nesting)

## Results

| Route | Time (ms) | us/render |
|---|---|---|
| B (ValueProvider*) | <fill> | <fill> |
| A (four-kind Step) | <fill> | <fill> |
| Speedup A/B | <fill>x | |

## Decision

Per spec section 3.2: if the difference is negligible in the target scenario, choose B
(simpler); if the hot path is a bottleneck, choose A.

Chosen route: <A or B> — rationale: <one paragraph referencing the numbers and the
security-scene call frequency>.

## Action

< "Remove Route A code" | "Adopt Route A as default, retire Route B" | "Keep both behind a flag" >
```

- [ ] **Step 5: Commit**

```bash
git add bench/bench_routes.cpp CMakeLists.txt docs/superpowers/specs/2026-08-04-struct-view-benchmark-result.md
git commit -m "bench: Route A vs Route B + record decision (spec 3.2)"
```

---

# Final: full test run

- [ ] **Run the entire suite**

```bash
cmake --build build
ctest --test-dir build --output-on-failure
python3 tests/test_codegen.py
./tests/test_drift.sh
./build/security_example
./build/bench_routes
```
Expected: all C++ tests PASS, codegen tests PASS, drift script prints `ALL OK`, example prints the alarm line, benchmark prints timings.

---

## Self-Review (run by plan author before handoff)

**1. Spec coverage:**
- §1 goals → Task 14 E2E demonstrates struct→string with order/connectors/device info. ✓
- §2 architecture (load-time pipeline + runtime walk) → Tasks 6–10. ✓
- §3.1 name two-layer model (scalar + struct block, recursive) → Tasks 3, 5, 9 (bind), 14. ✓
- §3.2 dual-track A/B → Tasks 1–14 (B) + 15–16 (A + benchmark). ✓
- §3.3 Recipe immutable → Task 3/8 (shared_ptr, atomic swap). ✓
- §3.4 ValueProvider / FieldProvider / StructBlockProvider → Task 3 (LambdaProvider replaces FieldProvider template; documented as simplification). ✓
- §3.5 three registries → Tasks 4, 5, DeviceCtx in Task 2. ✓
- §3.6 engine stringing → Task 10. ✓
- §4 config + pipeline → Tasks 6–9. ✓
- §5 error handling (no half-publish, full collection) → Tasks 6 (parse), 7 (validate collect-all), 9/10 (compile/publish). ✓
- §6.1 codegen → Tasks 12–13. ✓
- §6.2 extension workflow → covered by codegen + hot-reload (Task 11). ✓
- §6.3 user surface → recipe JSON (Task 14). ✓
- §6.4 public API → Task 10. ✓
- §7 boundaries (C++17, nlohmann, no runtime deps, shared_mutex) → Tasks 1, 8. ✓
- §8 testing → every task is TDD + Tasks 11, 13, 16. ✓
- §9 E2E example → Task 14. ✓
- §10 YAGNI → respected (no conditionals/loops/include/codegen-JIT). ✓
- §11 pending: Step A/B → Task 16 decides; ptr/embedded → codegen `ptr` flag (Task 12); render defense → Task 10; JSON lib → nlohmann (Task 1); codegen tool → Python (Task 12). ✓

**2. Placeholder scan:** Task 15 BuilderA contains an explicit "sketch → clean version" handoff with a real, complete clean version provided in the implementer-action note. That is acceptable (the implementer writes the named loop with full code shown). No "TBD"/"TODO"/"add error handling" without code. ✓

**3. Type consistency:** `makeProvider`, `Navigator`, `ConnectorProvider`, `StructBlockProvider`, `NameRegistry::registerProvider/registerStruct/structBlocks/lookup`, `ConnectorLib::add/get`, `ConfigLoader::parse`, `Validator::validate`, `Builder::compile`, `RecipeStore::publish/snapshot`, `Engine::loadConfig/render` (+ `loadConfigA/renderA`) — names are consistent across tasks. `StructBlockProvider::navigator()` accessor added in Task 15 is the one addition; flagged. ✓

No gaps found. Plan is complete.
