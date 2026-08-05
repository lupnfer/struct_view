# 数组字段支持 —— 设计 spec

- 日期: 2026-08-05
- 状态: 已批准 (设计阶段)
- 关联: `docs/superpowers/specs/2026-08-04-struct-view-design.md`(主 spec,本文是其扩展)
- 关联实现: `tools/gen_registry.py`、`include/struct_view/ValueProvider.hpp`、`NameRegistry.hpp`、`Builder.cpp`

## 1. 目标与背景

### 1.1 要解决的问题

上游安防结构体常见**数组字段**。当前框架只支持字符数组(`char name[32]` 走 `cstr` 语义),不支持一般标量数组(`int ids[8]`)与结构体数组(`Box boxes[4]`)的逐元素遍历拼接。需要扩展,使这类数组能按元素遍历、用连接符拼成字符串(如 `feature_ids` → `11-22-33-0-0-0-0-0`,`boxes` → `100,200,300,400|110,210,310,410|...`)。

### 1.2 已确定的关键约束(设计决策)

| 维度 | 决定 |
|---|---|
| 范围 | 标量数组 + 结构体数组(固定长度)。**不含动态长度**(`T* + countField`),留 v2 |
| 数组连接符 | schema 内置 `sep`(与字段类型/格式器同类知识,放描述文件最自然) |
| 长度漂移防护 | `std::extent_v<decltype(s->arr)> >= count` + `static_assert`(C++17 原生类型 trait,比 `sizeof` 比更语义化,且能防指针误标成数组) |
| 实现方案 | 方案 1:标量数组生成循环 `LambdaProvider`(零新增运行时类);结构体数组新增 `IndexedNavigator` + `ArrayStructBlockProvider`(与现有 `Navigator`/`StructBlockProvider` 同构) |
| Route A 对等 | Route A 复用 `ArrayStructBlockProvider`(塞进 `FieldBinding`),**不专造 Route A 数组 binding**(YAGNI;基准已决断 Route B 为默认) |

### 1.3 现状核查(当前 codegen 对数组的行为)

`tools/gen_registry.py` 的 `field_line` 对标量字段生成 `snprintf(buf, fmt, cast s->field)`。

| 数组形态 | 现状 | 正确性 |
|---|---|---|
| `char name[32]` | `type: cstr` → `(const char*)s->name` + `%s` | ✅ 已支持。`char[]` 衰减为 `const char*`,`%s` 读到 `\0`。 |
| `int ids[8]` | `type: int` → `(int)s->ids` | ❌ 错误。`s->ids` 衰减为 `int*`,被强转 `int`(首元素地址截断),`%d` 输出垃圾。 |
| `Box boxes[4]` | `block` + `ptr:false` → `&s->boxes` | ❌ 错误。导航到整个数组首地址,子配方拿到 `Box(*)[4]`,无下标遍历。 |

## 2. schema 扩展

在现有 member(`field` 标量 / `block` 结构体块)基础上加 `array` 标志。两种数组共用 `array` 对象,靠有无 `block` 区分。

### 2.1 标量数组(`field` + `array`)

```json
{ "field": "feature_ids", "as": "feat_ids", "type": "int", "fmt": "%d",
  "array": { "count": 8, "sep": "-" } }
```
- `type`/`fmt` 复用现有标量类型系统(uint64/int/float/cstr),描述**单个元素**怎么格式化。
- `count` = 固定长度(编译期已知);`sep` = 元素间连接符(schema 内置,首元素前不加 —— join 语义)。

### 2.2 结构体数组(`block` + `array`)

```json
{ "block": "boxes", "array": { "subRecipe": "box", "count": 4, "sep": "|" } }
```
- `block` = 真实 C 数组成员名;`subRecipe` = 每个元素跑哪个子配方(与单结构体块的 `subRecipe` 同义)。
- **不带 `ptr`** —— 数组元素永远是 `&s->arr[i]`(取地址,无论元素类型)。比单结构体块简单(单块要区分指针/内嵌)。

## 3. codegen 生成 + `std::extent_v` 漂移防护

`gen_registry.py` 加两个分支。判定:`member` 里有 `"array"` 键 → 数组分支;其中 `"field" in m` → 标量数组,`"block" in m` → 结构体数组。

### 3.1 标量数组:循环 `LambdaProvider`(零新增运行时类)

复用 `makeProvider`,lambda 体里 `for` 循环 + `sep` 连接 + `std::extent_v` 断言:

```cpp
// codegen 为 {"field":"feature_ids","as":"feat_ids","type":"int","fmt":"%d","array":{"count":8,"sep":"-"}}
// 生成:
reg.registerProvider("feat_ids", sv::makeProvider(
    [](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* s = static_cast<const Event*>(p);
        static_assert(std::extent_v<decltype(s->feature_ids)> >= 8,
                      "struct_view: feature_ids length drift (schema count=8)");
        std::string out;
        char buf[64];
        for (std::size_t i = 0; i < 8; ++i) {
            if (i) out += "-";                       // sep(join 语义)
            std::snprintf(buf, sizeof(buf), "%d", (int)s->feature_ids[i]);
            out += buf;
        }
        return out;
    }));
```

要点:
- `std::extent_v<decltype(s->arr)>` 编译期取真实数组长度。上游 `[8]`→`[4]` → 4>=8 false → 编译失败,错误信息指明字段名 + schema count。
- **额外防误标**:若 schema 把指针成员(`int* ids`)误标成定长数组,`std::extent_v<int*>` = 0 → 0>=count 失败 → 编译报错(比 `sizeof` 比更安全,后者对指针得无意义正数不报错)。
- `if (i) out += sep` = join 语义(首元素前不加)。
- `count=0` → 循环不执行 → 返回空串(安全)。

### 3.2 结构体数组:`registerStructArray` 调用

```cpp
// codegen 为 {"block":"boxes","array":{"subRecipe":"box","count":4,"sep":"|"}}
// 生成:
reg.registerStructArray("boxes", sv::IndexedNavigator(
    [](const void* p, std::size_t i) -> const void* {
        const Event* s = static_cast<const Event*>(p);
        static_assert(std::extent_v<decltype(s->boxes)> >= 4,
                      "struct_view: boxes length drift (schema count=4)");
        return &s->boxes[i];
    }), "box", 4, "|");
```

要点:
- `IndexedNavigator` 是新增类(带下标导航器,见 §4.1)。
- 导航返回 `&s->boxes[i]`(元素地址)。
- `static_assert` 同样防护长度漂移。

### 3.3 codegen 改动点汇总

| 现有 | 新增 |
|---|---|
| `field_line`(标量) | `field_array_line`(标量数组,生成循环 lambda) |
| `block_line`(单结构体块) | `block_array_line`(结构体数组,生成 registerStructArray) |
| HEADER 含 `<cstdio>`/`<string>` | 加 `<type_traits>`(std::extent_v)、`<cstddef>`(std::size_t) |
| `gen()` 遍历 members 分 `field`/`block` | 加 `array` 判断 |

## 4. 运行时新增类 + 注册表/Builder/Validator 改动

### 4.1 新增 `IndexedNavigator`(带下标导航器)

与现有 `Navigator` 同构,多一个 `std::size_t i` 参数:

```cpp
// include/struct_view/ValueProvider.hpp(追加)
class IndexedNavigator {
    const void* (*fn_)(const void*, std::size_t);
public:
    explicit IndexedNavigator(const void* (*f)(const void*, std::size_t)) : fn_(f) {}
    const void* navigate(const void* parent, std::size_t i) const { return fn_(parent, i); }
};
```

### 4.2 新增 `ArrayStructBlockProvider`(结构体数组 provider)

与现有 `StructBlockProvider` 同构,多 `count_`/`sep_`,`get()` 循环:

```cpp
// include/struct_view/ValueProvider.hpp(追加)
class ArrayStructBlockProvider : public ValueProvider {
    IndexedNavigator nav_;
    std::shared_ptr<const RecipeB> sub_;   // 子配方(引用计数,RCU §3.4a Rule 1/3)
    std::size_t count_;
    std::string sep_;
public:
    ArrayStructBlockProvider(IndexedNavigator nav, std::shared_ptr<const RecipeB> sub,
                             std::size_t count, std::string sep)
        : nav_(std::move(nav)), sub_(std::move(sub)),
          count_(count), sep_(std::move(sep)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

// src/ValueProvider.cpp(追加)
std::string ArrayStructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    std::string out;
    for (std::size_t i = 0; i < count_; ++i) {
        if (i) out += sep_;                         // join 语义
        const void* child = nav_.navigate(structPtr, i);
        out += runRecipeB(*sub_, child, ctx);       // 每个元素跑子配方
    }
    return out;
}
```

要点:
- **标量数组不新增 provider 类** —— 它是普通 `LambdaProvider`(codegen 生成循环体),零新增运行时类。
- `ArrayStructBlockProvider` 持 `shared_ptr<const RecipeB>` → RCU 三规定照搬(配方内私有、编译期绑定、运行期只读)。
- `runRecipeB` 已存在,直接复用。

### 4.3 `NameRegistry` 改动

新增 `StructArrayDecl` + `registerStructArray` + `structArrayDecls()`:

```cpp
// include/struct_view/NameRegistry.hpp(追加)
struct StructArrayDecl {
    IndexedNavigator nav;
    std::string subRecipeName;
    std::size_t count;
    std::string sep;
};

class NameRegistry {
    // ... 现有 entries_ / structDecls_ ...
    std::vector<std::pair<std::string, StructArrayDecl>> structArrayDecls_;
public:
    // ... 现有 registerProvider / registerStruct / lookup / structDecls ...
    void registerStructArray(std::string name, IndexedNavigator nav,
                             std::string subRecipeName, std::size_t count, std::string sep);
    const std::vector<std::pair<std::string, StructArrayDecl>>& structArrayDecls() const;
};
```

要点:`StructArrayDecl` 只存声明,**不存 bound provider**(绑定配方内私有,与 `StructBlockDecl` 同模式)。`lookup()` 仍只返回标量字段/设备 provider。

### 4.4 `Builder` 改动(Pass 1 + Pass 2)

Pass 1 编译时,识别 `${name}` 引用是结构体数组(查 `structArrayDecls()`),加入 `pendingArray` 列表(step.provider 暂留 nullptr)。

Pass 2(与现有 struct-block 绑定并列):为每个 pending 创建配方内私有 `ArrayStructBlockProvider`:

```cpp
// src/Builder.cpp Pass 2(追加,与现有 pending struct-block 绑定并列)
for (auto& pa : pendingArray) {
    const StructArrayDecl* decl = findArrayDecl(names, pa.blockName);
    auto it = byName.find(decl->subRecipeName);
    auto asbp = std::make_unique<ArrayStructBlockProvider>(
        decl->nav, it->second, decl->count, decl->sep);
    pa.rb->steps[pa.stepIdx].provider = asbp.get();
    pa.rb->ownedProviders.push_back(std::move(asbp));   // 配方内私有
}
```

### 4.5 `Validator` 改动

三项检查扩展(在现有三项后追加):
- 结构体数组 name 引用的 `subRecipe` 存在(与单结构体块同检查)。
- 结构体数组 `subRecipe` 无循环依赖(DFS 把 `structArrayDecls()` 也纳入依赖图,与单结构体块同算法)。
- 标量数组引用无需校验 `subRecipe`(它是 provider,在 `entries_`,现有 check 1「name 存在」已覆盖)。

### 4.6 RCU 并发安全(不变)

数组 provider 同样遵循主 spec §3.4a 三规定:
- 标量数组 = `LambdaProvider`,配方内私有,不可变。
- 结构体数组 = `ArrayStructBlockProvider`,配方内私有,持 `shared_ptr` 子配方。
- Engine 稳定 store + `publishAll` 原子换 map。

热加载时旧配方(含其数组 provider + 子配方图)靠引用计数保活至在途 render 跑完。数组循环在单个 render 调用内完成,无跨线程共享可变状态 —— **不引入新的并发风险**。`test_hot_reload` 扩展为含结构体数组的配方,继续 TSan 验证。

## 5. Route A 对等处理

Route A(显式四类 Step)也支持数组,但**不专造 Route A 数组 binding**(YAGNI)。

**做法(已定):Route A 复用 `ArrayStructBlockProvider`。**
Route A 的 `FieldBinding` 持 `const ValueProvider*` —— 标量数组是 `LambdaProvider`、结构体数组是 `ArrayStructBlockProvider`,两者都是 `ValueProvider*`,直接塞进 `FieldBinding`。Route A 的 `runRecipeA` 对 `Field` case 调 `provider->get(structPtr, ctx)` —— 一次虚调用取整个数组字符串(数组循环在 provider 内部)。

**代价**:Route A 对数组元素**不内联**(经 `ArrayStructBlockProvider` 虚调用 + 内部循环),失去 Route A「无虚调用」优势。但 Route A 本是基准参照(基准已决断 Route B 为默认),数组场景下 Route A 退化为与 Route B 行为一致,parity 测试仍成立。基准决断文档注明「数组场景 Route A 无内联优势」。

**BuilderA 改动**:Pass 1 识别结构体数组引用时,与 Route B 一样走 `pendingArray`,Pass 2 创建 `ArrayStructBlockProvider` 塞进 `FieldBinding`(而非 `SubRecipeBinding`)。标量数组直接 `names.lookup` 命中(它在 `entries_`)。

## 6. 测试策略(TDD,逐层)

| 层 | 测试 | 验证 |
|---|---|---|
| codegen | `tests/test_codegen.py` 加 2 case | 标量数组生成循环 `for` + `static_assert(std::extent_v...)`;结构体数组生成 `registerStructArray` + `&s->arr[i]` + `static_assert` |
| codegen | `tests/test_drift.sh` 加数组长度漂移 case | 上游 `ids[8]`→`ids[4]` → 生成代码 `static_assert` 失败 → 编译失败(证明 `std::extent_v` 防护生效) |
| NameRegistry | `tests/test_name_registry.cpp` 加 case | `registerStructArray` 存 decl;`structArrayDecls()` 取回(nav/subRecipe/count/sep 正确) |
| Builder | `tests/test_builder.cpp` 加 3 case | 标量数组编译成 `LambdaProvider`(循环输出 `11-22-33-0-0-0-0-0`);结构体数组绑定子配方(`[box1|box2|box3|box4]`);**结构体数组里字段含数组**(§9a 组合形态:`100,200,300,400(7-8)|...`) |
| Engine | `tests/test_engine.cpp` 加 case | `loadConfig` + `render` 含两种数组的配方,端到端输出正确 |
| 并发 | `tests/test_hot_reload.cpp` 扩展配方 | 含结构体数组的配方,4 线程 render + 200 次重载,TSan 0 race |
| Route A 对等 | `tests/test_route_a.cpp` 扩展 | Route A `renderA` 与 Route B `render` 在含数组配方上输出一致 |

## 7. 端到端示例(扩展现有 `examples/security`)

把现有 `Event` 加上两种数组字段:

```c
// examples/security/event.h(扩展)
typedef struct {
    int x, y, w, h;
    int tags[2];               // Box 里的标量数组(新,§9a 组合形态)
} Box;
typedef struct {
    uint64_t timestamp;
    PersonInfo* person;        // 单结构体块(已有)
    Box rect;                  // 单结构体块(已有,rect.tags 也走数组)
    int feature_ids[8];        // 标量数组(新)
    Box boxes[4];              // 结构体数组(新,每个 box 含 tags 数组 → 嵌套组合)
} Event;
```

`schema.json` + `recipes.json` + `registry_gen.cpp`(重生成)+ `main.cpp` + `test_example_e2e.cpp` 同步扩展,最终输出含数组(含结构体数组里字段含数组的嵌套组合):
```
CAM001:1717171717|Alice-30@100,200,300,400(7-8)|11-22-33-0-0-0-0-0|100,200,300,400(1-2)|110,210,310,410(3-4)|0,0,0,0(0-0)|0,0,0,0(0-0)
```
其中 `${rect}` 的 `box` 子配方含 `${tags}` → `100,200,300,400(7-8)`(单结构体块里字段含数组);`${boxes}` 每个 box 同理 → 4 个 `x,y,w,h(tags)` 用 `|` 连接(结构体数组里字段含数组,§9a 组合形态)。

`test_example_e2e.cpp` 断言这个精确串。这是整个扩展的集成验证门。

## 8. 全流程(从上游结构体到结构体对象访问)

以含两种数组的 `Event` 走一遍。

**8.1 上游 `event.h`**(上游提供,只读):见 §7。

**8.2 描述文件 `schema.json`**(开发者照 `.h` 手写镜像 + 元数据):见 §2 示例。

**8.3 构建期 codegen**(`gen_registry.py schema.json → registry_gen.cpp`):见 §3 生成产物。生成的代码引用真实 `s->feature_ids[i]`/`s->boxes[i]`/`s->name`/`s->timestamp`,且每数组带 `static_assert(std::extent_v...)`。

**8.4 编译期校验**(漂移兜底):
- 上游把 `feature_ids` 改名 → `s->feature_ids` 编译失败。
- 上游把 `feature_ids[8]` 缩成 `[4]` → `static_assert(4>=8)` 失败 → 编译失败(`std::extent_v` 防护生效)。
- 上游把 `boxes` 改成 `bxs` → `s->boxes` 编译失败。

**8.5 启动期注册**:
```cpp
sv::NameRegistry reg;
registerStructViewNames(reg);          // codegen 生成的函数,填充 reg
sv::ConnectorLib lib;
sv::Engine engine(reg, lib);
engine.loadConfig(recipesJsonText);
```
注册后:`time`/`name`/`feat_ids`/`x`/`y`/.../`camera` → `ValueProvider*`;`boxes` → `StructArrayDecl`。

**8.6 用户配方 `recipes.json`**(对用户,数组 name 与普通 name 语法完全一致):
```json
{ "recipes": [
    { "name": "full_line", "template": "${camera}:${time}|${name}|${feat_ids}|${boxes}" },
    { "name": "box",       "template": "${x},${y},${w},${h}" }
] }
```

**8.7 运行时结构体对象访问(热路径)**:
```cpp
Event ev{1717171717, {"Alice"}, {11,22,33,0,0,0,0,0}, {{{100,200,300,400},{110,210,310,410},{0,0,0,0},{0,0,0,0}}}};
MyDeviceCtx ctx;
engine.render("full_line", &ev, ctx);
```
执行流(Engine 直线遍历 `full_line` 的 Step[]):
```
${camera}     → device provider: "CAM001"
":"           → 字面量
${time}       → uint64 provider: "1717171717"
"|"           → 字面量
${name}       → cstr provider: "Alice"
"|"           → 字面量
${feat_ids}   → 标量数组 LambdaProvider: for i in 0..8: snprintf s->feature_ids[i], "-" 连接 → "11-22-33-0-0-0-0-0"
"|"           → 字面量
${boxes}      → ArrayStructBlockProvider: for i in 0..4: nav(&ev,i)=&ev->boxes[i] → 跑 "box" 子配方 → "x,y,w,h"; "|" 连接
                → "100,200,300,400|110,210,310,410|0,0,0,0|0,0,0,0"
```
最终输出:`CAM001:1717171717|Alice|11-22-33-0-0-0-0-0|100,200,300,400|110,210,310,410|0,0,0,0|0,0,0,0`

**8.8 并发安全(RCU)**:数组 provider 配方内私有 + 引用计数保活,见 §4.6。

## 9. 明确的 YAGNI / 不做项

- **不做动态长度**(`countField` 兄弟字段引用)—— v2。
- **不做 `${i}` 下标暴露**(需局部变量绑定机制)—— v2。
- **不为 Route A 专造数组 binding** —— Route A 复用 `ArrayStructBlockProvider`(§5)。
- **不做二维数组**(`T arr[N][M]`,如 `int matrix[3][4]`)—— 安防场景确认无此需求。

## 9a. 天然支持:结构体数组里的字段含数组

> 说明:本节澄清一种**无需额外设计、方案 1 自然支持**的组合形态,避免被误归入「不做」。

安防场景存在「结构体数组的元素自身又含数组字段」的需求:

```c
typedef struct {
    int x, y, w, h;
    int tags[2];        // Box 里有标量数组
} Box;
typedef struct {
    Box boxes[4];       // 结构体数组,每个元素含数组
} Event;
```

期望展开:`100,200,300,400(7-8)|110,210,310,410(9-10)|...`(每个 box 的 `tags` 用 `-` 连,box 之间用 `|` 连)。

**这已经是方案 1 的自然组合,零额外设计**:
- 外层 `boxes[4]` → `ArrayStructBlockProvider`,每个元素跑 `box` 子配方。
- `box` 子配方里写 `${tags}` → codegen 为 `Box` 的 `tags` 字段生成标量数组 `LambdaProvider`(§3.1)。
- 子配方跑 `box` 时,`structPtr` 是 `&s->boxes[i]`(当前那个 Box),`${tags}` provider 在这个指针上取 `tags` 数组循环。

递归天然成立 —— `ValueProvider::get(const void*, ctx)` 的 `structPtr` 就是「当前层要处理的结构体指针」,外层数组把元素地址传给子配方,子配方里的数组 provider 在该地址上继续循环。**接口不变,零额外类。**

schema 写法(外层结构体数组 + Box 的标量数组字段):
```json
{ "name": "Event", "members": [
    { "block": "boxes", "array": { "subRecipe": "box", "count": 4, "sep": "|" } }
]},
{ "name": "Box", "members": [
    { "field": "x", "type": "int", "fmt": "%d" },
    { "field": "y", "type": "int", "fmt": "%d" },
    { "field": "w", "type": "int", "fmt": "%d" },
    { "field": "h", "type": "int", "fmt": "%d" },
    { "field": "tags", "type": "int", "fmt": "%d", "array": { "count": 2, "sep": "-" } }
]}
```
配方:`{ "name": "box", "template": "${x},${y},${w},${h}(${tags})" }`。

此形态在端到端示例(§7)与测试(§6 `test_builder`/`test_engine`)中应覆盖一例,作为组合能力的验证。

## 10. 实现改动清单(交由实现计划)

| 文件 | 改动 |
|---|---|
| `tools/gen_registry.py` | 加 `field_array_line` / `block_array_line`;HEADER 加 `<type_traits>`/`<cstddef>`;`gen()` 加 `array` 分支 |
| `include/struct_view/ValueProvider.hpp` | 追加 `IndexedNavigator` + `ArrayStructBlockProvider` |
| `src/ValueProvider.cpp` | 追加 `ArrayStructBlockProvider::get` |
| `include/struct_view/NameRegistry.hpp` | 追加 `StructArrayDecl` + `registerStructArray` + `structArrayDecls()` + `structArrayDecls_` 成员 |
| `src/NameRegistry.cpp` | 实现 `registerStructArray` / `structArrayDecls` |
| `src/Validator.cpp` | 扩展:结构体数组 subRecipe 存在性 + 循环检测(纳入 `structArrayDecls()`) |
| `src/Builder.cpp` | Pass 1 `pendingArray` + Pass 2 创建 `ArrayStructBlockProvider` |
| `src/BuilderA.cpp` | 同 Builder,但塞进 `FieldBinding`(Route A 对等,§5) |
| `tests/test_codegen.py` | +2 case(标量/结构体数组生成形状) |
| `tests/test_drift.sh` | +数组长度漂移 case |
| `tests/test_name_registry.cpp` | +`registerStructArray` case |
| `tests/test_builder.cpp` | +2 case(标量/结构体数组编译) |
| `tests/test_engine.cpp` | +含数组配方 case |
| `tests/test_hot_reload.cpp` | 扩展配方含结构体数组 |
| `tests/test_route_a.cpp` | 扩展配方含数组(Route A 对等) |
| `examples/security/*` | `event.h` 加数组字段;`schema.json`/`recipes.json`/`registry_gen.cpp`(重生成)/`main.cpp`/`test_example_e2e.cpp` 同步 |
