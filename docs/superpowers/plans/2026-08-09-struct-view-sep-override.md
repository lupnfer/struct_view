# `${name:sep=}` / `${name:isep=}` 分隔符覆盖 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 recipe 里的 `${name:sep=xxx}` / `${name:isep=yyy}` 覆盖 schema 默认分隔符,使分隔符可热加载(不改 schema、不重编译)。

**Architecture:** Segment 加 `sepOverride`/`isepOverride` 字段;分词器解析 `:sep=`/`:isep=`;Builder 用覆盖值(或 schema 默认)构造 provider;标量数组从 codegen 烤死 sep 改为运行时 sep(新 `ScalarArrayProvider` 类);Validator 校验 sep/isep 对非可分类型报错。StructBlockProvider/ArrayStructBlockProvider 已持 sep_ 成员,注入点直接替换即可。

**Tech Stack:** C++17, Python 3(codegen), nlohmann/json + doctest(vendored)。

**Spec:** `docs/superpowers/specs/2026-08-09-struct-view-sep-override-design.md`

---

## Task 1: Segment + 分词器解析 `:sep=` / `:isep=`

**Files:**
- Modify: `include/struct_view/ConfigAst.hpp`
- Modify: `src/ConfigLoader.cpp`
- Test: `tests/test_config_loader.cpp`

- [ ] **Step 1: 写失败测试(追加到 tests/test_config_loader.cpp)**

```cpp
TEST_CASE("ConfigLoader: parses :sep= override") {
    auto result = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r_test","template":"${ids:sep=-}"}]})");
    REQUIRE(result.ok);
    REQUIRE(result.ast.recipes[0].segments.size() == 1);
    const auto& seg = result.ast.recipes[0].segments[0];
    CHECK(seg.isRef);
    CHECK(seg.text == "ids");
    CHECK(seg.sepOverride == "-");
    CHECK(seg.isepOverride.empty());
}

TEST_CASE("ConfigLoader: parses :isep= override") {
    auto result = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r_test","template":"${boxes:isep=,}"}]})");
    REQUIRE(result.ok);
    const auto& seg = result.ast.recipes[0].segments[0];
    CHECK(seg.text == "boxes");
    CHECK(seg.sepOverride.empty());
    CHECK(seg.isepOverride == ",");
}

TEST_CASE("ConfigLoader: parses combined :sep=:isep=") {
    auto result = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r_test","template":"${boxes:sep=|:isep=,}"}]})");
    REQUIRE(result.ok);
    const auto& seg = result.ast.recipes[0].segments[0];
    CHECK(seg.text == "boxes");
    CHECK(seg.sepOverride == "|");
    CHECK(seg.isepOverride == ",");
}

TEST_CASE("ConfigLoader: no override → empty strings") {
    auto result = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r_test","template":"${ids}"}]})");
    REQUIRE(result.ok);
    const auto& seg = result.ast.recipes[0].segments[0];
    CHECK(seg.sepOverride.empty());
    CHECK(seg.isepOverride.empty());
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cd /Users/lpf2026/MyCode/struct_view && cmake --build build 2>&1 | head -10`
Expected: FAIL — Segment 无 sepOverride/isepOverride 成员。

- [ ] **Step 3: 修改 include/struct_view/ConfigAst.hpp —— Segment 加字段**

```cpp
struct Segment {
    bool isRef;
    std::string text;
    std::string sepOverride;  // from :sep=xxx, empty = use schema default
    std::string isepOverride; // from :isep=xxx, empty = use schema default (struct arrays only)
};
```

- [ ] **Step 4: 修改 src/ConfigLoader.cpp —— 分词器解析 `:sep=`/`:isep=`**

替换 `tokenize` 函数里 `${...}` 内容的解析部分。当前是:
```cpp
            std::string name(tpl.substr(i + 2, end - (i + 2)));
            segs.push_back({true, std::move(name)});
```
改为解析 `name:sep=xxx:isep=yyy`(或任意顺序):
```cpp
            std::string content(tpl.substr(i + 2, end - (i + 2)));
            Segment seg{true, "", "", ""};
            // Split on ':' to separate name from params
            size_t colon = content.find(':');
            if (colon == std::string::npos) {
                seg.text = std::move(content);
            } else {
                seg.text = content.substr(0, colon);
                // Parse params: sep=xxx / isep=yyy, separated by ':'
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
                        // Unknown keys silently ignored (forward compat)
                    }
                    if (nextColon == std::string::npos) break;
                    pos = nextColon + 1;
                }
            }
            segs.push_back(std::move(seg));
```

- [ ] **Step 5: 构建并运行 test_config_loader**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_config_loader --output-on-failure`
Expected: test_config_loader PASS(含 4 新 case)。全量会有回归(Builder 等用旧 Segment 聚合初始化)—— 预期。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/ConfigAst.hpp src/ConfigLoader.cpp tests/test_config_loader.cpp
git commit -m "feat(sep): Segment + tokenizer parse :sep= / :isep= overrides"
```

---

## Task 2: ScalarArrayProvider —— 标量数组运行时 sep

**Files:**
- Modify: `include/struct_view/ValueProvider.hpp`
- Modify: `src/ValueProvider.cpp`
- Test: `tests/test_value_provider.cpp`

标量数组当前是 codegen 生成的 `LambdaProvider`(sep 烤死在 lambda 里)。改为新的 `ScalarArrayProvider` 类:持「单元素格式器 lambda + count + sep」,sep 运行时可注入。

- [ ] **Step 1: 写失败测试(追加到 tests/test_value_provider.cpp)**

```cpp
namespace {
struct SaEv { int ids[4]; };
}

TEST_CASE("ScalarArrayProvider: joins elements with runtime sep") {
    // 单元素格式器:取 ids[i] → snprintf "%d"
    sv::ScalarArrayProvider sap(
        [](const void* p, std::size_t i) -> std::string {
            const SaEv* s = static_cast<const SaEv*>(p);
            char buf[16]; std::snprintf(buf, sizeof(buf), "%d", s->ids[i]); return buf;
        }, 4, "-");
    sv::DeviceCtx dummy;
    SaEv e{{11, 22, 33, 44}};
    CHECK(sap.get(&e, dummy) == "11-22-33-44");
}

TEST_CASE("ScalarArrayProvider: different sep at construction") {
    sv::ScalarArrayProvider sap(
        [](const void* p, std::size_t i) -> std::string {
            const SaEv* s = static_cast<const SaEv*>(p);
            char buf[16]; std::snprintf(buf, sizeof(buf), "%d", s->ids[i]); return buf;
        }, 4, "*");
    sv::DeviceCtx dummy;
    SaEv e{{11, 22, 33, 44}};
    CHECK(sap.get(&e, dummy) == "11*22*33*44");
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | head -5`
Expected: FAIL — `ScalarArrayProvider` 未定义。

- [ ] **Step 3: 修改 include/struct_view/ValueProvider.hpp —— 加 ScalarArrayProvider**

在 `StructBlockProvider` 之前(或之后)加:
```cpp
// Scalar array: iterates count_ elements, formats each with elemFn_, joins with sep_.
// sep_ is runtime-configurable (recipe can override via ${name:sep=xxx}, spec §5).
// elemFn_ is codegen-generated (snprintf per element type); count_ is from schema.
class ScalarArrayProvider : public ValueProvider {
    std::function<std::string(const void*, std::size_t)> elemFn_;
    std::size_t count_;
    std::string sep_;
public:
    ScalarArrayProvider(std::function<std::string(const void*, std::size_t)> elemFn,
                        std::size_t count, std::string sep)
        : elemFn_(std::move(elemFn)), count_(count), sep_(std::move(sep)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};
```
(需要 `#include <functional>` —— 检查 ValueProvider.hpp 是否已有,若无则加。)

- [ ] **Step 4: 修改 src/ValueProvider.cpp —— 加 ScalarArrayProvider::get**

```cpp
std::string ScalarArrayProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += sep_;
        out += elemFn_(structPtr, i);
    }
    return out;
}
```

- [ ] **Step 5: 构建并运行 test_value_provider**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_value_provider --output-on-failure`
Expected: test_value_provider PASS(含 2 新 case)。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/ValueProvider.hpp src/ValueProvider.cpp tests/test_value_provider.cpp
git commit -m "feat(sep): ScalarArrayProvider with runtime sep (not baked in codegen)"
```

---

## Task 3: codegen —— 标量数组生成 ScalarArrayProvider

**Files:**
- Modify: `tools/gen_registry.py`
- Test: `tests/test_codegen.py`

标量数组不再生成 `LambdaProvider`(烤死 sep),改为生成 `ScalarArrayProvider`(sep 不烤死,由 Builder 注入)。

- [ ] **Step 1: 写失败测试(改 tests/test_codegen.py 的 test_emits_scalar_array_loop_and_static_assert)**

```python
def test_emits_scalar_array_provider_with_runtime_sep():
    out = run_gen(json.dumps(ARRAY_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerProvider("feat_ids"' in out
    assert 'ScalarArrayProvider' in out
    assert 'std::extent_v<decltype(s->feature_ids)> >= 8' in out
    assert '(int)s->feature_ids[i]' in out
    # sep NOT baked into the provider — it's a runtime arg
    assert 'if (i) out += "-"' not in out   # old baked-in sep gone
```

- [ ] **Step 2: 运行验证失败**

Run: `python3 tests/test_codegen.py 2>&1 | tail -5`
Expected: FAIL — codegen 还生成旧 LambdaProvider。

- [ ] **Step 3: 修改 tools/gen_registry.py —— field_array_line 改为生成 ScalarArrayProvider**

```python
def field_array_line(struct_name, member):
    field = member["field"]
    as_name = member.get("as", field)
    typ = member["type"]
    fmt = member["fmt"]
    arr = member["array"]
    count = arr["count"]
    sep = arr["sep"]
    cast = TYPE_CAST[typ]
    sep_lit = _esc(sep)
    desc = _esc(member.get("desc", ""))
    return f'''    reg.registerProvider("{as_name}", std::make_unique<sv::ScalarArrayProvider>(
        [](const void* p, std::size_t i) -> std::string {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            static_assert(std::extent_v<decltype(s->{field})> >= {count},
                          "struct_view: {field} length drift (schema count={count})");
            char buf[256];
            std::snprintf(buf, sizeof(buf), "{fmt}", {cast}s->{field}[i]);
            return std::string(buf);
        }}, {count}, "{sep_lit}"), "{desc}");\n'''
```

> 注意:`registerProvider` 的第二个参数是 `unique_ptr<ValueProvider>`。`ScalarArrayProvider` is-a `ValueProvider`,`make_unique<sv::ScalarArrayProvider>(...)` 隐式转换为 `unique_ptr<ValueProvider>`。需确认 codegen 生成的代码 include 了 `<memory>`(HEADER 里有 `#include <struct_view/ValueProvider.hpp>` → 间接 include `<memory>`)。

- [ ] **Step 4: 运行 Python 测试**

Run: `python3 tests/test_codegen.py 2>&1 | tail -10`
Expected: 全部 PASS(含新 case)。

- [ ] **Step 5: 编译验证生成代码**

```bash
cd /Users/lpf2026/MyCode/struct_view
python3 tools/gen_registry.py <(echo '{"deviceCtxType":"sv::DeviceCtx","structs":[{"name":"Ev","members":[{"field":"ids","type":"int","fmt":"%d","array":{"count":4,"sep":"-"}}]}]}') /tmp/sa.cpp
cat > /tmp/sa_main.cpp <<'EOF'
#include <cstdint>
#include <cstdio>
struct Ev { int ids[4]; };
#include <struct_view/NameRegistry.hpp>
#include "/tmp/sa.cpp"
int main() {
    sv::NameRegistry r; registerStructViewNames(r);
    Ev e{{11,22,33,44}};
    const sv::ValueProvider* p = r.lookup("ids");
    sv::DeviceCtx dummy;
    std::printf("ids=%s\n", p->get(&e, dummy).c_str());
    return (p->get(&e, dummy) == "11-22-33-44") ? 0 : 1;
}
EOF
g++ -std=c++17 -Iinclude -Ithird_party /tmp/sa_main.cpp src/*.cpp -o /tmp/sa_exe 2>&1 | tail -5
/tmp/sa_exe && echo "COMPILE+RUN OK"
```
Expected: `ids=11-22-33-44`,`COMPILE+RUN OK`。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add tools/gen_registry.py tests/test_codegen.py
git commit -m "feat(sep): codegen scalar array emits ScalarArrayProvider (runtime sep)"
```

---

## Task 4: Builder —— sep/isep 覆盖注入

**Files:**
- Modify: `src/Builder.cpp`
- Test: `tests/test_builder.cpp`

Builder 编译 `${name}` 引用时,从 Segment 取 `sepOverride`/`isepOverride`,用覆盖值(或 schema 默认)构造 provider。

- [ ] **Step 1: 写失败测试(追加到 tests/test_builder.cpp)**

```cpp
TEST_CASE("Builder: sep override on scalar array") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r_test","template":"${ids:sep=*}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerProvider("ids", std::make_unique<sv::ScalarArrayProvider>(
        [](const void* p, std::size_t i) -> std::string {
            static int data[] = {11,22,33,0,0,0,0,0};
            (void)p;
            char b[16]; std::snprintf(b,sizeof(b),"%d",data[i]); return b;
        }, 8, "-"));  // default sep "-", but recipe overrides to "*"
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("r_test");
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(nullptr, dummy);
    CHECK(out == "11*22*33*0*0*0*0*0");
}
```

> 注:这个测试注册了一个 ScalarArrayProvider(默认 sep="-"),但 recipe 用 `${ids:sep=*}` 覆盖为 "*"。Builder 需要在编译时用覆盖值重新构造 provider(sep="*")。但 ScalarArrayProvider 是 NameRegistry 拥有的(不可变)—— Builder 不能改它的 sep。所以 Builder 需要**创建一个新的 ScalarArrayProvider 副本**(用覆盖 sep),而不是直接用 NameRegistry 的那个。这和 StructBlockProvider 的模式一致(Builder 创建配方内私有 provider)。

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | tail -3 && ctest --test-dir build -R test_builder --output-on-failure 2>&1 | tail -5`
Expected: FAIL — Builder 不读 sepOverride(用默认 sep,输出 "11-22-33-0-0-0-0-0" 而非 "11*22*33*0*0*0*0*0")。

- [ ] **Step 3: 修改 src/Builder.cpp —— Pass 1 读 sepOverride/isepOverride**

Pass 1 编译 `${name}` 引用时,需要把 Segment 的 sepOverride/isepOverride 传给 Pass 2 的 pending 条目。改 PendingBind 加两个字段:

```cpp
    struct PendingBind {
        std::shared_ptr<RecipeB> rb;
        std::size_t stepIdx;
        std::string name;
        std::string sepOverride;   // from :sep=, empty = use default
        std::string isepOverride;  // from :isep=, empty = use default (struct arrays only)
    };
```

Pass 1 的 `${name}` 分派里,把 `seg.sepOverride`/`seg.isepOverride` 存进 pending/pendingArray:

```cpp
                } else if (names.isStructBlock(seg.text)) {
                    pending.push_back({rb, rb->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
                } else if (names.isStructArray(seg.text)) {
                    pendingArray.push_back({rb, rb->steps.size(), seg.text, seg.sepOverride, seg.isepOverride});
```

标量数组(ScalarArrayProvider)的覆盖:标量数组是 `names.lookup` 命中的 provider(NameRegistry 拥有,sep 烤死)。若 seg.sepOverride 非空,Builder 需创建一个新的 ScalarArrayProvider(用覆盖 sep)。但这需要知道原 provider 的 elemFn + count —— 当前 NameRegistry 只存 `ValueProvider*`(不暴露 elemFn/count)。**方案**:改 codegen,标量数组不再用 `registerProvider`(把 provider 存进 entries_),而是用一个新的 `registerScalarArray` 方法,存进一个独立的 scalarArrayDecls_ 列表(像 structDecls_ 一样),Builder 从那里拿 elemFn+count+defaultSep,创建配方内私有 provider(用覆盖或默认 sep)。

> 这是比 StructBlockProvider 更大的改动 —— 需要新的 `ScalarArrayDecl` + `registerScalarArray`。但模式与 StructBlockDecl 同构。

**替代方案(更简单)**:不改 NameRegistry 存储,而是让 codegen 生成的 ScalarArrayProvider 把 sep 存为可改的成员(加 `setSep()` 方法),Builder 调 `setSep()` 覆盖。但这破坏 immutability(RCU)—— 不行。

**采用方案**:加 `ScalarArrayDecl` + `registerScalarArray`,模式与 StructBlockDecl 同构。具体:

**3a. NameRegistry 加 ScalarArrayDecl + registerScalarArray:**
```cpp
// include/struct_view/NameRegistry.hpp
struct ScalarArrayDecl {
    std::function<std::string(const void*, std::size_t)> elemFn;
    std::size_t count;
    std::string sep;     // default sep (fallback)
    std::string desc;
};
// NameRegistry 加:
    std::vector<std::pair<std::string, ScalarArrayDecl>> scalarArrayDecls_;
public:
    void registerScalarArray(std::string name,
                             std::function<std::string(const void*, std::size_t)> elemFn,
                             std::size_t count, std::string sep, std::string desc = "");
    const ScalarArrayDecl* findScalarArrayDecl(const std::string& name) const;
```

**3b. Builder Pass 1:** 标量数组不再走 `names.lookup`(它不在 entries_ 了),改为 `names.findScalarArrayDecl(seg.text)` → pendingScalar(新 pending 列表)。

**3c. Builder Pass 2c(新):** 创建配方内私有 ScalarArrayProvider(用覆盖或默认 sep):
```cpp
    for (auto& ps : pendingScalar) {
        const ScalarArrayDecl* decl = names.findScalarArrayDecl(ps.name);
        if (!decl) { r.errors.push_back({ps.rb->name, "unknown scalar array: " + ps.name, 0}); continue; }
        std::string sep = ps.sepOverride.empty() ? decl->sep : ps.sepOverride;
        auto sap = std::make_unique<ScalarArrayProvider>(decl->elemFn, decl->count, sep);
        ps.rb->steps[ps.stepIdx].provider = sap.get();
        ps.rb->ownedProviders.push_back(std::move(sap));
    }
```

**3d. StructBlockProvider/ArrayStructBlockProvider 的覆盖注入(Pass 2/2b):**
```cpp
    // Pass 2: struct block — sep override
    std::string sep = p.sepOverride.empty() ? decl->sep : p.sepOverride;
    auto sbp = std::make_unique<StructBlockProvider>(decl->nav, std::move(fields), sep);

    // Pass 2b: struct array — sep (element-internal) + arraySep (element-between) override
    std::string isep = pa.isepOverride.empty() ? decl->sep : pa.isepOverride;      // element-internal
    std::string asep = pa.sepOverride.empty() ? decl->arraySep : pa.sepOverride;   // element-between
    auto asbp = std::make_unique<ArrayStructBlockProvider>(decl->nav, std::move(fields), decl->count, isep, asep);
```

- [ ] **Step 4: 构建 + 运行 test_builder**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_builder --output-on-failure`
Expected: test_builder 新 case PASS。

- [ ] **Step 5: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/NameRegistry.hpp include/struct_view/ValueProvider.hpp src/NameRegistry.cpp src/Builder.cpp tests/test_builder.cpp
git commit -m "feat(sep): Builder injects sep/isep overrides (scalar array decl + struct block/array)"
```

---

## Task 5: BuilderA —— Route A sep/isep 覆盖注入

**Files:**
- Modify: `src/BuilderA.cpp`

同 Builder,但 Route A 的 SubRecipeBinding/FieldBinding 注入。

- [ ] **Step 1: 修改 src/BuilderA.cpp**

PendingBind 加 sepOverride/isepOverride(同 Builder)。Pass 1 分派标量数组改为 `names.findScalarArrayDecl`。Pass 2/2b/2c 注入覆盖值:

```cpp
    // Pass 2: struct block — SubRecipeBinding with sep override
    std::string sep = p.sepOverride.empty() ? decl->sep : p.sepOverride;
    step.binding = SubRecipeBinding{decl->nav, std::move(fields), sep};

    // Pass 2b: struct array — ArrayStructBlockProviderA with isep/asep override
    std::string isep = pa.isepOverride.empty() ? decl->sep : pa.isepOverride;
    std::string asep = pa.sepOverride.empty() ? decl->arraySep : pa.sepOverride;
    auto asbp = std::make_unique<ArrayStructBlockProviderA>(decl->nav, std::move(fields), decl->count, isep, asep);

    // Pass 2c: scalar array — ScalarArrayProvider with sep override
    std::string sep = ps.sepOverride.empty() ? decl->sep : ps.sepOverride;
    auto sap = std::make_unique<ScalarArrayProvider>(decl->elemFn, decl->count, sep);
```

- [ ] **Step 2: 构建**

Run: `cmake --build build 2>&1 | tail -5`
Expected: BuilderA 编译通过。

- [ ] **Step 3: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add src/BuilderA.cpp
git commit -m "feat(sep): BuilderA Route A sep/isep override injection"
```

---

## Task 6: Validator —— sep/isep 合法性校验

**Files:**
- Modify: `src/Validator.cpp`
- Test: `tests/test_validator.cpp`

- [ ] **Step 1: 写失败测试(追加到 tests/test_validator.cpp)**

```cpp
TEST_CASE("Validator: :sep= on scalar field is error") {
    auto ast = parse(R"({"recipes":[{"name":"r_test","template":"${x:sep=-}"}]})");
    sv::NameRegistry reg;
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool found = false;
    for (auto& e : errs) if (e.message.find("sep") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("Validator: :isep= on non-struct-array is error") {
    auto ast = parse(R"({"recipes":[{"name":"r_test","template":"${ids:isep=,}"}]})");
    sv::NameRegistry reg;
    reg.registerScalarArray("ids", [](const void*, std::size_t){return std::string("1");}, 4, "-", "");
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool found = false;
    for (auto& e : errs) if (e.message.find("isep") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("Validator: :sep= on scalar array is valid") {
    auto ast = parse(R"({"recipes":[{"name":"r_test","template":"${ids:sep=*}"}]})");
    sv::NameRegistry reg;
    reg.registerScalarArray("ids", [](const void*, std::size_t){return std::string("1");}, 4, "-", "");
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | tail -3 && ctest --test-dir build -R test_validator --output-on-failure 2>&1 | tail -5`
Expected: FAIL — Validator 不校验 sep/isep 合法性。

- [ ] **Step 3: 修改 src/Validator.cpp**

在 check 1(引用解析)之后加 sep/isep 合法性校验:
```cpp
    // 1b. :sep=/:isep= overrides: validate they're on separable names.
    for (const auto& r : ast.recipes) {
        for (const auto& seg : r.segments) {
            if (!seg.isRef) continue;
            bool isScalarField = names.lookup(seg.text) != nullptr;   // scalar field/device
            if (!seg.sepOverride.empty() && isScalarField) {
                errs.push_back({r.name, "sep override on non-separable name: " + seg.text, 0});
            }
            if (!seg.isepOverride.empty() && !names.isStructArray(seg.text)) {
                errs.push_back({r.name, "isep override on non-struct-array name: " + seg.text, 0});
            }
        }
    }
```
> 注意:标量数组现在在 `scalarArrayDecls_`(不在 entries_),`names.lookup` 对标量数组返回 nullptr —— 所以 `isScalarField` 对标量数组为 false(正确,标量数组可接受 :sep=)。但这也意味着 check 1 的引用解析要加标量数组的判断:`names.findScalarArrayDecl(seg.text)`。

- [ ] **Step 4: 构建 + 运行 test_validator**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_validator --output-on-failure`
Expected: test_validator 新 case PASS。

- [ ] **Step 5: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add src/Validator.cpp tests/test_validator.cpp
git commit -m "feat(sep): Validator checks :sep=/:isep= on separable names only"
```

---

## Task 7: codegen —— 标量数组用 registerScalarArray

**Files:**
- Modify: `tools/gen_registry.py`
- Test: `tests/test_codegen.py`

标量数组不再用 `registerProvider`(把 ScalarArrayProvider 存进 entries_),改为 `registerScalarArray`(存进 scalarArrayDecls_,Builder 创建配方内私有 provider)。

- [ ] **Step 1: 改 field_array_line —— 生成 registerScalarArray**

```python
def field_array_line(struct_name, member):
    field = member["field"]
    as_name = member.get("as", field)
    typ = member["type"]
    fmt = member["fmt"]
    arr = member["array"]
    count = arr["count"]
    sep = arr["sep"]
    cast = TYPE_CAST[typ]
    sep_lit = _esc(sep)
    desc = _esc(member.get("desc", ""))
    return f'''    reg.registerScalarArray("{as_name}",
        [](const void* p, std::size_t i) -> std::string {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            static_assert(std::extent_v<decltype(s->{field})> >= {count},
                          "struct_view: {field} length drift (schema count={count})");
            char buf[256];
            std::snprintf(buf, sizeof(buf), "{fmt}", {cast}s->{field}[i]);
            return std::string(buf);
        }}, {count}, "{sep_lit}", "{desc}");\n'''
```

- [ ] **Step 2: 改 test_codegen.py 的 test_emits_scalar_array_provider_with_runtime_sep**

```python
def test_emits_scalar_array_via_register_scalar_array():
    out = run_gen(json.dumps(ARRAY_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerScalarArray("feat_ids"' in out
    assert 'std::extent_v<decltype(s->feature_ids)> >= 8' in out
    assert '(int)s->feature_ids[i]' in out
```

- [ ] **Step 3: 运行 + 编译验证**

Run: `python3 tests/test_codegen.py 2>&1 | tail -5`
Expected: PASS。

- [ ] **Step 4: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add tools/gen_registry.py tests/test_codegen.py
git commit -m "feat(sep): codegen scalar array emits registerScalarArray (not registerProvider)"
```

---

## Task 8: 全量迁移 + 端到端 + TSan

**Files:**
- Modify: `examples/security/*`(schema 不变,recipes 用覆盖语法)
- Modify: `tests/test_example_e2e.cpp` / `test_engine.cpp` / `test_hot_reload.cpp` / `test_route_a.cpp`(迁移)
- Regenerate: `examples/security/registry_gen.cpp`

- [ ] **Step 1: 全量构建 + 修编译错误**

Run: `cmake --build build 2>&1 | grep "error:" | head -20`
逐个修复(主要是测试里标量数组的注册从 `registerProvider` 改为 `registerScalarArray`,以及 ScalarArrayProvider 构造方式变化)。

- [ ] **Step 2: 端到端示例 —— recipe 用覆盖语法**

`examples/security/recipes.json` 用 `${feat_ids:sep=-}` / `${boxes:sep=|:isep=,}` 等(或保持 `${feat_ids}` 用默认)。确保输出与预期一致。

- [ ] **Step 3: 全量验证**

```bash
cmake --build build 2>&1 | tail -3
ctest --test-dir build --output-on-failure 2>&1 | tail -4
python3 tests/test_codegen.py 2>&1 | tail -5
./tests/test_drift.sh 2>&1 | tail -3
./build/security_example
cmake --build build_tsan -q 2>&1 | tail -2
ctest --test-dir build_tsan --output-on-failure 2>&1 | tail -4
./build_tsan/test_hot_reload 2>&1 | grep -c "ThreadSanitizer: data race"   # 0
```

- [ ] **Step 4: 提交 + 推送**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add -A
git commit -m "feat(sep): full migration + e2e + TSan (sep/isep override)"
```

---

## Self-Review

**1. Spec coverage:** §2 语法 → Task 1(分词器)。§3 schema 默认 → 不变(已有)。§4 Segment → Task 1。§5 Builder → Task 4/5。§6 Validator → Task 6。§7 codegen → Task 3/7。§8 影响范围 → 全覆盖。✓

**2. Placeholder scan:** 无 TBD/TODO。Task 4 的 ScalarArrayDecl 方案完整给出。✓

**3. Type consistency:** `ScalarArrayProvider(elemFn, count, sep)` —— Task 2/4/5/7 一致。`registerScalarArray(name, elemFn, count, sep, desc)` —— Task 4/7 一致。`sepOverride`/`isepOverride` —— Task 1/4/5/6 一致。✓

无 gap。Plan 完整。
