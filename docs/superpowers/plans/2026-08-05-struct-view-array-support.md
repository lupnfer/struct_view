# struct_view 数组字段支持 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 struct_view 框架加上对标量数组(`int ids[8]`)与结构体数组(`Box boxes[4]`)的逐元素遍历拼接支持,含 `std::extent_v` 编译期长度漂移防护。

**Architecture:** 标量数组复用已有 `LambdaProvider`(codegen 生成循环体,零新增运行时类);结构体数组新增 `IndexedNavigator` + `ArrayStructBlockProvider`(与现有 `Navigator`/`StructBlockProvider` 同构,RCU 三规定照搬)。每数组生成 `static_assert(std::extent_v<decltype(s->arr)> >= count)` 防长度漂移。结构体数组里字段含数组(如 `boxes[4]` 里 `Box` 有 `tags[2]`)是方案 1 的天然组合,零额外设计。

**Tech Stack:** C++17(`std::extent_v` 在 `<type_traits>`),Python 3(codegen),nlohmann/json + doctest(vendored)。

**Spec:** `docs/superpowers/specs/2026-08-05-struct-view-array-support.md`

---

## File Structure

修改既有文件为主,不新增目录。

```
struct_view/
├── include/struct_view/
│   ├── ValueProvider.hpp    # 追加 IndexedNavigator + ArrayStructBlockProvider
│   └── NameRegistry.hpp     # 追加 StructArrayDecl + registerStructArray + structArrayDecls()
├── src/
│   ├── ValueProvider.cpp    # 追加 ArrayStructBlockProvider::get
│   ├── NameRegistry.cpp     # 追加 registerStructArray / structArrayDecls 实现
│   ├── Validator.cpp        # 扩展:structArrayDecls 纳入存在性 + 环检测
│   ├── Builder.cpp          # Pass 1 pendingArray + Pass 2 创建 ArrayStructBlockProvider
│   └── BuilderA.cpp         # 同 Builder,但塞进 FieldBinding(Route A 对等)
├── tools/gen_registry.py    # 加 field_array_line / block_array_line + HEADER 加 <type_traits>
├── examples/security/
│   ├── event.h              # Box 加 tags[2];Event 加 feature_ids[8] + boxes[4]
│   ├── schema.json          # 加数组 member
│   ├── registry_gen.cpp     # 重生成(codegen 产物)
│   ├── recipes.json         # 配方含 ${feat_ids} / ${boxes} / ${tags}
│   ├── main.cpp             # 装配数据含数组
│   └── (test_example_e2e.cpp 在 tests/)
└── tests/
    ├── test_codegen.py      # +2 case(标量/结构体数组生成形状)
    ├── test_drift.sh        # +数组长度漂移 case
    ├── test_name_registry.cpp # +registerStructArray case
    ├── test_builder.cpp     # +3 case(标量数组/结构体数组/嵌套组合)
    ├── test_engine.cpp      # +含数组配方 case
    ├── test_hot_reload.cpp  # 扩展配方含结构体数组
    ├── test_route_a.cpp     # 扩展配方含数组(Route A 对等)
    └── test_example_e2e.cpp # 断言含数组的精确输出串
```

**责任边界:** `IndexedNavigator`/`ArrayStructBlockProvider` 与现有 `Navigator`/`StructBlockProvider` 同构,放同一文件(`ValueProvider.hpp`)。`StructArrayDecl` 与 `StructBlockDecl` 同构,放 `NameRegistry.hpp`。codegen 数组分支与现有 `field_line`/`block_line` 并列。

---

## Task 1: IndexedNavigator + ArrayStructBlockProvider(运行时新增类)

**Files:**
- Modify: `include/struct_view/ValueProvider.hpp`(在 `StructBlockProvider` 之后、`} // namespace sv` 之前追加)
- Modify: `src/ValueProvider.cpp`(追加 `ArrayStructBlockProvider::get`)
- Test: `tests/test_value_provider.cpp`(追加 case)

- [ ] **Step 1: 写失败测试(追加到 tests/test_value_provider.cpp 末尾)**

先在文件顶部 includes 区加 `#include <struct_view/Recipe.hpp>`(若已有则跳过)、`#include <cstdio>`(若已有则跳过)。然后追加:

```cpp
namespace {
struct ArrEvent { int ids[8]; };
struct ArrBox { int x, y; };
struct ArrEventBox { ArrBox boxes[4]; };
}

TEST_CASE("ArrayStructBlockProvider: navigates each element, runs sub-recipe, joins with sep") {
    // 手搓一个子配方 box:${x},${y},用 ArrayStructBlockProvider 遍历 boxes[4]
    auto sub = std::make_shared<const sv::RecipeB>();
    // 子配方需要 x/y provider — 这里用 makeProvider 直接绑 ArrBox 字段
    // 但 RecipeB 的 step.provider 需要指向 provider;为隔离测试,我们把 provider
    // 存在 sub->ownedProviders 里,step.provider 指向它。
    auto xp = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->x); return buf;
    });
    auto yp = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->y); return buf;
    });
    sv::StepB sx{xp.get()}, sy{yp.get()};
    sub->steps.push_back(sx);
    sub->steps.push_back(sy);  // 注意:没把字面"," 加进去,本测试只验数组遍历+join
    // 为了让 sub 持有 xp/yp 生命期,放进 ownedProviders(const_cast 绕过 shared_ptr 的 const)
    // 更干净的做法:sub 用 shared_ptr<RecipeB>(非 const)构造,填充后再转 const。
    // 这里为简化:直接用 const_cast 把 provider 塞进 ownedProviders(测试专用,生产代码不这样)。
    // —— 实际上 RecipeB::ownedProviders 是 vector<unique_ptr<ValueProvider>>,在 const 配方里
    // 不可变。正确做法见下:用非 const 构建。
    // 重写(干净版):
    auto subMut = std::make_shared<sv::RecipeB>();
    {
        auto xq = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
            const ArrBox* b = static_cast<const ArrBox*>(p);
            char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->x); return buf;
        });
        auto yq = sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
            const ArrBox* b = static_cast<const ArrBox*>(p);
            char buf[16]; std::snprintf(buf, sizeof(buf), "%d", b->y); return buf;
        });
        sv::StepB sx2{xq.get()}, sy2{yq.get()};
        subMut->steps.push_back(sx2);
        subMut->steps.push_back(sy2);
        subMut->ownedProviders.push_back(std::move(xq));
        subMut->ownedProviders.push_back(std::move(yq));
    }
    std::shared_ptr<const sv::RecipeB> subConst = std::move(subMut);

    sv::IndexedNavigator nav([](const void* p, std::size_t i) -> const void* {
        const ArrEventBox* e = static_cast<const ArrEventBox*>(p);
        return &e->boxes[i];
    });
    sv::ArrayStructBlockProvider asbp(nav, subConst, 4, "|");
    sv::DeviceCtx dummy;
    ArrEventBox e{ {{{1,2},{3,4},{5,6},{7,8}}} };
    // 每个 box 跑 "x,y" 子配方(无逗号,因为本测试只验数组遍历)→ "12|34|56|78"
    CHECK(asbp.get(&e, dummy) == "12|34|56|78");
}
```

> 测试说明:子配方只放 x、y 两个 provider(不加字面逗号),目的是隔离验证「数组遍历 + sep 连接 + 每元素跑子配方」。完整含逗号的拼接在 Task 7 端到端测试覆盖。若你觉得测试里 x、y 输出无分隔(`12` 而非 `1,2`)易混淆,可在子配方 steps 间插一个字面 `","` 的 ConnectorProvider —— 但那需要在 ownedProviders 里再加一个 provider,代码更长。保持当前简洁版即可,语义清晰(`12` = x="1" + y="2")。

- [ ] **Step 2: 运行验证失败**

Run: `cd /Users/lpf2026/MyCode/struct_view && cmake --build build 2>&1 | head -20`
Expected: FAIL —— `IndexedNavigator`/`ArrayStructBlockProvider` 未定义。

- [ ] **Step 3: 追加到 include/struct_view/ValueProvider.hpp**

在 `StructBlockProvider` 类定义之后、`} // namespace sv` 之前追加:

```cpp
// Indexed navigator: parent + index i → child element pointer.
// Used by ArrayStructBlockProvider to iterate struct arrays. Mirrors Navigator
// but takes a per-element index (spec §4.1 of array-support spec).
class IndexedNavigator {
    const void* (*fn_)(const void*, std::size_t);
public:
    explicit IndexedNavigator(const void* (*f)(const void*, std::size_t)) : fn_(f) {}
    const void* navigate(const void* parent, std::size_t i) const { return fn_(parent, i); }
};

// A struct array: for each of count_ elements, navigate to &s->arr[i], run the
// sub-recipe, join with sep_. Recipe-private, holds shared_ptr<const RecipeB> to
// its sub-recipe (RCU §3.4a Rule 1/3 — same pattern as StructBlockProvider).
class ArrayStructBlockProvider : public ValueProvider {
    IndexedNavigator nav_;
    std::shared_ptr<const RecipeB> sub_;
    std::size_t count_;
    std::string sep_;
public:
    ArrayStructBlockProvider(IndexedNavigator nav, std::shared_ptr<const RecipeB> sub,
                             std::size_t count, std::string sep)
        : nav_(std::move(nav)), sub_(std::move(sub)),
          count_(count), sep_(std::move(sep)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};
```

- [ ] **Step 4: 追加到 src/ValueProvider.cpp**

在文件末尾 `} // namespace sv` 之前追加:

```cpp
std::string ArrayStructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += sep_;                         // join 语义(首元素前不加)
        const void* child = nav_.navigate(structPtr, i);
        out += runRecipeB(*sub_, child, ctx);       // 每个元素跑子配方
    }
    return out;
}
```

- [ ] **Step 5: 构建并运行**

Run: `cd /Users/lpf2026/MyCode/struct_view && cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_value_provider --output-on-failure`
Expected: test_value_provider PASS(含新增的 ArrayStructBlockProvider case)。全量 `ctest --test-dir build` 也应无回归。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/ValueProvider.hpp src/ValueProvider.cpp tests/test_value_provider.cpp
git commit -m "feat(array): IndexedNavigator + ArrayStructBlockProvider (struct array provider)"
```

---

## Task 2: NameRegistry —— StructArrayDecl + registerStructArray

**Files:**
- Modify: `include/struct_view/NameRegistry.hpp`
- Modify: `src/NameRegistry.cpp`
- Test: `tests/test_name_registry.cpp`

- [ ] **Step 1: 写失败测试(追加到 tests/test_name_registry.cpp 末尾)**

```cpp
TEST_CASE("NameRegistry: registerStructArray stores decl") {
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "box", 4, "|");
    auto decls = reg.structArrayDecls();
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].first == "boxes");
    CHECK(decls[0].second.subRecipeName == "box");
    CHECK(decls[0].second.count == 4);
    CHECK(decls[0].second.sep == "|");
}

TEST_CASE("NameRegistry: struct array not in lookup (only scalars/devices)") {
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "box", 4, "|");
    CHECK(reg.lookup("boxes") == nullptr);   // 结构体数组不在 entries_
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | head -10`
Expected: FAIL —— `registerStructArray`/`structArrayDecls`/`StructArrayDecl` 未定义。

- [ ] **Step 3: 修改 include/struct_view/NameRegistry.hpp**

在 `StructBlockDecl` 之后追加 `StructArrayDecl`:

```cpp
// Struct-array declaration: indexed nav + sub-recipe name + count + sep.
// Same pattern as StructBlockDecl — declaration only, no bound provider here
// (binding is recipe-private, spec §3.4a Rule 1).
struct StructArrayDecl {
    IndexedNavigator nav;
    std::string subRecipeName;
    std::size_t count;
    std::string sep;
};
```

在 `NameRegistry` 类的 private 区(`structDecls_` 之后)加成员:

```cpp
    std::vector<std::pair<std::string, StructArrayDecl>> structArrayDecls_;
```

在 public 区(`structDecls()` 之后)加方法声明:

```cpp
    // For struct arrays (codegen emits registerStructArray). Declaration only.
    void registerStructArray(std::string name, IndexedNavigator nav,
                             std::string subRecipeName, std::size_t count, std::string sep);
    // For Validator (existence/cycle) and Builder (instantiate per-recipe
    // ArrayStructBlockProviders).
    const std::vector<std::pair<std::string, StructArrayDecl>>& structArrayDecls() const;
```

- [ ] **Step 4: 修改 src/NameRegistry.cpp —— 追加实现**

在文件末尾 `} // namespace sv` 之前追加:

```cpp
void NameRegistry::registerStructArray(std::string name, IndexedNavigator nav,
                                       std::string subRecipeName, std::size_t count, std::string sep) {
    StructArrayDecl decl{nav, subRecipeName, count, sep};
    structArrayDecls_.emplace_back(std::move(name), std::move(decl));
}
const std::vector<std::pair<std::string, StructArrayDecl>>& NameRegistry::structArrayDecls() const {
    return structArrayDecls_;
}
```

- [ ] **Step 5: 构建并运行**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_name_registry --output-on-failure && ctest --test-dir build --output-on-failure 2>&1 | tail -4`
Expected: test_name_registry PASS(含 2 新 case)。全量无回归。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/NameRegistry.hpp src/NameRegistry.cpp tests/test_name_registry.cpp
git commit -m "feat(array): NameRegistry registerStructArray + StructArrayDecl"
```

---

## Task 3: codegen —— 标量数组 + 结构体数组生成

**Files:**
- Modify: `tools/gen_registry.py`
- Test: `tests/test_codegen.py`

- [ ] **Step 1: 写失败测试(追加到 tests/test_codegen.py,在现有 test_* 函数之后、`if __name__` 之前)**

```python
ARRAY_SCHEMA = {
    "deviceCtxType": "sv::DeviceCtx",
    "structs": [
        {"name": "Event", "members": [
            {"field": "feature_ids", "as": "feat_ids", "type": "int", "fmt": "%d",
             "array": {"count": 8, "sep": "-"}},
            {"block": "boxes", "array": {"subRecipe": "box", "count": 4, "sep": "|"}}
        ]}
    ]
}

def test_emits_scalar_array_loop_and_static_assert():
    out = run_gen(json.dumps(ARRAY_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerProvider("feat_ids"' in out
    assert 'for (std::size_t i = 0; i < 8; ++i)' in out
    assert 'if (i) out += "-";' in out
    assert 'std::extent_v<decltype(s->feature_ids)> >= 8' in out
    assert '(int)s->feature_ids[i]' in out

def test_emits_struct_array_register_struct_array_and_static_assert():
    out = run_gen(json.dumps(ARRAY_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerStructArray("boxes"' in out
    assert 'std::extent_v<decltype(s->boxes)> >= 4' in out
    assert 'return &s->boxes[i];' in out
    assert '"box", 4, "|"' in out
```

- [ ] **Step 2: 运行验证失败**

Run: `cd /Users/lpf2026/MyCode/struct_view && python3 tests/test_codegen.py 2>&1 | tail -10`
Expected: FAIL —— 新增的 2 个 test_* 函数报 assertion error(生成代码里还没有数组分支)。

- [ ] **Step 3: 修改 tools/gen_registry.py**

**3a. HEADER 加 include**(在现有 `#include <cstdio>` 之后、`#include <string>` 之前/之后均可,加这两行):
```python
HEADER = """\
// AUTO-GENERATED by tools/gen_registry.py — DO NOT EDIT.
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <cstddef>
#include <cstdio>
#include <string>
#include <type_traits>

void registerStructViewNames(sv::NameRegistry& reg) {
"""
```

**3b. 加两个生成函数**(在现有 `block_line` 之后、`device_line` 之前插入):

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
    sep_lit = sep.replace('\\', '\\\\').replace('"', '\\"')
    return f'''    reg.registerProvider("{as_name}", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            static_assert(std::extent_v<decltype(s->{field})> >= {count},
                          "struct_view: {field} length drift (schema count={count})");
            std::string out;
            char buf[256];
            for (std::size_t i = 0; i < {count}; ++i) {{
                if (i) out += "{sep_lit}";
                std::snprintf(buf, sizeof(buf), "{fmt}", {cast}s->{field}[i]);
                out += buf;
            }}
            return out;
        }}));\n'''

def block_array_line(struct_name, member):
    block = member["block"]
    arr = member["array"]
    sub = arr["subRecipe"]
    count = arr["count"]
    sep = arr["sep"]
    sep_lit = sep.replace('\\', '\\\\').replace('"', '\\"')
    return f'''    reg.registerStructArray("{block}", sv::IndexedNavigator(
        [](const void* p, std::size_t i) -> const void* {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            static_assert(std::extent_v<decltype(s->{block})> >= {count},
                          "struct_view: {block} length drift (schema count={count})");
            return &s->{block}[i];
        }}), "{sub}", {count}, "{sep_lit}");\n'''
```

**3c. 修改 `gen()` 的 member 分派**(把现有的 `if "field" in m / elif "block" in m` 改为先判 `array`):

```python
def gen(schema):
    device_ctx_type = schema.get("deviceCtxType", "sv::DeviceCtx")
    out = HEADER
    for st in schema.get("structs", []):
        sn = st["name"]
        for m in st.get("members", []):
            if "array" in m:
                if "field" in m:
                    out += field_array_line(sn, m)
                elif "block" in m:
                    out += block_array_line(sn, m)
            elif "field" in m:
                out += field_line(sn, m)
            elif "block" in m:
                out += block_line(sn, m)
    for d in schema.get("device", []):
        out += device_line(device_ctx_type, d)
    out += FOOTER
    return out
```

- [ ] **Step 4: 运行 Python 测试**

Run: `python3 tests/test_codegen.py 2>&1 | tail -10`
Expected: 全部 test_* PASS(含 2 新 case)。

- [ ] **Step 5: 编译验证生成代码是合法 C++**

```bash
cd /Users/lpf2026/MyCode/struct_view
cat > /tmp/arr_schema.json <<'EOF'
{"deviceCtxType":"sv::DeviceCtx","structs":[{"name":"ArrEv","members":[
  {"field":"ids","as":"ids","type":"int","fmt":"%d","array":{"count":4,"sep":"-"}},
  {"field":"name","as":"name","type":"cstr","fmt":"%s"}
]}]}
EOF
python3 tools/gen_registry.py /tmp/arr_schema.json /tmp/arr_gen.cpp
cat > /tmp/arr_main.cpp <<'EOF'
#include <cstdint>
#include <cstdio>
struct ArrEv { int ids[4]; char name[16]; };
#include <struct_view/NameRegistry.hpp>
#include "/tmp/arr_gen.cpp"
int main() {
    sv::NameRegistry r; registerStructViewNames(r);
    ArrEv e{{11,22,33,44},"hi"};
    const sv::ValueProvider* p = r.lookup("ids");
    sv::DeviceCtx dummy;
    std::printf("ids=%s name=%s\n", p->get(&e, dummy).c_str(), r.lookup("name")->get(&e, dummy).c_str());
    return (p->get(&e, dummy) == "11-22-33-44") ? 0 : 1;
}
EOF
g++ -std=c++17 -Iinclude -Ithird_party /tmp/arr_main.cpp src/*.cpp -o /tmp/arr_exe 2>&1 | tail -10
/tmp/arr_exe && echo "COMPILE+RUN OK"
```
Expected: 打印 `ids=11-22-33-44 name=hi`,`COMPILE+RUN OK`。证明标量数组生成代码合法且 `static_assert` 通过。

- [ ] **Step 6: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add tools/gen_registry.py tests/test_codegen.py
git commit -m "feat(array): codegen scalar + struct array members with std::extent_v drift guard"
```

---

## Task 4: 数组长度漂移测试(drift.sh 扩展)

**Files:**
- Modify: `tests/test_drift.sh`
- Modify: `tests/drift_schema.json`(加数组 member)
- Modify: `tests/drift_header.h`(加数组字段)

- [ ] **Step 1: 改 tests/drift_header.h(加数组字段)**

```c
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t timestamp;
    int feature_ids[8];
} DriftEvent;
```

- [ ] **Step 2: 改 tests/drift_schema.json(加数组 member)**

```json
{
  "deviceCtxType": "sv::DeviceCtx",
  "structs": [{"name": "DriftEvent", "members": [
      {"field": "timestamp", "as": "time", "type": "uint64", "fmt": "%llu"},
      {"field": "feature_ids", "as": "feat_ids", "type": "int", "fmt": "%d",
       "array": {"count": 8, "sep": "-"}}
  ]}]
}
```

- [ ] **Step 3: 改 tests/test_drift.sh —— Step B 的漂移改成「缩数组长度」**

把脚本里 Step B(原改名 `timestamp`→`renamed`)改为缩数组长度 + 改名两步都测。替换 Step B 段为:

```bash
echo "=== Step B: shrink array length -> compile MUST fail (static_assert) ==="
cat > tests/drift_header.h <<'EOF'
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t timestamp;
    int feature_ids[4];
} DriftEvent;
EOF
if g++ -std=c++17 -Iinclude -Ithird_party tests/drift_main.cpp src/*.cpp -o /tmp/drift_bad 2>/dev/null; then
  echo "ERROR: shrink [8]->[4] should have failed the build (static_assert missed)"
  exit 1
else
  echo "array length drift caught at compile (expected)"
fi

echo "=== Step C: rename field -> compile MUST fail ==="
cat > tests/drift_header.h <<'EOF'
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t renamed;
    int feature_ids[8];
} DriftEvent;
EOF
if g++ -std=c++17 -Iinclude -Ithird_party tests/drift_main.cpp src/*.cpp -o /tmp/drift_bad2 2>/dev/null; then
  echo "ERROR: rename should have failed the build"
  exit 1
else
  echo "field rename drift caught at compile (expected)"
fi

# Restore baseline
cat > tests/drift_header.h <<'EOF'
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t timestamp;
    int feature_ids[8];
} DriftEvent;
EOF
echo "ALL OK"
```

注意:`drift_main.cpp`(脚本生成的)调 `registerStructViewNames`,不需改 —— 它只验编译成功/失败。但 drift_main.cpp 里 `r.lookup("time")` 仍有效(time 字段还在)。无需改 drift_main.cpp 的生成逻辑(脚本里的 heredoc 保持原样)。

- [ ] **Step 4: 运行**

Run: `cd /Users/lpf2026/MyCode/struct_view && ./tests/test_drift.sh`
Expected: 打印 `baseline OK`、`array length drift caught at compile (expected)`、`field rename drift caught at compile (expected)`、`ALL OK`,exit 0。

- [ ] **Step 5: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add tests/test_drift.sh tests/drift_schema.json tests/drift_header.h
git commit -m "test(array): drift detection covers array length shrink (std::extent_v)"
```

---

## Task 5: Validator —— 结构体数组纳入存在性 + 环检测

**Files:**
- Modify: `src/Validator.cpp`
- Test: `tests/test_validator.cpp`

- [ ] **Step 1: 写失败测试(追加到 tests/test_validator.cpp 末尾)**

```cpp
TEST_CASE("Validator: struct array subRecipe must exist as a recipe") {
    auto ast = parse(R"({"recipes":[{"name":"main","template":"${boxes}"}]})");
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "box", 4, "|");   // "box" 子配方不存在
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    REQUIRE_FALSE(errs.empty());
    bool mentions = false;
    for (auto& e : errs) if (e.message.find("box") != std::string::npos) mentions = true;
    CHECK(mentions);
}

TEST_CASE("Validator: struct array valid subRecipe passes clean") {
    auto ast = parse(R"({"recipes":[
        {"name":"main","template":"[${boxes}]"},
        {"name":"box","template":"${x}"}]})");
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "box", 4, "|");
    reg.registerProvider("x", sv::makeProvider([](const void*, const sv::DeviceCtx&){return std::string("1");}));
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    CHECK(errs.empty());
}

TEST_CASE("Validator: detects cyclic struct-array dependency") {
    // main -> boxes(array, subRecipe "box") -> box recipe refs arr2(array, subRecipe "main") => cycle
    auto ast = parse(R"({"recipes":[
        {"name":"main","template":"${boxes}"},
        {"name":"box","template":"${arr2}"}]})");
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "box", 4, "|");
    reg.registerStructArray("arr2",
        sv::IndexedNavigator([](const void*, std::size_t) -> const void* { return nullptr; }),
        "main", 2, "|");
    sv::ConnectorLib lib;
    auto errs = sv::Validator::validate(ast, reg, lib);
    bool foundCycle = false;
    for (auto& e : errs) if (e.message.find("cycle") != std::string::npos) foundCycle = true;
    CHECK(foundCycle);
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | head -5 && ctest --test-dir build -R test_validator --output-on-failure 2>&1 | tail -15`
Expected: FAIL —— 结构体数组引用 `${boxes}` 被当成 unknown name(check 1 不认识它),或环检测漏掉结构体数组。

- [ ] **Step 3: 修改 src/Validator.cpp**

**3a. 加 isStructArray / findArrayDecl helper**(在现有 `isStructBlock`/`findDecl` 附近,`namespace { }` 内):

```cpp
bool isStructArray(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structArrayDecls()) if (d.first == s) return true;
    return false;
}
```

**3b. 修改 check 1**(「each referenced name resolves」):把 `if (!names.lookup(seg.text) && !connectors.get(seg.text))` 加上结构体数组的合法判断:

```cpp
    // 1. each referenced name resolves to a provider, connector, struct block, or struct array
    for (const auto& r : ast.recipes) {
        for (const auto& seg : r.segments) {
            if (!seg.isRef) continue;
            if (!names.lookup(seg.text) && !connectors.get(seg.text)
                && !isStructBlock(names, seg.text) && !isStructArray(names, seg.text)) {
                errs.push_back({r.name, "unknown name: " + seg.text, 0});
            }
        }
    }
```
(注意:现有代码可能已经把 `isStructBlock` 加进了 check 1 —— 查当前代码;若已有,只需再加 `&& !isStructArray(...)`。)

**3c. 加 check 2b**(结构体数组 subRecipe 存在性,在现有 check 2「struct block subRecipe exists」之后):

```cpp
    // 2b. each struct array's subRecipe exists as a recipe
    for (auto& d : names.structArrayDecls()) {
        if (!byName.count(d.second.subRecipeName)) {
            errs.push_back({d.first, "struct array subRecipe not found: " + d.second.subRecipeName, 0});
        }
    }
```

**3d. 修改 hasCycle**:把结构体数组纳入依赖图。当前 `dfs(blockName)` 只遍历 `structDecls()`。需要让结构体数组也参与:一个结构体数组的 subRecipe 引用的 struct block / struct array 都是它的依赖。

最简改法:在 `hasCycle` 的 dfs 里,「找这个 block 的 subRecipe」时,同时查 structDecls 和 structArrayDecls;「seg.text 是 struct block/struct array」时递归。重写 `dfs` 的 subRecipe 查找 + 引用判断:

```cpp
static bool hasCycle(const NameRegistry& names,
                     const std::unordered_map<std::string, const RecipeAst*>& byName) {
    std::unordered_set<std::string> visiting, visited;
    // 查一个 name(可能是 struct block 或 struct array)的 subRecipe 名
    auto subRecipeOf = [&](const std::string& nm) -> std::string {
        for (auto& d : names.structDecls()) if (d.first == nm) return d.second.subRecipeName;
        for (auto& d : names.structArrayDecls()) if (d.first == nm) return d.second.subRecipeName;
        return "";
    };
    auto isDepNode = [&](const std::string& nm) -> bool {
        return isStructBlock(names, nm) || isStructArray(names, nm);
    };
    std::function<bool(const std::string&)> dfs = [&](const std::string& nm) -> bool {
        if (visiting.count(nm)) return true;
        if (visited.count(nm)) return false;
        visiting.insert(nm);
        std::string sub = subRecipeOf(nm);
        if (!sub.empty()) {
            auto it = byName.find(sub);
            if (it != byName.end()) {
                for (const auto& seg : it->second->segments) {
                    if (seg.isRef && isDepNode(seg.text)) {
                        if (dfs(seg.text)) { visiting.erase(nm); visited.insert(nm); return true; }
                    }
                }
            }
        }
        visiting.erase(nm);
        visited.insert(nm);
        return false;
    };
    for (auto& d : names.structDecls())     if (dfs(d.first)) return true;
    for (auto& d : names.structArrayDecls()) if (dfs(d.first)) return true;
    return false;
}
```

- [ ] **Step 4: 构建并运行**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_validator --output-on-failure && ctest --test-dir build --output-on-failure 2>&1 | tail -4`
Expected: test_validator PASS(含 3 新 case)。全量无回归。

- [ ] **Step 5: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add src/Validator.cpp tests/test_validator.cpp
git commit -m "feat(array): Validator covers struct arrays (existence + cycle detection)"
```

---

## Task 6: Builder —— 标量数组 + 结构体数组编译(Route B)

**Files:**
- Modify: `src/Builder.cpp`
- Test: `tests/test_builder.cpp`

- [ ] **Step 1: 写失败测试(追加到 tests/test_builder.cpp 末尾)**

```cpp
namespace {
struct ArrScalarEv { int ids[8]; };
struct ArrBox { int x, y; };
struct ArrStructEv { ArrBox boxes[4]; };
struct ArrNestedBox { int x, y; int tags[2]; };
struct ArrNestedEv { ArrNestedBox boxes[4]; };
}

TEST_CASE("Builder: scalar array compiles to looping provider") {
    // 用 codegen 风格手搓一个标量数组 provider 注册,验编译后输出
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"r","template":"${ids}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerProvider("ids", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {
            const ArrScalarEv* s = static_cast<const ArrScalarEv*>(p);
            std::string out; char b[16];
            for (std::size_t i = 0; i < 8; ++i) { if (i) out += "-"; std::snprintf(b,sizeof(b),"%d",s->ids[i]); out += b; }
            return out;
        }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("r");
    ArrScalarEv e{{11,22,33,0,0,0,0,0}};
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "11-22-33-0-0-0-0-0");
}

TEST_CASE("Builder: struct array binds sub-recipe per element") {
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"main","template":"[${boxes}]"},
        {"name":"box","template":"${x},${y}"}]})").ast;
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const ArrStructEv* s = static_cast<const ArrStructEv*>(p); return &s->boxes[i];
        }), "box", 4, "|");
    reg.registerProvider("x", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->x); return buf;
    }));
    reg.registerProvider("y", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrBox* b = static_cast<const ArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->y); return buf;
    }));
    sv::ConnectorLib lib;
    lib.add("comma", ",");
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("main");
    ArrStructEv e{ {{{1,2},{3,4},{5,6},{7,8}}} };
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "[1,2|3,4|5,6|7,8]");
}

TEST_CASE("Builder: struct array with array field in element (nested combo, §9a)") {
    // boxes[4],每个 box 有 tags[2] → "x,y(tags0-tags1)" 用 | 连
    auto ast = sv::ConfigLoader::parse(R"({"recipes":[
        {"name":"main","template":"${boxes}"},
        {"name":"box","template":"${x}(${tags})"}]})").ast;
    sv::NameRegistry reg;
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const ArrNestedEv* s = static_cast<const ArrNestedEv*>(p); return &s->boxes[i];
        }), "box", 4, "|");
    reg.registerProvider("x", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const ArrNestedBox* b = static_cast<const ArrNestedBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->x); return buf;
    }));
    reg.registerProvider("tags", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {
            const ArrNestedBox* b = static_cast<const ArrNestedBox*>(p);
            std::string out; char buf[16];
            for (std::size_t i = 0; i < 2; ++i) { if (i) out += "-"; std::snprintf(buf,sizeof(buf),"%d",b->tags[i]); out += buf; }
            return out;
        }));
    sv::ConnectorLib lib;
    auto result = sv::Builder::compile(ast, reg, lib);
    REQUIRE(result.ok);
    auto snap = result.recipes.at("main");
    ArrNestedEv e{ {{{1,2,{7,8}},{3,4,{9,10}},{5,6,{11,12}},{7,8,{13,14}}}} };
    sv::DeviceCtx dummy;
    std::string out;
    for (auto& s : snap->steps) out += s.provider->get(&e, dummy);
    CHECK(out == "1(7-8)|3(9-10)|5(11-12)|7(13-14)");
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_builder --output-on-failure 2>&1 | tail -15`
Expected: FAIL —— 结构体数组 `${boxes}` 在 Builder 里未识别(走 defensive error → ok=false),或编译成 nullptr。

- [ ] **Step 3: 修改 src/Builder.cpp**

**3a. 加 isStructArray / findArrayDecl helper**(在现有 `isStructBlock`/`findDecl` 之后,`namespace { }` 内):

```cpp
bool isStructArray(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structArrayDecls()) if (d.first == s) return true;
    return false;
}
const StructArrayDecl* findArrayDecl(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structArrayDecls()) if (d.first == s) return &d.second;
    return nullptr;
}
```

**3b. Pass 1 加 pendingArray**(在现有 `struct PendingBind {...}` 定义之后,加一个并列结构;在 `std::vector<PendingBind> pending;` 之后加 `std::vector<PendingBind> pendingArray;` —— 可复用 PendingBind 结构,它有 rb/stepIdx/blockName 三字段,结构体数组也够用):

```cpp
    struct PendingBind {
        std::shared_ptr<RecipeB> rb;
        std::size_t stepIdx;
        std::string blockName;
    };
    std::unordered_map<std::string, std::shared_ptr<RecipeB>> byName;
    std::vector<PendingBind> pending;       // 单结构体块
    std::vector<PendingBind> pendingArray;  // 结构体数组(spec §4.4)
```

**3c. Pass 1 分派**(在现有 `else if (isStructBlock(names, seg.text))` 分支之后,加 `else if (isStructArray(...))` 分支):

```cpp
                } else if (isStructArray(names, seg.text)) {
                    pendingArray.push_back({rb, rb->steps.size(), seg.text});
                    // step.provider left nullptr; fixed in Pass 2.
                } else if (auto lit = connectors.get(seg.text)) {
```

(注意:标量数组不需要特殊处理 —— 它是 `names.lookup` 命中的普通 provider,走现有 `if (const ValueProvider* vp = names.lookup(seg.text))` 分支即可。)

**3d. Pass 2 加结构体数组绑定**(在现有 `for (auto& p : pending)` 循环之后,加并列循环):

```cpp
    // Pass 2b: instantiate recipe-private ArrayStructBlockProviders (struct arrays).
    for (auto& pa : pendingArray) {
        const StructArrayDecl* decl = findArrayDecl(names, pa.blockName);
        if (!decl) {
            r.errors.push_back({pa.rb->name, "unknown struct array: " + pa.blockName, 0});
            continue;
        }
        auto it = byName.find(decl->subRecipeName);
        if (it == byName.end()) {
            r.errors.push_back({pa.rb->name, "subRecipe missing at bind: " + decl->subRecipeName, 0});
            continue;
        }
        auto asbp = std::make_unique<ArrayStructBlockProvider>(
            decl->nav, it->second, decl->count, decl->sep);
        pa.rb->steps[pa.stepIdx].provider = asbp.get();
        pa.rb->ownedProviders.push_back(std::move(asbp));   // 配方内私有(RCU §3.4a)
    }
```

- [ ] **Step 4: 构建并运行**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_builder --output-on-failure && ctest --test-dir build --output-on-failure 2>&1 | tail -4`
Expected: test_builder PASS(含 3 新 case)。全量无回归。

- [ ] **Step 5: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add src/Builder.cpp tests/test_builder.cpp
git commit -m "feat(array): Builder compiles scalar + struct arrays (recipe-private, RCU)"
```

---

## Task 7: BuilderA —— Route A 对等(struct 数组塞 FieldBinding)

**Files:**
- Modify: `src/BuilderA.cpp`
- Test: `tests/test_route_a.cpp`(扩展)

- [ ] **Step 1: 写失败测试(修改 tests/test_route_a.cpp 的 TEST_CASE,加数组配方)**

把现有 `TEST_CASE("Route A: renderA matches Route B output")` 的 cfg 改为含数组的配方,并加结构体数组注册。完整替换该 TEST_CASE:

```cpp
#include <struct_view/ValueProvider.hpp>
#include <cstdio>

namespace {
struct RouteArrBox { int x, y; int tags[2]; };
struct RouteArrEv {
    uint64_t timestamp;
    RouteArrBox boxes[4];
};
struct RouteDeviceCtx : sv::DeviceCtx { std::string cameraId() const { return "CAM001"; } };

static std::string routeArrCfg() {
    return R"({"recipes":[
      {"name":"alarm_line","template":"${camera}:${time}|${boxes}"},
      {"name":"box","template":"${x},${y}(${tags})"}]})";
}
}

TEST_CASE("Route A: renderA matches Route B output (with arrays)") {
    sv::NameRegistry reg;
    // time 字段
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const RouteArrEv* e = static_cast<const RouteArrEv*>(p);
        char b[32]; std::snprintf(b,sizeof(b),"%llu",(unsigned long long)e->timestamp); return b;
    }));
    // camera 设备
    reg.registerProvider("camera", sv::makeProvider([](const void*, const sv::DeviceCtx& base) -> std::string {
        return static_cast<const RouteDeviceCtx&>(base).cameraId();
    }));
    // boxes 结构体数组
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const RouteArrEv* e = static_cast<const RouteArrEv*>(p); return &e->boxes[i];
        }), "box", 4, "|");
    // box 子配方的 x / y / tags(标量数组)
    reg.registerProvider("x", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const RouteArrBox* b = static_cast<const RouteArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->x); return buf;
    }));
    reg.registerProvider("y", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const RouteArrBox* b = static_cast<const RouteArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->y); return buf;
    }));
    reg.registerProvider("tags", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {
            const RouteArrBox* b = static_cast<const RouteArrBox*>(p);
            std::string out; char buf[16];
            for (std::size_t i = 0; i < 2; ++i) { if (i) out += "-"; std::snprintf(buf,sizeof(buf),"%d",b->tags[i]); out += buf; }
            return out;
        }));
    sv::ConnectorLib lib;
    sv::Engine engine(reg, lib);
    std::string cfg = routeArrCfg();
    REQUIRE(engine.loadConfig(cfg).ok);
    REQUIRE(engine.loadConfigA(cfg).ok);

    RouteArrEv ev{1717171717, {{{1,2,{7,8}},{3,4,{9,10}},{5,6,{11,12}},{7,8,{13,14}}}}};
    RouteDeviceCtx ctx;
    std::string b = engine.render("alarm_line", &ev, ctx);
    std::string a = engine.renderA("alarm_line", &ev, ctx);
    CHECK(a == b);
    CHECK(a == "CAM001:1717171717|1,2(7-8)|3,4(9-10)|5,6(11-12)|7,8(13-14)");
}
```

- [ ] **Step 2: 运行验证失败**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_route_a --output-on-failure 2>&1 | tail -15`
Expected: FAIL —— Route A 的 `${boxes}` 在 BuilderA 里未识别(走 defensive error → loadConfigA ok=false),或 renderA 输出与 render 不符。

- [ ] **Step 3: 修改 src/BuilderA.cpp**

Route A 的结构体数组处理与 Route B 几乎一样,但创建的 `ArrayStructBlockProvider` 塞进 `FieldBinding`(不是 `SubRecipeBinding`,spec §5)。

**3a. 加 isStructArray / findArrayDecl helper**(与 Builder.cpp 同,在 BuilderA 的 `namespace { }` 内加):

```cpp
bool isStructArray(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structArrayDecls()) if (d.first == s) return true;
    return false;
}
const StructArrayDecl* findArrayDecl(const NameRegistry& names, const std::string& s) {
    for (const auto& d : names.structArrayDecls()) if (d.first == s) return &d.second;
    return nullptr;
}
```

**3b. Pass 1 加 pendingArray**(同 Builder,复用 PendingBind 结构):

```cpp
    struct PendingBind {
        std::shared_ptr<RecipeA> ra;
        std::size_t stepIdx;
        std::string blockName;
    };
    std::unordered_map<std::string, std::shared_ptr<RecipeA>> byName;
    std::vector<PendingBind> pending;       // 单结构体块(SubRecipeBinding)
    std::vector<PendingBind> pendingArray;  // 结构体数组(塞 FieldBinding,spec §5)
```

**3c. Pass 1 分派**:Route A 现有对 `${name}` 的处理是 `if (const ValueProvider* vp = names.lookup(seg.text))` → FieldBinding;`else if (isStructBlock(...))` → pending(SubRecipeBinding)。在 `isStructBlock` 分支之后加 `isStructArray` 分支:

```cpp
                } else if (isStructArray(names, seg.text)) {
                    pendingArray.push_back({ra, ra->steps.size(), seg.text});
                    step.kind = StepKind::Field;                      // 占位,Pass 2 替换
                    step.binding = FieldBinding{nullptr};
                } else if (auto lit = connectors.get(seg.text)) {
```

**3d. Pass 2 加结构体数组绑定**(在现有单结构体块 Pass 2 循环之后):

```cpp
    // Pass 2b: struct arrays — create ArrayStructBlockProvider, put in FieldBinding
    // (Route A does NOT inline array iteration; it reuses the Route B provider via a
    // single virtual call. spec §5.)
    for (auto& pa : pendingArray) {
        const StructArrayDecl* decl = findArrayDecl(names, pa.blockName);
        if (!decl) {
            r.errors.push_back({pa.ra->name, "unknown struct array: " + pa.blockName, 0});
            continue;
        }
        auto it = byName.find(decl->subRecipeName);
        if (it == byName.end()) {
            r.errors.push_back({pa.ra->name, "subRecipe missing at bind: " + decl->subRecipeName, 0});
            continue;
        }
        auto asbp = std::make_unique<ArrayStructBlockProvider>(
            decl->nav, it->second, decl->count, decl->sep);
        StepA step;
        step.kind = StepKind::Field;
        step.binding = FieldBinding{asbp.get()};   // ArrayStructBlockProvider is-a ValueProvider
        pa.ra->steps[pa.stepIdx] = std::move(step);
        pa.ra->ownedProviders.push_back(std::move(asbp));   // 配方内私有
    }
```

注意:Route A 的 `RecipeA` 当前**没有** `ownedProviders` 字段(它的 ConnectorBinding 持字面量、SubRecipeBinding 持 shared_ptr,原本不需要 ownedProviders)。现在结构体数组要持有 `ArrayStructBlockProvider`,需要在 `RecipeA` 加 `ownedProviders`。

**3e. 给 RecipeA 加 ownedProviders(修改 include/struct_view/Recipe.hpp)**:

在 `struct RecipeA { ... }` 内(`steps` 之后)加:
```cpp
    // Owns ArrayStructBlockProvider instances for struct-array steps (Route A
    // reuses the Route B provider via FieldBinding, spec §5). Field/Device
    // bindings hold raw ptrs into NameRegistry-owned providers (longer-lived).
    std::vector<std::unique_ptr<ValueProvider>> ownedProviders;
```
并在 Recipe.hpp 顶部 includes 加 `#include <memory>`(若已有则跳过)和确认 `ValueProvider` 可见(ValueProvider.hpp 已 include,`unique_ptr<ValueProvider>` 需 ValueProvider 完整定义 —— 已满足)。

- [ ] **Step 4: 构建并运行**

Run: `cmake --build build 2>&1 | tail -5 && ctest --test-dir build -R test_route_a --output-on-failure && ctest --test-dir build --output-on-failure 2>&1 | tail -4`
Expected: test_route_a PASS(Route A 与 Route B 输出一致)。全量无回归。

- [ ] **Step 5: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add include/struct_view/Recipe.hpp src/BuilderA.cpp tests/test_route_a.cpp
git commit -m "feat(array): BuilderA Route A parity — struct array via FieldBinding (spec §5)"
```

---

## Task 8: Engine + 并发热加载(数组场景 TSan)

**Files:**
- Modify: `tests/test_engine.cpp`(加含数组配方 case)
- Modify: `tests/test_hot_reload.cpp`(配方加结构体数组)

- [ ] **Step 1: 写 Engine 测试(追加到 tests/test_engine.cpp 末尾)**

```cpp
#include <cstdio>

namespace {
struct EngArrBox { int x, y; };
struct EngArrEv { uint64_t ts; EngArrBox boxes[4]; };
}

TEST_CASE("Engine: render with struct array end-to-end") {
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const EngArrEv* e = static_cast<const EngArrEv*>(p);
        char b[32]; std::snprintf(b,sizeof(b),"%llu",(unsigned long long)e->ts); return b;
    }));
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const EngArrEv* e = static_cast<const EngArrEv*>(p); return &e->boxes[i];
        }), "box", 4, "|");
    reg.registerProvider("x", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const EngArrBox* b = static_cast<const EngArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->x); return buf;
    }));
    reg.registerProvider("y", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const EngArrBox* b = static_cast<const EngArrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->y); return buf;
    }));
    sv::ConnectorLib lib;
    lib.add("comma", ",");
    sv::Engine engine(reg, lib);
    REQUIRE(engine.loadConfig(R"({"recipes":[
        {"name":"line","template":"${time}|${boxes}"},
        {"name":"box","template":"${x}${comma}${y}"}]})").ok);
    EngArrEv e{42, {{{1,2},{3,4},{5,6},{7,8}}}};
    MyDeviceCtx ctx;
    CHECK(engine.render("line", &e, ctx) == "42|1,2|3,4|5,6|7,8");
}
```

- [ ] **Step 2: 修改 tests/test_hot_reload.cpp —— 配方加结构体数组**

把现有 `cfg(sep)` 函数和测试里的配方改为含结构体数组。替换 `cfg` 函数及 `Event` 结构体为含数组的版本:

```cpp
namespace {
struct Event { uint64_t timestamp; };
struct HrBox { int v; };
struct HrEv { uint64_t timestamp; HrBox boxes[4]; };
struct MyDeviceCtx : sv::DeviceCtx { std::string cameraId() const { return "C"; } };

static std::string cfg(const std::string& sep) {
    return std::string("{\"recipes\":[{\"name\":\"r\",\"template\":\"${time}") + sep +
           "${boxes}\"},{\"name\":\"box\",\"template\":\"${v}\"}]}";
}
}
```

并在 TEST_CASE 里注册 `boxes` 结构体数组 + `v` provider,把 `Event e{99}` 换成 `HrEv e{99, {{{1},{2},{3},{4}}} }`,把 `engine.render("r", &e, ctx)` 期望的最终串相应调整(末尾追加 `|1|2|3|4` 或按 sep)。具体:把注册段改为:

```cpp
    sv::NameRegistry reg;
    reg.registerProvider("time", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const HrEv* e = static_cast<const HrEv*>(p);
        char b[32]; std::snprintf(b, sizeof(b), "%llu", (unsigned long long)e->timestamp); return b;
    }));
    reg.registerStructArray("boxes",
        sv::IndexedNavigator([](const void* p, std::size_t i) -> const void* {
            const HrEv* e = static_cast<const HrEv*>(p); return &e->boxes[i];
        }), "box", 4, "|");
    reg.registerProvider("v", sv::makeProvider([](const void* p, const sv::DeviceCtx&) -> std::string {
        const HrBox* b = static_cast<const HrBox*>(p); char buf[16]; std::snprintf(buf,sizeof(buf),"%d",b->v); return buf;
    }));
```

最终断言:最后一次 loadConfig 是 `i=199`(奇)→ sep=`|`,所以 `engine.render("r", &e, ctx) == "99|1|2|3|4"`(time=99,sep=`|`,boxes=`1|2|3|4`,中间还有个 `|` 连接 time 和 boxes → `99|1|2|3|4`)。把末尾 `CHECK(engine.render("r", &e, ctx) == "99-99");` 改为 `CHECK(engine.render("r", &e, ctx) == "99|1|2|3|4");`。

- [ ] **Step 3: 构建 + 跑 plain + TSan**

```bash
cd /Users/lpf2026/MyCode/struct_view
cmake --build build 2>&1 | tail -5
ctest --test-dir build -R "test_engine|test_hot_reload" --output-on-failure
ctest --test-dir build --output-on-failure 2>&1 | tail -4
# TSan
cmake --build build_tsan -q 2>&1 | tail -2
ctest --test-dir build_tsan --output-on-failure 2>&1 | tail -4
./build_tsan/test_hot_reload 2>&1 | grep -c "ThreadSanitizer: data race"   # 期望 0
```
Expected: test_engine + test_hot_reload plain PASS;全量无回归;TSan 全量 PASS,0 data race。

- [ ] **Step 4: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add tests/test_engine.cpp tests/test_hot_reload.cpp
git commit -m "test(array): engine + hot-reload with struct arrays (TSan-clean)"
```

---

## Task 9: 端到端示例扩展(examples/security)

**Files:**
- Modify: `examples/security/event.h`
- Modify: `examples/security/schema.json`
- Regenerate: `examples/security/registry_gen.cpp`
- Modify: `examples/security/recipes.json`
- Modify: `examples/security/main.cpp`
- Modify: `tests/test_example_e2e.cpp`

- [ ] **Step 1: 改 examples/security/event.h(Box 加 tags,Event 加数组)**

```c
#pragma once
#include <stdint.h>
typedef struct { char name[32]; int age; } PersonInfo;
typedef struct { int x, y, w, h; int tags[2]; } Box;   // Box 加 tags[2]
typedef struct {
    uint64_t timestamp;
    PersonInfo* person;
    Box rect;                  // 单结构体块(rect 也有 tags)
    int feature_ids[8];        // 标量数组(新)
    Box boxes[4];              // 结构体数组(新,每个 box 含 tags → 嵌套组合 §9a)
} Event;
```

- [ ] **Step 2: 改 examples/security/schema.json**

```json
{
  "deviceCtxType": "MyDeviceCtx",
  "structs": [
    {"name": "Event", "members": [
        {"field": "timestamp", "as": "time", "type": "uint64", "fmt": "%llu"},
        {"block": "person", "ptr": true,  "subRecipe": "person"},
        {"block": "rect",   "ptr": false, "subRecipe": "box"},
        {"field": "feature_ids", "as": "feat_ids", "type": "int", "fmt": "%d",
         "array": {"count": 8, "sep": "-"}},
        {"block": "boxes", "array": {"subRecipe": "box", "count": 4, "sep": "|"}}
    ]},
    {"name": "PersonInfo", "members": [
        {"field": "name", "as": "name", "type": "cstr", "fmt": "%s"},
        {"field": "age",  "as": "age",  "type": "int",   "fmt": "%d"}
    ]},
    {"name": "Box", "members": [
        {"field": "x", "type": "int", "fmt": "%d"},
        {"field": "y", "type": "int", "fmt": "%d"},
        {"field": "w", "type": "int", "fmt": "%d"},
        {"field": "h", "type": "int", "fmt": "%d"},
        {"field": "tags", "type": "int", "fmt": "%d", "array": {"count": 2, "sep": "-"}}
    ]}
  ],
  "device": [{"getter": "cameraId", "as": "camera"}]
}
```

- [ ] **Step 3: 重新生成 registry_gen.cpp**

Run: `cd /Users/lpf2026/MyCode/struct_view && python3 tools/gen_registry.py examples/security/schema.json examples/security/registry_gen.cpp`
Expected: 文件更新,含 `registerStructArray("boxes"...)` + `feat_ids` 循环 + `tags` 循环。抽查:`grep -n "registerStructArray\|feat_ids\|extent_v" examples/security/registry_gen.cpp`。

- [ ] **Step 4: 改 examples/security/recipes.json**

```json
{
  "recipes": [
    { "name": "alarm_line", "template": "${camera}:${time}|${person}@${rect}|${feat_ids}|${boxes}" },
    { "name": "person",     "template": "${name}-${age}" },
    { "name": "box",        "template": "${x},${y},${w},${h}(${tags})" }
  ]
}
```

- [ ] **Step 5: 改 examples/security/main.cpp(数据含数组)**

把构造 `Event ev` 的数据改为含 `feature_ids` + `boxes`(每个 box 含 tags)。替换 `main.cpp` 里的数据构造段:

```cpp
    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{
        1717171717,
        &p,
        {100, 200, 300, 400, {7, 8}},                                   // rect (含 tags 7-8)
        {11, 22, 33, 0, 0, 0, 0, 0},                                    // feature_ids
        {{{100,200,300,400,{1,2}}, {110,210,310,410,{3,4}},             // boxes[4] 各含 tags
          {0,0,0,0,{0,0}}, {0,0,0,0,{0,0}}}}
    };
```

- [ ] **Step 6: 改 tests/test_example_e2e.cpp(断言含数组的精确串)**

把现有 TEST_CASE 的数据 + 断言改为含数组版本。完整替换数据构造与 CHECK:

```cpp
    PersonInfo p; std::strncpy(p.name, "Alice", 32); p.age = 30;
    Event ev{
        1717171717, &p,
        {100, 200, 300, 400, {7, 8}},
        {11, 22, 33, 0, 0, 0, 0, 0},
        {{{100,200,300,400,{1,2}}, {110,210,310,410,{3,4}},
          {0,0,0,0,{0,0}}, {0,0,0,0,{0,0}}}}
    };
    MyDeviceCtx ctx;
    CHECK(engine.render("alarm_line", &ev, ctx) ==
        "CAM001:1717171717|Alice-30@100,200,300,400(7-8)|11-22-33-0-0-0-0-0|100,200,300,400(1-2)|110,210,310,410(3-4)|0,0,0,0(0-0)|0,0,0,0(0-0)");
```

- [ ] **Step 7: 构建 + 跑全量 + 示例**

```bash
cd /Users/lpf2026/MyCode/struct_view
cmake -B build -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -2
cmake --build build 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -5
./build/security_example
```
Expected: 全量测试 PASS;示例打印 `CAM001:1717171717|Alice-30@100,200,300,400(7-8)|11-22-33-0-0-0-0-0|100,200,300,400(1-2)|110,210,310,410(3-4)|0,0,0,0(0-0)|0,0,0,0(0-0)`。

- [ ] **Step 8: 提交**

```bash
cd /Users/lpf2026/MyCode/struct_view
git add examples/security/event.h examples/security/schema.json examples/security/registry_gen.cpp examples/security/recipes.json examples/security/main.cpp tests/test_example_e2e.cpp
git commit -m "feat(array): end-to-end security example with scalar + struct + nested arrays"
```

---

## Task 10: 最终全量验证(plain + TSan + codegen + drift)

**Files:** 无(仅验证)

- [ ] **Step 1: 全量 plain**

```bash
cd /Users/lpf2026/MyCode/struct_view
cmake -B build -DCMAKE_BUILD_TYPE=Debug -q 2>&1 | tail -1
cmake --build build -q 2>&1 | tail -1
ctest --test-dir build --output-on-failure 2>&1 | tail -5
```
Expected: 100% tests passed(数量比扩展前多,含所有新 case)。

- [ ] **Step 2: TSan**

```bash
cmake --build build_tsan -q 2>&1 | tail -1
ctest --test-dir build_tsan --output-on-failure 2>&1 | tail -5
./build_tsan/test_hot_reload 2>&1 | grep -c "ThreadSanitizer: data race"
```
Expected: TSan 全量 PASS,0 data race。

- [ ] **Step 3: codegen + drift**

```bash
python3 tests/test_codegen.py 2>&1 | tail -8
./tests/test_drift.sh 2>&1 | tail -5
./build/security_example
```
Expected: codegen 全 PASS;drift 打印 `array length drift caught` + `field rename drift caught` + `ALL OK`;示例输出精确串。

- [ ] **Step 4: 推送(可选)**

```bash
git push
```

---

## Self-Review (plan author)

**1. Spec coverage:**
- §2 schema 标量数组/结构体数组 → Task 3(codegen 生成)+ Task 9(示例 schema)。✓
- §3.1 标量数组循环 LambdaProvider + std::extent_v → Task 3。✓
- §3.2 结构体数组 registerStructArray + std::extent_v → Task 3。✓
- §4.1 IndexedNavigator → Task 1。✓
- §4.2 ArrayStructBlockProvider → Task 1。✓
- §4.3 NameRegistry StructArrayDecl/registerStructArray/structArrayDecls → Task 2。✓
- §4.4 Builder Pass 1 pendingArray + Pass 2 → Task 6。✓
- §4.5 Validator 存在性 + 环检测 → Task 5。✓
- §4.6 RCU(不变,TSan 验)→ Task 8。✓
- §5 Route A 对等(FieldBinding)→ Task 7。✓
- §6 测试 7 层 → Task 1/2/3/4/5/6/7/8/9。✓
- §7 端到端示例(含 tags 嵌套)→ Task 9。✓
- §9a 结构体数组里字段含数组(嵌套组合)→ Task 6 第 3 case + Task 9(rect/boxes 都含 tags)。✓
- §9 YAGNI(不做二维/动态/i/Route A binding)→ 遵守。✓

**2. Placeholder scan:** 无 TBD/TODO/「add error handling」。每个 step 有完整代码或完整命令。Task 1 的测试里有一段「重写(干净版)」说明,但给出了完整可用代码,非占位。✓

**3. Type consistency:**
- `IndexedNavigator(const void* (*)(const void*, std::size_t))`,`navigate(parent, i)` —— Task 1/2/3/6/7/8 全一致。✓
- `ArrayStructBlockProvider(nav, sub, count, sep)` —— Task 1/6/7 一致。✓
- `registerStructArray(name, nav, subRecipeName, count, sep)` + `structArrayDecls()` —— Task 2/3/5/6/7 一致。✓
- `StructArrayDecl{nav, subRecipeName, count, sep}` —— Task 2/5/6/7 一致。✓
- `findArrayDecl`/`isStructArray` helper —— Task 5/6/7 都定义了同名同签名(各自 namespace 内,不冲突)。✓
- Route A `RecipeA::ownedProviders` —— Task 7 Step 3e 新增,Task 7 Step 3d 使用。✓

无 gap。Plan 完整。
