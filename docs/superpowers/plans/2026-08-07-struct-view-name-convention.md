# struct_view 命名约定 + 块展开规则 + desc Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 struct_view 加三类 name 的命名区分(配方强制 `r_` 前缀)、desc 描述字段(给 client 显示)、以及把结构体块展开规则(sep+fields)从 recipe 收进 schema(去掉冗余子配方),同时保留用户组合子配方。

**Architecture:** schema 的 `block` 加 `sep`+`fields`、去掉 `subRecipe`;StructBlockProvider 从持子配方改为持字段访问器列表+sep;Validator 加 `r_` 前缀校验 + block fields 存在性 + 用户子配方循环检测(去掉 subRecipe 检查);codegen/registry/Engine 全链路带 desc;recipes.json 配方名加 `r_` 前缀、删结构体块子配方、保留用户组合子配方。

**Tech Stack:** C++17, Python 3(codegen), nlohmann/json + doctest(vendored)。

**Spec:** `docs/superpowers/specs/2026-08-07-struct-view-name-convention-design.md`

---

## File Structure

```
struct_view/
├── include/struct_view/
│   ├── NameRegistry.hpp      # StructBlockDecl/StructArrayDecl 改 fields+sep+desc;加 descs_+describe()
│   ├── ValueProvider.hpp     # StructBlockProvider/Array* 改持字段列表+sep;去 shared_ptr<Recipe> sub_
│   ├── Recipe.hpp            # SubRecipeBinding 改字段列表+sep(Route A)
│   ├── RecipeRunner.hpp      # 不变(签名同)
│   ├── ConfigAst.hpp         # RecipeAst 加 desc
│   ├── Validator.hpp         # 不变(签名同)
│   ├── Builder.hpp           # 不变(签名同)
│   ├── BuilderA.hpp          # 不变(签名同)
│   ├── Engine.hpp            # 加 recipeDescs_ + describeRecipe()
│   ├── ConnectorLib.hpp      # 不变
│   ├── DeviceCtx.hpp         # 不变
│   └── Errors.hpp            # 不变
├── src/                       # 一头一 cpp,对应改动
│   ├── NameRegistry.cpp      # register* 新签名;descs_ 存储;describe()
│   ├── ValueProvider.cpp     # StructBlockProvider/Array*::get 改字段遍历
│   ├── RecipeRunner.cpp      # runRecipeA SubRecipe case 改字段遍历
│   ├── ConfigLoader.cpp      # 解析 recipe desc
│   ├── Validator.cpp         # r_ 前缀 + block fields 存在性 + 用户子配方循环;去 subRecipe 检查
│   ├── Builder.cpp           # 块编译改字段列表;去 Pass 2 子配方绑定
│   ├── BuilderA.cpp          # 同 Builder(Route A)
│   ├── Engine.cpp            # loadConfig 抽 desc;describeRecipe()
│   └── ConnectorLib.cpp      # 不变
├── tools/gen_registry.py      # block 生成 registerStruct(fields,sep,desc);field/device 带 desc
├── examples/security/         # schema 加 sep/fields/desc 去 subRecipe;recipes 删子配方加 r_;main/test 迁移
└── tests/                     # 全部迁移到新签名 + r_ 前缀 + desc 断言
```

**责任边界:** desc 存储 — 字段/块/设备 desc 在 NameRegistry(`descs_` + `describe()`);配方 desc 在 Engine(`recipeDescs_` + `describeRecipe()`)。块展开规则 — schema 的 `block` 声明 sep+fields;StructBlockProvider 持字段 provider 列表(Builder 组装)。用户子配方 — 仍走 RecipeB 机制(不变)。

---

## Task 1: NameRegistry —— StructBlockDecl/StructArrayDecl 改 fields+sep+desc + descs_/describe()

**Files:**
- Modify: `include/struct_view/NameRegistry.hpp`
- Modify: `src/NameRegistry.cpp`
- Test: `tests/test_name_registry.cpp`

- [ ] **Step 1: 写失败测试(追加到 tests/test_name_registry.cpp 末尾)**

```cpp
TEST_CASE("NameRegistry: registerStruct stores fields+sep+desc (no subRecipe)") {
    sv::NameRegistry reg;
    reg.registerStruct("person",
        sv::Navigator([](const void*) -> const void* { return nullptr; }),
        {"name", "age"}, "-", "人体信息");
    auto decls = reg.structDecls();
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].first == "person");
    CHECK(decls[0].second.fieldNames == std::vector<std::string>{"name", "age"});
    CHECK(decls[0].second.sep == "-");
    CHECK(decls[0].second.desc == "人体信息");
}

TEST_CASE("NameRegistry: registerProvider stores desc; describe() returns it") {
    sv::NameRegistry reg;
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}), "横坐标");
    CHECK(reg.describe("x") == "横坐标");
    CHECK(reg.describe("nope").empty());   // unknown name → empty
}

TEST_CASE("NameRegistry: registerProvider desc defaults to empty (backward compat)") {
    sv::NameRegistry reg;
    reg.registerProvider("y", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("2");}));
    CHECK(reg.describe("y").empty());
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cd /Users/lpf2026/MyCode/struct_view && cmake --build build 2>&1 | head -10`
Expected: FAIL — `registerStruct` 新签名/`StructBlockDecl` 新成员/`describe` 未定义。

- [ ] **Step 3: 修改 include/struct_view/NameRegistry.hpp**

**3a. 改 StructBlockDecl**(去掉 subRecipeName,加 fieldNames/sep/desc):
```cpp
struct StructBlockDecl {
    Navigator nav;
    std::vector<std::string> fieldNames;   // 块内字段展开顺序(取代 subRecipeName)
    std::string sep;                        // 块内字段间分隔符
    std::string desc;                       // 可选描述(给 client 显示)
};
```

**3b. 改 StructArrayDecl**(去掉 subRecipeName,加 fieldNames/arraySep/desc):
```cpp
struct StructArrayDecl {
    IndexedNavigator nav;
    std::vector<std::string> fieldNames;   // 每个元素的字段展开顺序
    std::size_t count;
    std::string sep;                        // 元素内字段间分隔符
    std::string arraySep;                   // 元素间分隔符
    std::string desc;
};
```

**3c. 加 descs_ 成员 + describe()**(在 NameRegistry 类 private 区加):
```cpp
    std::unordered_map<std::string, std::string> descs_;   // name → desc(字段/块/设备)
```
public 区加:
```cpp
    const std::string& describe(const std::string& name) const;  // 返回 desc,无则空串
```

**3d. 改 register 方法签名**(public 区):
```cpp
    void registerProvider(std::string name, std::unique_ptr<ValueProvider> vp, std::string desc = "");
    void registerStruct(std::string name, Navigator nav,
                        std::vector<std::string> fieldNames, std::string sep, std::string desc = "");
    void registerStructArray(std::string name, IndexedNavigator nav,
                             std::vector<std::string> fieldNames, std::size_t count,
                             std::string sep, std::string arraySep, std::string desc = "");
```
(去掉旧的 `std::string subRecipeName` 参数;registerProvider 加 `desc = ""`。)

- [ ] **Step 4: 修改 src/NameRegistry.cpp**

```cpp
void NameRegistry::registerProvider(std::string name, std::unique_ptr<ValueProvider> vp, std::string desc) {
    if (!desc.empty()) descs_[name] = desc;          // 仅存非空 desc
    entries_[std::move(name)] = std::move(vp);
}
void NameRegistry::registerStruct(std::string name, Navigator nav,
                                  std::vector<std::string> fieldNames, std::string sep, std::string desc) {
    StructBlockDecl decl{nav, std::move(fieldNames), std::move(sep), std::move(desc)};
    if (!decl.desc.empty()) descs_[name] = decl.desc;
    structDecls_.emplace_back(std::move(name), std::move(decl));
}
void NameRegistry::registerStructArray(std::string name, IndexedNavigator nav,
                                       std::vector<std::string> fieldNames, std::size_t count,
                                       std::string sep, std::string arraySep, std::string desc) {
    StructArrayDecl decl{nav, std::move(fieldNames), count, std::move(sep), std::move(arraySep), std::move(desc)};
    if (!decl.desc.empty()) descs_[name] = decl.desc;
    structArrayDecls_.emplace_back(std::move(name), std::move(decl));
}
const std::string& NameRegistry::describe(const std::string& name) const {
    auto it = descs_.find(name);
    static const std::string EMPTY;
    return it == descs_.end() ? EMPTY : it->second;
}
```
(注意:registerStruct/Array 里 `desc` 被 move 进 decl 后不能再用,所以 `descs_[name] = decl.desc` 要在 move 之前,或先存 desc 再 move。修正:把 `if (!desc.empty()) descs_[name] = desc;` 放在构造 decl **之前**(desc 还没被 move)。)

修正后的 registerStruct:
```cpp
void NameRegistry::registerStruct(std::string name, Navigator nav,
                                  std::vector<std::string> fieldNames, std::string sep, std::string desc) {
    if (!desc.empty()) descs_[name] = desc;          // 先存 desc(还没 move)
    StructBlockDecl decl{nav, std::move(fieldNames), std::move(sep), std::move(desc)};
    structDecls_.emplace_back(std::move(name), std::move(decl));
}
```
registerStructArray 同理(`if (!desc.empty()) descs_[name] = desc;` 在构造 decl 前)。

- [ ] **Step 5: 构建并运行**

Run: `cmake --build build 2>&1 | tail -10 && ctest --test-dir build -R test_name_registry --output-on-failure`
Expected: test_name_registry 的新 case PASS。但全量会有回归(registerStruct 旧签名的调用方还没改)—— 这是预期的,后续 task 修。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/NameRegistry.hpp src/NameRegistry.cpp tests/test_name_registry.cpp
git commit -m "feat(name): NameRegistry StructBlock/ArrayDecl use fields+sep+desc; add describe()"
```
CRITICAL: COMMIT before reporting. Run `git rev-parse HEAD`.

---

## Task 2: StructBlockProvider/Array* 改持字段列表+sep(去子配方)

**Files:**
- Modify: `include/struct_view/ValueProvider.hpp`
- Modify: `src/ValueProvider.cpp`
- Test: `tests/test_value_provider.cpp`

- [ ] **Step 1: 写失败测试(替换 tests/test_value_provider.cpp 里的 StructBlockProvider case)**

把现有的 `ArrayStructBlockProvider` 相关 case 保留,但 `StructBlockProvider` 的 case 改为字段列表版。先看现有 test_value_provider.cpp 有没有 StructBlockProvider 的直接单测 —— 如果没有,追加:

```cpp
namespace {
struct SbEvent { int a, b; };
}

TEST_CASE("StructBlockProvider: field list + sep (no sub-recipe)") {
    // 两个字段 provider,绑 SbEvent 的 a/b
    auto ap = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const SbEvent* e = static_cast<const SbEvent*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", e->a); return buf;
    });
    auto bp = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const SbEvent* e = static_cast<const SbEvent*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", e->b); return buf;
    });
    sv::Navigator nav([](const void* p) -> const void* { return p; });  // toy: 同一结构体
    sv::StructBlockProvider sbp(nav, {ap.get(), bp.get()}, "-");
    sv::DeviceCtx dummy;
    SbEvent e{11, 22};
    CHECK(sbp.get(&e, dummy) == "11-22");
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | head -10`
Expected: FAIL — StructBlockProvider 构造签名不匹配(旧签名是 `(Navigator, shared_ptr<const RecipeB>)`)。

- [ ] **Step 3: 修改 include/struct_view/ValueProvider.hpp —— StructBlockProvider**

```cpp
// A whole key struct block: navigate to child, then join its fields with sep.
// No sub-recipe — the field order + sep live in the schema (spec §4 of name-convention spec).
// fieldProviders_ are raw ptrs into NameRegistry-owned field providers (longer-lived, read-only).
class StructBlockProvider : public ValueProvider {
    Navigator nav_;
    std::vector<const ValueProvider*> fieldProviders_;   // 子结构体各字段访问器(按 fields 顺序)
    std::string sep_;
public:
    StructBlockProvider(Navigator nav, std::vector<const ValueProvider*> fields, std::string sep)
        : nav_(std::move(nav)), fieldProviders_(std::move(fields)), sep_(std::move(sep)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};
```

- [ ] **Step 4: 修改 src/ValueProvider.cpp —— StructBlockProvider::get**

```cpp
std::string StructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    if (fieldProviders_.empty()) return {};            // defensive
    const void* child = nav_.navigate(structPtr);
    std::string out;
    for (std::size_t i = 0; i < fieldProviders_.size(); ++i) {
        if (i) out += sep_;                             // join 语义
        out += fieldProviders_[i]->get(child, ctx);
    }
    return out;
}
```

- [ ] **Step 5: 同样改 ArrayStructBlockProvider 和 ArrayStructBlockProviderA**

这两个当前持 `shared_ptr<const RecipeB/A> sub_` + 调 runRecipeB/A。改为持字段列表 + sep(元素内)+ arraySep(元素间)+ count。include/struct_view/ValueProvider.hpp:

```cpp
class ArrayStructBlockProvider : public ValueProvider {
    IndexedNavigator nav_;
    std::vector<const ValueProvider*> fieldProviders_;   // 每个元素的字段(按 fields 顺序)
    std::size_t count_;
    std::string sep_;        // 元素内字段间分隔符
    std::string arraySep_;   // 元素间分隔符
public:
    ArrayStructBlockProvider(IndexedNavigator nav, std::vector<const ValueProvider*> fields,
                             std::size_t count, std::string sep, std::string arraySep)
        : nav_(std::move(nav)), fieldProviders_(std::move(fields)),
          count_(count), sep_(std::move(sep)), arraySep_(std::move(arraySep)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};
// ArrayStructBlockProviderA 同构(只是给 Route A 用,签名相同)
class ArrayStructBlockProviderA : public ValueProvider {
    IndexedNavigator nav_;
    std::vector<const ValueProvider*> fieldProviders_;
    std::size_t count_;
    std::string sep_;
    std::string arraySep_;
public:
    ArrayStructBlockProviderA(IndexedNavigator nav, std::vector<const ValueProvider*> fields,
                              std::size_t count, std::string sep, std::string arraySep)
        : nav_(std::move(nav)), fieldProviders_(std::move(fields)),
          count_(count), sep_(std::move(sep)), arraySep_(std::move(arraySep)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};
```

src/ValueProvider.cpp:
```cpp
std::string ArrayStructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    if (fieldProviders_.empty()) return {};
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += arraySep_;                        // 元素间
        const void* child = nav_.navigate(structPtr, i);
        for (std::size_t j = 0; j < fieldProviders_.size(); ++j) {
            if (j) out += sep_;                         // 元素内字段间
            out += fieldProviders_[j]->get(child, ctx);
        }
    }
    return out;
}
std::string ArrayStructBlockProviderA::get(const void* structPtr, const DeviceCtx& ctx) const {
    // 与 ArrayStructBlockProvider::get 逻辑完全相同(字段遍历,不再调 runRecipeA)
    if (fieldProviders_.empty()) return {};
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += arraySep_;
        const void* child = nav_.navigate(structPtr, i);
        for (std::size_t j = 0; j < fieldProviders_.size(); ++j) {
            if (j) out += sep_;
            out += fieldProviders_[j]->get(child, ctx);
        }
    }
    return out;
}
```

> 注意:ArrayStructBlockProviderA 现在**不再调 runRecipeA**(它直接遍历字段,和 B 版本逻辑相同)。这意味着 Route A 和 Route B 对结构体数组的行为完全一致(都是字段遍历),ArrayStructBlockProviderA 和 ArrayStructBlockProvider 可以合并成一个类 —— 但为最小改动,先保留两个类(逻辑相同),后续可合并。

- [ ] **Step 6: 修改 tests/test_value_provider.cpp 的 ArrayStructBlockProvider case**

现有 case 用 `shared_ptr<const RecipeB> subConst` 构造 —— 改为字段列表。替换该 case:

```cpp
TEST_CASE("ArrayStructBlockProvider: navigates each element, joins fields with sep") {
    auto xp = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->x); return buf;
    });
    auto yp = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->y); return buf;
    });
    sv::IndexedNavigator nav([](const void* p, std::size_t i) -> const void* {
        const ArrEventBox* e = static_cast<const ArrEventBox*>(p);
        return &e->boxes[i];
    });
    // 字段 [x, y],元素内 sep="",元素间 sep="|" → "12|34|56|78"(x+y 拼成 "12")
    sv::ArrayStructBlockProvider asbp(nav, {xp.get(), yp.get()}, 4, "", "|");
    sv::DeviceCtx dummy;
    ArrEventBox e{ {{1,2},{3,4},{5,6},{7,8}} };
    CHECK(asbp.get(&e, dummy) == "12|34|56|78");
}
```
(同样改 ArrayStructBlockProviderA 的 case —— 签名相同,只是类名换 A。)

- [ ] **Step 7: 构建并运行 test_value_provider**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_value_provider --output-on-failure`
Expected: test_value_provider PASS。全量仍有回归(Builder/Validator 等还没改)—— 预期。

- [ ] **Step 8: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/ValueProvider.hpp src/ValueProvider.cpp tests/test_value_provider.cpp
git commit -m "feat(name): StructBlockProvider/Array* hold field list + sep (no sub-recipe)"
```
CRITICAL: COMMIT before reporting.

---

## Task 3: Recipe.hpp SubRecipeBinding 改字段列表(Route A) + RecipeRunner

**Files:**
- Modify: `include/struct_view/Recipe.hpp`
- Modify: `src/RecipeRunner.cpp`
- Test: `tests/test_route_a.cpp`(后续 Task 9 一起改,这里先保证编译)

- [ ] **Step 1: 修改 include/struct_view/Recipe.hpp —— SubRecipeBinding**

```cpp
struct SubRecipeBinding {
    Navigator nav;
    std::vector<const ValueProvider*> fieldProviders;   // 块内字段(按 fields 顺序)
    std::string sep;                                     // 字段间分隔符
};
```
(去掉 `std::shared_ptr<const RecipeA> sub`;加 fieldProviders + sep。注意:`const ValueProvider*` 需要 ValueProvider 完整定义 —— ValueProvider.hpp 已 include。)

- [ ] **Step 2: 修改 src/RecipeRunner.cpp —— runRecipeA SubRecipe case**

```cpp
            case StepKind::SubRecipe: {
                const auto& b = std::get<SubRecipeBinding>(s.binding);
                const void* child = b.nav.navigate(structPtr);
                for (std::size_t j = 0; j < b.fieldProviders.size(); ++j) {
                    if (j) out += b.sep;
                    out += b.fieldProviders[j]->get(child, ctx);
                }
            } break;
```
(不再递归调 runRecipeA;直接遍历字段列表。)

- [ ] **Step 3: 构建(预期 BuilderA 等还会失败,但 RecipeRunner 自身应编译)**

Run: `cmake --build build 2>&1 | grep -E "RecipeRunner|Recipe.hpp" | head -5`
Expected: RecipeRunner.cpp / Recipe.hpp 自身无错(其他文件的错后续 task 修)。

- [ ] **Step 4: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/Recipe.hpp src/RecipeRunner.cpp
git commit -m "feat(name): Route A SubRecipeBinding holds field list + sep (no sub-recipe)"
```

---

## Task 4: ConfigAst + ConfigLoader —— RecipeAst 加 desc

**Files:**
- Modify: `include/struct_view/ConfigAst.hpp`
- Modify: `src/ConfigLoader.cpp`
- Test: `tests/test_config_loader.cpp`

- [ ] **Step 1: 写失败测试(追加到 tests/test_config_loader.cpp)**

```cpp
TEST_CASE("ConfigLoader: parses recipe desc") {
    std::string text = R"({"recipes":[
        {"name":"r_alarm","desc":"告警","template":"${x}"}]})";
    auto result = sv::ConfigLoader::parse(text);
    REQUIRE(result.ok);
    REQUIRE(result.ast.recipes.size() == 1);
    CHECK(result.ast.recipes[0].desc == "告警");
}

TEST_CASE("ConfigLoader: missing desc defaults to empty") {
    auto result = sv::ConfigLoader::parse(R"({"recipes":[{"name":"r_x","template":"${a}"}]})");
    REQUIRE(result.ok);
    CHECK(result.ast.recipes[0].desc.empty());
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | head -5 && ctest --test-dir build -R test_config_loader --output-on-failure 2>&1 | tail -8`
Expected: FAIL — RecipeAst 无 desc 成员。

- [ ] **Step 3: 修改 include/struct_view/ConfigAst.hpp**

```cpp
struct RecipeAst {
    std::string name;
    std::string desc;   // 可选描述(给 client 显示)
    std::vector<Segment> segments;
};
```

- [ ] **Step 4: 修改 src/ConfigLoader.cpp**

在 parse 的 recipe 循环里加:`ra.desc = rc.value("desc", "");`(在 `ra.name = ...` 之后)。

- [ ] **Step 5: 构建并运行 test_config_loader**

Run: `cmake --build build 2>&1 | tail -3 && ctest --test-dir build -R test_config_loader --output-on-failure`
Expected: test_config_loader PASS(含 2 新 case)。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/ConfigAst.hpp src/ConfigLoader.cpp tests/test_config_loader.cpp
git commit -m "feat(name): RecipeAst + ConfigLoader parse recipe desc"
```

---

## Task 5: Validator —— r_ 前缀 + block fields 存在性 + 用户子配方循环(去 subRecipe 检查)

**Files:**
- Modify: `src/Validator.cpp`
- Test: `tests/test_validator.cpp`

- [ ] **Step 1: 写失败测试(追加到 tests/test_validator.cpp)**

```cpp
TEST_CASE("Validator: recipe name must start with r_") {
    auto ast = parse(R"({"recipes":[{"name":"bad","template":"${x}"}]})");
    sv::NameRegistry reg;
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool found = false;
    for (auto& e : errs) if (e.message.find("r_") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("Validator: field name must not start with r_") {
    // schema 里字段名 r_x 开头 → 报错。但 schema 不经 Validator(Validator 只看 ast + registry)。
    // 字段名 r_ 开头是在 codegen/schema 层校验?还是 Validator?
    // —— 字段名在 NameRegistry 里(registerProvider 的 name)。Validator 不直接看 schema。
    // 所以这条校验在 codegen(gen_registry.py 读 schema 时)或 NameRegistry(register 时)做。
    // 本测试改为:验证合规的 r_ 配方名 + 合规字段名通过。
    auto ast = parse(R"({"recipes":[{"name":"r_ok","template":"${x}"}]})");
    sv::NameRegistry reg;
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}

TEST_CASE("Validator: block fields must exist in struct") {
    // block 的 fields 字段不存在 → 报错。但 fields 在 schema/registry(StructBlockDecl)。
    // Validator 要校验 StructBlockDecl.fieldNames 里的每个字段名,在 NameRegistry 里有对应 provider。
    // 注册一个 person 块,fields=["name","age"],但只注册 name 不注册 age → 报错。
    auto ast = parse(R"({"recipes":[{"name":"r_main","template":"${person}"}]})");
    sv::NameRegistry reg;
    reg.registerStruct("person",
        sv::Navigator([](const void*) -> const void* { return nullptr; }),
        {"name", "age"}, "-", "");
    reg.registerProvider("name", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("n");}));
    // age 没注册
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool found = false;
    for (auto& e : errs) if (e.message.find("age") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("Validator: user sub-recipe cycle detected") {
    // r_a 引用 r_b,r_b 引用 r_a → 循环
    auto ast = parse(R"({"recipes":[
        {"name":"r_a","template":"${r_b}"},
        {"name":"r_b","template":"${r_a}"}]})";
    sv::NameRegistry reg;
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    bool foundCycle = false;
    for (auto& e : errs) if (e.message.find("cycle") != std::string::npos) foundCycle = true;
    CHECK(foundCycle);
}
```

> 注:test_validator.cpp 的 `parse` helper 当前可能用 `R"(...)"` —— 注意 `r_b}` 里的 `)"` 会终止 raw string。用 `R"js(...)js"`。检查现有 parse helper,必要时用 `R"js` 分隔符。

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | tail -3 && ctest --test-dir build -R test_validator --output-on-failure 2>&1 | tail -10`
Expected: FAIL — r_ 前缀/fields 存在性/用户子配方循环未校验。

- [ ] **Step 3: 修改 src/Validator.cpp**

**3a. 加 r_ 前缀检查**(在 validate 开头,check 1 之前):
```cpp
    // 0. recipe names must start with r_; field/block names must not.
    for (const auto& r : ast.recipes) {
        if (r.name.rfind("r_", 0) != 0) {
            errs.push_back({r.name, "recipe name must start with 'r_': " + r.name, 0});
        }
    }
    for (const auto& d : names.structDecls()) {
        if (d.first.rfind("r_", 0) == 0) {
            errs.push_back({d.first, "block name must not start with 'r_' (reserved for recipes): " + d.first, 0});
        }
    }
    // (字段名 r_ 开头的校验在 codegen/schema 层,见 Task 8)
```

**3b. 加 block fields 存在性检查**(在现有 check 之后):
```cpp
    // 4. each struct block's fieldNames must resolve to a registered provider.
    for (const auto& d : names.structDecls()) {
        for (const auto& fn : d.second.fieldNames) {
            if (!names.lookup(fn)) {
                errs.push_back({d.first, "block field not found in registry: " + fn, 0});
            }
        }
    }
    for (const auto& d : names.structArrayDecls()) {
        for (const auto& fn : d.second.fieldNames) {
            if (!names.lookup(fn)) {
                errs.push_back({d.first, "block field not found in registry: " + fn, 0});
            }
        }
    }
```

**3c. 改 hasCycle —— 只检测用户子配方循环**(结构体块不再引用子配方,不参与图):
```cpp
// Walk the user-sub-recipe -> referenced-name -> user-sub-recipe graph; detect cycles.
// Struct blocks/fields are leaves (they don't reference sub-recipes anymore).
static bool hasCycle(const NameRegistry& names,
                     const std::unordered_map<std::string, const RecipeAst*>& byName) {
    std::unordered_set<std::string> visiting, visited;
    std::function<bool(const std::string&)> dfs = [&](const std::string& nm) -> bool {
        if (visiting.count(nm)) return true;
        if (visited.count(nm)) return false;
        visiting.insert(nm);
        auto it = byName.find(nm);
        if (it != byName.end()) {
            for (const auto& seg : it->second->segments) {
                if (seg.isRef && byName.count(seg.text)) {   // seg.text 是另一个用户子配方?
                    if (dfs(seg.text)) { visiting.erase(nm); visited.insert(nm); return true; }
                }
            }
        }
        visiting.erase(nm);
        visited.insert(nm);
        return false;
    };
    for (const auto& [nm, _] : byName) if (dfs(nm)) return true;
    return false;
}
```
(用户子配方 = byName 里的配方(r_ 开头)。结构体块不再有 subRecipe,不进图。)

**3d. 去掉旧检查**:删除 `struct block subRecipe not found` 检查(check 2)、`struct array subRecipe not found`(check 2b)。这些 subRecipe 检查不再需要。check 1(引用解析)保留 —— `${person}` 仍解析为 struct block(在 structDecls 里)。

- [ ] **Step 4: 构建并运行 test_validator**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_validator --output-on-failure`
Expected: test_validator 新 case PASS。全量仍有回归(Builder 等)。

- [ ] **Step 5: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add src/Validator.cpp tests/test_validator.cpp
git commit -m "feat(name): Validator r_ prefix + block fields existence + user sub-recipe cycle"
```

---

## Task 6: Builder —— 块编译改字段列表(去子配方绑定)

**Files:**
- Modify: `src/Builder.cpp`
- Test: `tests/test_builder.cpp`

- [ ] **Step 1: 写失败测试(改 tests/test_builder.cpp 的 struct block case)**

现有 `test_builder.cpp` 有 `Builder: binds struct block sub-recipe pointers` case —— 它注册 `registerStruct("person", nav, "person")`(旧签名)并期望子配方绑定。改为新签名 + 字段列表:

```cpp
TEST_CASE("Builder: struct block compiles to field-list provider") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r_main","template":"[${person}]"},
        {"name":"r_other","template":"${name}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerStruct("person",
        sv::IndexedNavigator([](const void* p) -> const void* {  // 注意:registerStruct 用 Navigator 不是 IndexedNavigator
            // ... 实际用 Navigator
        }), {"name"}, "-", "");
    // 上面类型错了:registerStruct 用 Navigator(单元素),不是 IndexedNavigator。修正:
    reg.registerStruct("person",
        sv::Navigator([](const void* p) -> const void* {
            const Event* e = static_cast<const Event*>(p); return &e->person;
        }), {"name"}, "-", "");
    reg.registerProvider("name", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        return "Alice";
    }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("r_main");
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(nullptr, dummy);
    CHECK(out == "[Alice]");
}
```

> 注:这个测试注册了两个 recipe(r_main 和 r_other),但 r_other 没被引用 —— Validator 会校验所有 recipe 都 r_ 开头(都合规)。r_main 的 `${person}` 是 struct block,展开为 "Alice"(name 字段)。

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_builder --output-on-failure 2>&1 | tail -10`
Expected: FAIL — Builder Pass 2 还在用旧 subRecipe 绑定。

- [ ] **Step 3: 修改 src/Builder.cpp —— Pass 2 改字段列表**

把 Pass 2(单结构体块绑定)从「查子配方 + 创建 StructBlockProvider(nav, sub)」改为「查字段 provider 列表 + 创建 StructBlockProvider(nav, fields, sep)」:

```cpp
    // Pass 2: instantiate recipe-private StructBlockProviders (field list + sep).
    // No sub-recipe — field order + sep live in the schema (spec §4).
    for (auto& p : pending) {
        const StructBlockDecl* decl = names.findStructBlockDecl(p.blockName);
        if (!decl) {
            r.errors.push_back({p.rb->name, "unknown struct block: " + p.blockName, 0});
            continue;
        }
        // 组装字段 provider 列表(按 decl->fieldNames 顺序从 NameRegistry 查)
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = names.lookup(fn);
            if (!vp) {
                r.errors.push_back({p.rb->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        auto sbp = std::make_unique<StructBlockProvider>(decl->nav, std::move(fields), decl->sep);
        p.rb->steps[p.stepIdx].provider = sbp.get();
        p.rb->ownedProviders.push_back(std::move(sbp));
    }
```

Pass 2b(结构体数组)同理 —— 用 `decl->fieldNames` + `decl->sep`(元素内)+ `decl->arraySep`(元素间)+ `decl->count`:
```cpp
    for (auto& pa : pendingArray) {
        const StructArrayDecl* decl = names.findStructArrayDecl(pa.blockName);
        if (!decl) {
            r.errors.push_back({pa.rb->name, "unknown struct array: " + pa.blockName, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = names.lookup(fn);
            if (!vp) {
                r.errors.push_back({pa.rb->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        auto asbp = std::make_unique<ArrayStructBlockProvider>(
            decl->nav, std::move(fields), decl->count, decl->sep, decl->arraySep);
        pa.rb->steps[pa.stepIdx].provider = asbp.get();
        pa.rb->ownedProviders.push_back(std::move(asbp));
    }
```

- [ ] **Step 4: 构建并运行 test_builder**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_builder --output-on-failure`
Expected: test_builder 的新 case PASS。全量仍有回归(BuilderA/Engine/示例等)。

- [ ] **Step 5: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add src/Builder.cpp tests/test_builder.cpp
git commit -m "feat(name): Builder compiles struct blocks to field-list provider (no sub-recipe)"
```

---

## Task 7: BuilderA —— Route A 块编译改字段列表

**Files:**
- Modify: `src/BuilderA.cpp`

- [ ] **Step 1: 修改 src/BuilderA.cpp —— Pass 2 改字段列表(同 Builder,但塞 SubRecipeBinding)**

BuilderA 的 Pass 2 创建 `SubRecipeBinding`(Route A)。改为持字段列表 + sep:

```cpp
    // Pass 2: struct blocks — SubRecipeBinding holds field list + sep (no sub-recipe).
    for (auto& p : pending) {
        const StructBlockDecl* decl = names.findStructBlockDecl(p.blockName);
        if (!decl) {
            r.errors.push_back({p.ra->name, "unknown struct block: " + p.blockName, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = names.lookup(fn);
            if (!vp) {
                r.errors.push_back({p.ra->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        StepA step;
        step.kind = StepKind::SubRecipe;
        step.binding = SubRecipeBinding{decl->nav, std::move(fields), decl->sep};
        p.ra->steps[p.stepIdx] = std::move(step);
    }
```
Pass 2b(结构体数组)—— Route A 用 `ArrayStructBlockProviderA`(塞 FieldBinding,spec §5 不变):
```cpp
    for (auto& pa : pendingArray) {
        const StructArrayDecl* decl = names.findStructArrayDecl(pa.blockName);
        if (!decl) {
            r.errors.push_back({pa.ra->name, "unknown struct array: " + pa.blockName, 0});
            continue;
        }
        std::vector<const ValueProvider*> fields;
        bool missing = false;
        for (const auto& fn : decl->fieldNames) {
            const ValueProvider* vp = names.lookup(fn);
            if (!vp) {
                r.errors.push_back({pa.ra->name, "block field not found: " + fn, 0});
                missing = true;
            } else {
                fields.push_back(vp);
            }
        }
        if (missing) continue;
        auto asbp = std::make_unique<ArrayStructBlockProviderA>(
            decl->nav, std::move(fields), decl->count, decl->sep, decl->arraySep);
        StepA step;
        step.kind = StepKind::Field;
        step.binding = FieldBinding{asbp.get()};
        pa.ra->steps[pa.stepIdx] = std::move(step);
        pa.ra->ownedProviders.push_back(std::move(asbp));
    }
```
> 注:Route A 不再需要 `ArrayStructBlockProviderA` 调 `runRecipeA`(Task 2 已改为字段遍历,逻辑与 B 相同)。Route A 仍可用它(塞 FieldBinding)。

- [ ] **Step 2: 构建**

Run: `cmake --build build 2>&1 | tail -5`
Expected: BuilderA 编译通过(全量仍有 Engine/示例回归)。

- [ ] **Step 3: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add src/BuilderA.cpp
git commit -m "feat(name): BuilderA Route A struct blocks to field list (SubRecipeBinding)"
```

---

## Task 8: codegen —— block 生成 fields+sep+desc;field/device 带 desc;字段名 r_ 校验

**Files:**
- Modify: `tools/gen_registry.py`
- Test: `tests/test_codegen.py`

- [ ] **Step 1: 写失败测试(追加到 tests/test_codegen.py)**

```python
NAME_SCHEMA = {
    "deviceCtxType": "MyDeviceCtx",
    "structs": [
        {"name": "Event", "members": [
            {"field": "timestamp", "as": "time", "desc": "时间戳", "type": "uint64", "fmt": "%llu"},
            {"block": "person", "desc": "人体", "ptr": True, "sep": "-", "fields": ["name", "age"]},
            {"block": "boxes", "desc": "框数组", "sep": ",", "fields": ["x", "y"],
             "array": {"count": 4, "sep": "|"}}
        ]},
        {"name": "PersonInfo", "members": [
            {"field": "name", "desc": "姓名", "type": "cstr", "fmt": "%s"}
        ]}
    ],
    "device": [{"getter": "cameraId", "as": "camera", "desc": "摄像头"}]
}

def test_emits_block_with_fields_sep_desc():
    out = run_gen(json.dumps(NAME_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerStruct("person"' in out
    assert '{"name", "age"}' in out   # fields 列表
    assert '"-"' in out               # sep
    assert '"人体"' in out            # desc

def test_emits_struct_array_with_fields_sep_arraysep_desc():
    out = run_gen(json.dumps(NAME_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerStructArray("boxes"' in out
    assert '{"x", "y"}' in out
    assert '4' in out                 # count
    assert '"|"' in out               # arraySep

def test_emits_field_with_desc():
    out = run_gen(json.dumps(NAME_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerProvider("time"' in out
    assert '"时间戳"' in out

def test_rejects_field_name_starting_with_r_():
    schema = json.dumps({"deviceCtxType":"sv::DeviceCtx","structs":[
        {"name":"Ev","members":[{"field":"r_x","type":"int","fmt":"%d"}]}
    ]})
    run_gen_expect_error(schema, "r_")

def test_rejects_block_name_starting_with_r_():
    schema = json.dumps({"deviceCtxType":"sv::DeviceCtx","structs":[
        {"name":"Ev","members":[{"block":"r_bad","ptr":False,"sep":",","fields":["x"]}]}
    ]})
    run_gen_expect_error(schema, "r_")
```

- [ ] **Step 2: 运行验证失败**

Run: `python3 tests/test_codegen.py 2>&1 | tail -10`
Expected: FAIL — codegen 还没生成 fields/sep/desc。

- [ ] **Step 3: 修改 tools/gen_registry.py**

**3a. 改 block_line**(去掉 subRecipe,加 fields/sep/desc):
```python
def block_line(struct_name, member):
    block = member["block"]
    ptr = member.get("ptr", False)
    deref = f"static_cast<const void*>(s->{block})" if ptr else f"static_cast<const void*>(&s->{block})"
    fields = member.get("fields", [])
    sep = member.get("sep", "")
    desc = member.get("desc", "")
    fields_lit = "{" + ", ".join(f'"{f}"' for f in fields) + "}" if fields else "{}"
    sep_lit = sep.replace('\\', '\\\\').replace('"', '\\"')
    desc_lit = desc.replace('\\', '\\\\').replace('"', '\\"')
    return f'''    reg.registerStruct("{block}", sv::Navigator(
        [](const void* p) -> const void* {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            return {deref};
        }}), {fields_lit}, "{sep_lit}", "{desc_lit}");\n'''
```

**3b. 改 block_array_line**(去掉 subRecipe,加 fields/sep/arraySep/desc):
```python
def block_array_line(struct_name, member):
    block = member["block"]
    arr = member["array"]
    count = arr["count"]
    array_sep = arr["sep"]
    fields = member.get("fields", [])
    sep = member.get("sep", "")
    desc = member.get("desc", "")
    fields_lit = "{" + ", ".join(f'"{f}"' for f in fields) + "}" if fields else "{}"
    sep_lit = sep.replace('\\', '\\\\').replace('"', '\\"')
    array_sep_lit = array_sep.replace('\\', '\\\\').replace('"', '\\"')
    desc_lit = desc.replace('\\', '\\\\').replace('"', '\\"')
    return f'''    reg.registerStructArray("{block}", sv::IndexedNavigator(
        [](const void* p, std::size_t i) -> const void* {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            static_assert(std::extent_v<decltype(s->{block})> >= {count},
                          "struct_view: {block} length drift (schema count={count})");
            return &s->{block}[i];
        }}), {fields_lit}, {count}, "{sep_lit}", "{array_sep_lit}", "{desc_lit}");\n'''
```

**3c. 改 field_line / field_array_line / device_line —— 带 desc**:
每个函数读 `desc = member.get("desc", "")`,在 `registerProvider(...)` 调用末尾加 `, "{desc_lit}"`。示例(field_line):
```python
def field_line(struct_name, member):
    field = member["field"]
    as_name = member.get("as", field)
    typ = member["type"]
    fmt = member["fmt"]
    cast = TYPE_CAST[typ]
    desc = member.get("desc", "")
    desc_lit = desc.replace('\\', '\\\\').replace('"', '\\"')
    return f'''    reg.registerProvider("{as_name}", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "{fmt}", {cast}s->{field});
            return std::string(buf);
        }}), "{desc_lit}");\n'''
```
(field_array_line 同理加 desc;device_line 同理。)

**3d. validate_member 加 r_ 前缀校验**(在 validate_member 里):
```python
    # field/block name must not start with r_ (reserved for recipes)
    name = m.get("field") or m.get("block")
    if name and name.startswith("r_"):
        raise ValueError(
            f"struct_view schema: struct '{struct_name}' field/block '{name}' must not start "
            f"with 'r_' (reserved for recipe names).")
    # block 必须有 fields + sep
    if has_block:
        if "fields" not in m:
            raise ValueError(f"struct_view schema: struct '{struct_name}' block '{m['block']}' is missing required key 'fields'.")
        if "sep" not in m:
            raise ValueError(f"struct_view schema: struct '{struct_name}' block '{m['block']}' is missing required key 'sep'.")
```
(加在 validate_member 末尾。)

- [ ] **Step 4: 运行 Python 测试**

Run: `python3 tests/test_codegen.py 2>&1 | tail -10`
Expected: 全部 PASS(含 5 新 case)。

- [ ] **Step 5: 编译验证生成代码**

```bash
cd /Users/lpf2026/MyCode/struct_view
python3 tools/gen_registry.py <(echo '{"deviceCtxType":"sv::DeviceCtx","structs":[{"name":"Ev","members":[{"field":"x","desc":"X","type":"int","fmt":"%d"},{"block":"b","sep":",","fields":["x"],"ptr":false}]}]}') /tmp/nm.cpp
grep -n "registerStruct\|registerProvider\|desc" /tmp/nm.cpp
```
Expected: 生成的代码含 `registerStruct("b", ..., {"x"}, ",", "X")` 和 `registerProvider("x", ..., "X")`。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add tools/gen_registry.py tests/test_codegen.py
git commit -m "feat(name): codegen block fields+sep+desc; field/device desc; r_ prefix validation"
```

---

## Task 9: Engine —— recipeDescs_ + describeRecipe()

**Files:**
- Modify: `include/struct_view/Engine.hpp`
- Modify: `src/Engine.cpp`
- Test: `tests/test_engine.cpp`

- [ ] **Step 1: 写失败测试(追加到 tests/test_engine.cpp)**

```cpp
TEST_CASE("Engine: describeRecipe returns desc after loadConfig") {
    sv::NameRegistry reg;
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(R"({"recipes":[
        {"name":"r_alarm","desc":"告警行","template":"${x}"}]})").ok);
    CHECK(engine.describeRecipe("r_alarm") == "告警行");
    CHECK(engine.describeRecipe("nope").empty());
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | tail -3 && ctest --test-dir build -R test_engine --output-on-failure 2>&1 | tail -5`
Expected: FAIL — describeRecipe 未定义。

- [ ] **Step 3: 修改 include/struct_view/Engine.hpp**

加 private 成员 + public 方法:
```cpp
    std::unordered_map<std::string, std::string> recipeDescs_;   // 配方名 → desc
public:
    const std::string& describeRecipe(const std::string& recipeName) const;
```
(加 `#include <unordered_map>` 若未有。)

- [ ] **Step 4: 修改 src/Engine.cpp**

**4a. loadConfig 里抽 desc**(在 publishAll 之前,遍历 parsed.ast.recipes 抽 desc):
```cpp
    // 抽取配方 desc 存进 recipeDescs_(随 loadConfig 一起替换)
    std::unordered_map<std::string, std::string> newDescs;
    for (const auto& r : parsed.ast.recipes) {
        if (!r.desc.empty()) newDescs[r.name] = r.desc;
    }
    recipeDescs_ = std::move(newDescs);   // 替换(与 publishAll 同时机)
```
(放在 `store_.publishAll(...)` 之前。Route A 的 loadConfigA 同理加 `recipeDescsA_`?—— 不,配方 desc 和 route 无关,一份就够。loadConfigA 也更新 recipeDescs_。)

**4b. describeRecipe 实现**:
```cpp
const std::string& Engine::describeRecipe(const std::string& recipeName) const {
    auto it = recipeDescs_.find(recipeName);
    static const std::string EMPTY;
    return it == recipeDescs_.end() ? EMPTY : it->second;
}
```

- [ ] **Step 5: 构建并运行 test_engine**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_engine --output-on-failure`
Expected: test_engine 新 case PASS。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/Engine.hpp src/Engine.cpp tests/test_engine.cpp
git commit -m "feat(name): Engine recipeDescs_ + describeRecipe()"
```

---

## Task 10: 端到端示例迁移 + 全量测试修复

**Files:**
- Modify: `examples/security/schema.json`(block 加 sep/fields/desc,去 subRecipe;字段加 desc)
- Modify: `examples/security/recipes.json`(配方名 r_ 前缀;删 r_person/r_box 子配方;加 desc)
- Regenerate: `examples/security/registry_gen.cpp`
- Modify: `examples/security/main.cpp`(render 用 r_alarm_line)
- Modify: `tests/test_example_e2e.cpp`(r_ 前缀 + 新块展开 + desc 断言)
- Modify: `tests/test_builder.cpp`(其余 case 迁移到 r_ + 字段列表)
- Modify: `tests/test_validator.cpp`(其余 case 迁移)
- Modify: `tests/test_route_a.cpp`(r_ + 字段列表)
- Modify: `tests/test_hot_reload.cpp`(r_ + 字段列表)
- Modify: `tests/test_drift.sh` / `drift_schema.json`(r_ 不影响 drift,但 schema 若有 block 要加 fields/sep)

- [ ] **Step 1: 改 examples/security/schema.json**

```json
{
  "deviceCtxType": "MyDeviceCtx",
  "structs": [
    {"name": "Event", "members": [
        {"field": "timestamp", "as": "time", "desc": "时间戳", "type": "uint64", "fmt": "%llu"},
        {"block": "person", "desc": "人体信息", "ptr": true, "sep": "-", "fields": ["name", "age"]},
        {"block": "rect",   "desc": "检测框",   "ptr": false, "sep": ",", "fields": ["x", "y", "w", "h", "tags"]},
        {"field": "feature_ids", "as": "feat_ids", "desc": "特征ID列表", "type": "int", "fmt": "%d",
         "array": {"count": 8, "sep": "-"}},
        {"block": "boxes", "desc": "检测框数组", "sep": ",", "fields": ["x", "y", "w", "h", "tags"],
         "array": {"count": 4, "sep": "|"}}
    ]},
    {"name": "PersonInfo", "members": [
        {"field": "name", "desc": "姓名", "type": "cstr", "fmt": "%s"},
        {"field": "age",  "desc": "年龄", "type": "int",   "fmt": "%d"}
    ]},
    {"name": "Box", "members": [
        {"field": "x", "desc": "横坐标", "type": "int", "fmt": "%d"},
        {"field": "y", "desc": "纵坐标", "type": "int", "fmt": "%d"},
        {"field": "w", "desc": "宽", "type": "int", "fmt": "%d"},
        {"field": "h", "desc": "高", "type": "int", "fmt": "%d"},
        {"field": "tags", "desc": "标签", "type": "int", "fmt": "%d", "array": {"count": 2, "sep": "-"}}
    ]}
  ],
  "device": [{"getter": "cameraId", "as": "camera", "desc": "摄像头编号"}]
}
```

- [ ] **Step 2: 改 examples/security/recipes.json**

```json
{
  "recipes": [
    { "name": "r_alarm_line", "desc": "告警行", "template": "${camera}:${time}|${person}@${rect}|${feat_ids}|${boxes}" }
  ]
}
```
(删掉 r_person / r_box / r_box_arr —— 结构体块展开规则在 schema 了。只留顶层 r_alarm_line。)

- [ ] **Step 3: 重新生成 registry_gen.cpp**

Run: `python3 tools/gen_registry.py examples/security/schema.json examples/security/registry_gen.cpp`
Verify: `grep -n "registerStruct\|fields\|sep" examples/security/registry_gen.cpp` —— 应见 `registerStruct("person", ..., {"name", "age"}, "-", "人体信息")` 等。

- [ ] **Step 4: 改 examples/security/main.cpp**

`render("alarm_line", ...)` → `render("r_alarm_line", ...)`。

- [ ] **Step 5: 改 tests/test_example_e2e.cpp**

配方名 r_alarm_line;loadConfig 的 JSON 用 `R"js(...)js"`(含 `(${tags})`?—— 现在 box 是块,recipe 里没有 `${tags}` 模板了,template 是 `${camera}:${time}|${person}@${rect}|${feat_ids}|${boxes}`,无 `(${tags})`,所以 `R"(...)"` 可用)。数据构造不变。期望输出:
```
CAM001:1717171717|Alice-30@100,200,300,400,7-8|11-22-33-0-0-0-0-0|100,200,300,400,7-8|110,210,310,410,3-4|0,0,0,0,0-0|0,0,0,0,0-0
```
注意:`${rect}` 现在按 schema 的 fields [x,y,w,h,tags] + sep "," 展开 → `100,200,300,400,7-8`(tags 是数组,内部 `-`)。`${boxes}` 每个元素同理,元素间 `|`。

- [ ] **Step 6: 修其余测试(r_ 前缀 + 字段列表)**

逐个测试文件迁移:配方名加 r_;registerStruct 用新签名(fields+sep);块不再绑子配方。具体:
- `test_builder.cpp`:其余 case 的 registerStruct 改新签名;配方名 r_。
- `test_validator.cpp`:其余 case 迁移。
- `test_route_a.cpp`:r_ + 字段列表(parity)。
- `test_hot_reload.cpp`:r_ + 字段列表。
- `test_drift.sh` / `drift_schema.json`:drift_header.h 的 DriftEvent 若有 block,加 fields/sep(当前 drift 只测 field,可能不用改 —— 检查)。

- [ ] **Step 7: 全量构建 + 测试**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build --output-on-failure 2>&1 | tail -5`
Expected: 全量 PASS(12/12 或更多)。

- [ ] **Step 8: TSan**

Run: `cmake --build build_tsan -q 2>&1 | tail -2 && ctest --test-dir build_tsan --output-on-failure 2>&1 | tail -3 && ./build_tsan/test_hot_reload 2>&1 | grep -c "ThreadSanitizer: data race"`
Expected: 0 races。

- [ ] **Step 9: codegen + drift**

Run: `python3 tests/test_codegen.py 2>&1 | tail -5 && ./tests/test_drift.sh 2>&1 | tail -3 && ./build/security_example`
Expected: codegen 全 PASS;drift ALL OK;example 输出正确。

- [ ] **Step 10: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add examples/security/ tests/
git commit -m "feat(name): migrate example + all tests to r_ prefix + block fields + desc"
```

---

## Self-Review (plan author)

**1. Spec coverage:**
- §2 命名约定(r_ 前缀)→ Task 5(Validator)+ Task 8(codegen 字段名 r_ 校验)。✓
- §3 desc 字段 → Task 1(NameRegistry describe)+ Task 4(RecipeAst desc)+ Task 9(Engine describeRecipe)+ Task 8(codegen 带 desc)。✓
- §4 块展开规则收进 schema → Task 1(StructBlockDecl fields+sep)+ Task 2(StructBlockProvider 字段列表)+ Task 6(Builder 组装)+ Task 8(codegen 生成 fields+sep)。✓
- §5 用户组合子配方保留 → Task 5(用户子配方循环检测)+ 现有 RecipeB 机制不变。✓
- §6 Validator 校验项 → Task 5(r_ + fields 存在性 + 用户子配方循环 + 去 subRecipe 检查)。✓
- §7 ConfigLoader 解析 desc → Task 4。✓
- §8 codegen → Task 8。✓
- §9 示例迁移 → Task 10。✓
- §10 测试 → 各 task + Task 10。✓
- §4.2 Route A SubRecipeBinding → Task 3 + Task 7。✓

**2. Placeholder scan:** 无 TBD/TODO。每个 step 有完整代码或命令。Task 10 的「逐个测试文件迁移」给了具体清单 + 关键改动(r_ 前缀 + registerStruct 新签名),非占位。✓

**3. Type consistency:**
- `registerStruct(name, nav, fieldNames, sep, desc)` —— Task 1/6/7/8 一致。✓
- `registerStructArray(name, nav, fieldNames, count, sep, arraySep, desc)` —— Task 1/6/7/8 一致。✓
- `StructBlockProvider(nav, fields, sep)` —— Task 2/6 一致。✓
- `ArrayStructBlockProvider(nav, fields, count, sep, arraySep)` —— Task 2/6/7 一致。✓
- `SubRecipeBinding{nav, fieldProviders, sep}` —— Task 3/7 一致。✓
- `describe(name)` / `describeRecipe(name)` —— Task 1/9 一致。✓

无 gap。Plan 完整。
