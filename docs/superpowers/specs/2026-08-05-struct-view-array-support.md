# 数组字段支持 —— 设计与全流程说明

> 关联 spec:`docs/superpowers/specs/2026-08-04-struct-view-design.md`
> 关联 codegen:`tools/gen_registry.py`
> 日期:2026-08-05

## 1. 问题

上游安防结构体常见数组字段,例如:

```c
typedef struct {
    uint64_t timestamp;
    char     name[32];          // 字符数组(人名)
    int      feature_ids[8];    // 标量数组(特征 ID 列表)
    Box      boxes[4];          // 结构体数组(多个检测框)
    PersonInfo* persons;        // 指针 + 长度语义的动态数组(另说)
} Event;
```

需求:框架能否支持把这些数组**按元素遍历、用连接符拼成字符串**?例如 `feature_ids` 展开成 `11-22-33`,`boxes` 展开成 `100,200,300,400|110,210,310,410|...`。

## 2. 现状核查(当前 codegen 对数组的行为)

`tools/gen_registry.py` 的 `field_line` 对标量字段生成:

```cpp
std::snprintf(buf, sizeof(buf), "{fmt}", {cast}s->{field});
```

| 数组形态 | 现状 | 正确性 |
|---|---|---|
| `char name[32]` | `type: cstr` → `(const char*)s->name` + `%s` | ✅ **已支持**。`char[]` 衰减为 `const char*`,`%s` 读到 `\0` 为止。这是「C 字符串」语义,不是「逐 char 遍历」。 |
| `int ids[8]` | `type: int` → `(int)s->ids` | ❌ **错误**。`s->ids` 衰减为 `int*`,被强转成 `int`(实为首元素地址低位的截断),`%d` 输出垃圾值。即便写成 `s->ids[0]` 也只取首元素,无法遍历。 |
| `Box boxes[4]` | `block` + `ptr:false` → `&s->boxes` | ❌ **错误**。导航到「整个数组」的首地址,子配方拿到的是 `Box(*)[4]`,不是单个 `Box`;无下标、无遍历。 |

**结论:当前框架只支持字符数组(cstr 语义),不支持一般标量数组与结构体数组的逐元素遍历。** 需要扩展。

## 3. 扩展设计

### 3.1 schema 新增 `array` member 类型

在 member 里加一种形态 `array`,描述「取第 i 个元素怎么取 + 取几个 + 怎么连」:

```json
{
  "field": "feature_ids",
  "as": "feat_ids",
  "array": {
    "elemType": "int",          // 元素标量类型(uint64/int/float/cstr)
    "elemFmt": "%d",            // 元素格式器
    "count": 8,                 // 数组长度(编译期已知;动态长度见 §3.4)
    "sep": "-",                 // 元素间连接符(字面量)
    "index": "i"                // 可选:把当前下标也暴露为 ${i}(用于 1-based 编号等)
  }
}
```

结构体数组同理,但元素是一个结构体块:

```json
{
  "block": "boxes",
  "array": {
    "elemSubRecipe": "box",     // 每个元素用哪个子配方展开
    "count": 4,
    "sep": "|"
  }
}
```

### 3.2 codegen 生成:数组 → 循环拼接的 provider

标量数组生成一个**配方内私有 provider**,内部循环 `count` 次,每次 `snprintf` 第 i 个元素 + 追加分隔符:

```cpp
// codegen 为 {"field":"feature_ids","as":"feat_ids","array":{...,"count":8,"sep":"-"}}
// 生成(示意):
reg.registerProvider("feat_ids", sv::makeProvider(
    [](const void* p, const sv::DeviceCtx&) -> std::string {
        const Event* s = static_cast<const Event*>(p);
        std::string out;
        char buf[64];
        for (std::size_t i = 0; i < 8; ++i) {
            if (i) out += "-";                       // sep
            std::snprintf(buf, sizeof(buf), "%d", (int)s->feature_ids[i]);
            out += buf;
        }
        return out;
    }));
```

结构体数组生成一个**配方内私有 StructBlockProvider 变体**:循环 `count` 次,每次导航到 `&s->boxes[i]` 跑子配方。这需要在 `ValueProvider` 层引入一个 `ArrayStructBlockProvider`(持有 `Navigator` 的「带下标版本」+ 子配方 + count + sep),或更简单地:把它实现成一个特殊的 `ValueProvider`,内部对每个元素调 `runRecipeB`。

### 3.3 关键实现点

1. **下标访问 `s->arr[i]`** —— codegen 知道 `count`,生成 `for (i=0; i<count; ++i) s->arr[i]`。编译期下标安全(`count` 与真实数组长度一致由 §4 漂移兜底保证:若上游把 `ids[8]` 改成 `ids[4]`,生成的 `s->ids[7]` 越界 → 实际上 C 不做越界检查,需另设防,见 §5)。
2. **分隔符** —— 首元素前不加,其余前加(`if (i) out += sep`)。这是「join」语义,与 recipe 模板里的字面连接符一致。
3. **结构体数组元素** —— 导航器需带下标:`[](const void* p, std::size_t i) -> const void* { return &static_cast<const Event*>(p)->boxes[i]; }`。当前 `Navigator` 签名是无下标的 `const void* (*)(const void*)`,需扩展为带下标版本(或新增 `IndexedNavigator`)。
4. **空数组** —— `count=0` → 返回空串(循环不执行)。安全。
5. **`index` 暴露** —— 若 schema 要 `${i}`,codegen 把循环变量也注册进一个临时局部上下文(目前 `DeviceCtx` 是唯一的副参数,扩展需引入「局部变量绑定」机制,超出 v1 范围,见 §5)。

### 3.4 动态长度数组(指针 + 长度)

安防场景常见 `T* arr + size_t n` 的动态数组:

```c
typedef struct { PersonInfo* persons; size_t person_count; } Event;
```

`count` 不能写死,需引用另一个字段:

```json
{
  "block": "persons",
  "array": {
    "elemSubRecipe": "person",
    "countField": "person_count",   // 从兄弟字段读长度
    "sep": "|"
  }
}
```

codegen 生成 `for (i=0; i<s->person_count; ++i)`,导航到 `s->persons[i]`。这要求 codegen 能引用同结构体的其他字段作为长度源 —— schema 里用 `countField` 指定,codegen 生成 `s->{countField}`。

## 4. 全流程(从上游结构体到结构体对象访问)

以一个含三种数组的完整示例走一遍。**注意:§4 的流程描述的是扩展后的目标行为;当前代码尚未实现 array member(见 §2 现状),标量/结构体数组会出错。** 字符数组(cstr)流程当前已可用。

### 4.1 上游结构体(`event.h`,上游提供,只读)

```c
#pragma once
#include <stdint.h>
typedef struct { int x, y, w, h; } Box;
typedef struct {
    uint64_t timestamp;
    char     name[32];           // 字符数组
    int      feature_ids[8];     // 标量数组
    Box      boxes[4];           // 结构体数组
} Event;
```

### 4.2 描述文件(`schema.json`,开发者照 `.h` 手写镜像 + 元数据)

```json
{
  "deviceCtxType": "MyDeviceCtx",
  "structs": [
    {
      "name": "Event",
      "members": [
        { "field": "timestamp", "as": "time", "type": "uint64", "fmt": "%llu" },
        { "field": "name",      "as": "name", "type": "cstr",   "fmt": "%s" },
        { "field": "feature_ids", "as": "feat_ids", "array": {
            "elemType": "int", "elemFmt": "%d", "count": 8, "sep": "-"
        }},
        { "block": "boxes", "array": {
            "elemSubRecipe": "box", "count": 4, "sep": "|"
        }}
      ]
    },
    {
      "name": "Box", "members": [
        { "field": "x", "type": "int", "fmt": "%d" },
        { "field": "y", "type": "int", "fmt": "%d" },
        { "field": "w", "type": "int", "fmt": "%d" },
        { "field": "h", "type": "int", "fmt": "%d" }
      ]
    }
  ],
  "device": [{ "getter": "cameraId", "as": "camera" }]
}
```

字段语义回顾:
- `field` = 真实 C 成员名;`as` = 暴露给配方的友好名(省略则取 `field`)。
- `type`/`fmt` = 标量类型与格式器(uint64/int/float/cstr)。
- `array` = 数组描述:`elemType`+`elemFmt`(标量元素)或 `elemSubRecipe`(结构体元素),`count`(固定长度)或 `countField`(动态长度),`sep`(元素间连接符)。
- `block` = 结构体块;带 `array` 即结构体数组。

### 4.3 构建期 codegen(`tools/gen_registry.py schema.json → registry_gen.cpp`)

codegen 读 schema,为每个 member 生成注册调用。数组 member 生成**循环拼接的 provider**(配方内私有,见 §3.2):

```cpp
// AUTO-GENERATED by tools/gen_registry.py — DO NOT EDIT.
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ValueProvider.hpp>
#include <struct_view/DeviceCtx.hpp>
#include <cstdio>
#include <string>

void registerStructViewNames(sv::NameRegistry& reg) {
    // --- 标量字段(已有能力)---
    reg.registerProvider("time", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {
            const Event* s = static_cast<const Event*>(p);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)s->timestamp);
            return std::string(buf);
        }));

    // --- 字符数组(已有能力,cstr 语义)---
    reg.registerProvider("name", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {
            const Event* s = static_cast<const Event*>(p);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", (const char*)s->name);
            return std::string(buf);
        }));

    // --- 标量数组(扩展后新增)---
    reg.registerProvider("feat_ids", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {
            const Event* s = static_cast<const Event*>(p);
            std::string out;
            char buf[64];
            for (std::size_t i = 0; i < 8; ++i) {
                if (i) out += "-";                              // sep
                std::snprintf(buf, sizeof(buf), "%d", (int)s->feature_ids[i]);
                out += buf;
            }
            return out;
        }));

    // --- 结构体数组(扩展后新增)---
    // 每个 boxes[i] 导航到 &s->boxes[i],跑 "box" 子配方,用 "|" 连接
    reg.registerStructArray("boxes", sv::IndexedNavigator(
        [](const void* p, std::size_t i) -> const void* {
            const Event* s = static_cast<const Event*>(p);
            return &s->boxes[i];
        }), /*subRecipe*/ "box", /*count*/ 4, /*sep*/ "|");

    // --- Box 子结构体字段(已有能力)---
    reg.registerProvider("x", sv::makeProvider(/* ... s->x ... */));
    reg.registerProvider("y", sv::makeProvider(/* ... s->y ... */));
    // w, h ...

    // --- 设备信息(已有能力)---
    reg.registerProvider("camera", sv::makeProvider(
        [](const void*, const sv::DeviceCtx& base) -> std::string {
            const MyDeviceCtx& ctx = static_cast<const MyDeviceCtx&>(base);
            return ctx.cameraId();
        }));
}
```

> 标量数组的 provider 是普通 `LambdaProvider`(已存在能力,只是 codegen 生成循环体)。结构体数组需要新增 `registerStructArray` + `IndexedNavigator` + `ArrayStructBlockProvider`(见 §5)。

### 4.4 编译期校验(漂移兜底)

生成的代码引用真实成员:`s->feature_ids[i]`、`s->boxes[i]`、`s->name`、`s->timestamp`。

- 上游把 `feature_ids[8]` 改名 `feat_ids[8]` → `s->feature_ids` 编译失败。
- 上游把 `boxes` 改成 `bxs` → `s->boxes` 编译失败。
- **注意:改数组长度(如 `ids[8]`→`ids[4]`)不会编译失败** —— C 不做编译期越界检查(对裸 `s->ids[i]`),运行时越界是 UB。需额外防护(见 §5)。

### 4.5 启动期注册

```cpp
// main.cpp / 示例装配
sv::NameRegistry reg;
registerStructViewNames(reg);          // 调 codegen 生成的函数,填充 reg
sv::ConnectorLib lib;
lib.add("dash", "-");  lib.add("pipe", "|");
sv::Engine engine(reg, lib);
engine.loadConfig(recipesJsonText);   // 加载配方
```

注册后 `NameRegistry` 内部:
- `time`/`name`/`feat_ids`/`x`/`y`/.../`camera` → `ValueProvider*`(标量/字符数组/标量数组都是 provider)。
- `boxes` → 结构体块声明(`IndexedNavigator` + 子配方名 `box` + count + sep)。

### 4.6 用户配方(`recipes.json`)

对用户而言,数组 name 与普通 name **语法完全一致**,都是 `${name}`:

```json
{
  "recipes": [
    { "name": "full_line",  "template": "${camera}:${time}|${name}|${feat_ids}|${boxes}" },
    { "name": "box",        "template": "${x},${y},${w},${h}" }
  ]
}
```

- `${feat_ids}` → 标量数组 provider 循环拼接 → `11-22-33-0-0-0-0-0`(假设前 3 个有值,后 5 个为 0)。
- `${boxes}` → 结构体数组,每个 `boxes[i]` 跑 `box` 子配方,用 `|` 连接 → `100,200,300,400|110,210,310,410|0,0,0,0|0,0,0,0`。

### 4.7 运行时结构体对象访问(热路径)

```cpp
Event ev{1717171717, {"Alice"}, {11,22,33,0,0,0,0,0}, {{{100,200,300,400},{110,210,310,410},{0,0,0,0},{0,0,0,0}}}};
MyDeviceCtx ctx;
engine.render("full_line", &ev, ctx);
```

执行流(Engine 直线遍历 `full_line` 的 Step[]):

```
${camera}     → device provider: downcast ctx → "CAM001"
":"           → 字面量
${time}       → uint64 provider: snprintf s->timestamp → "1717171717"
"|"           → 字面量
${name}       → cstr provider: s->name 衰减 → "Alice"
"|"           → 字面量
${feat_ids}   → 标量数组 provider:
                  for i in 0..8: snprintf s->feature_ids[i], "-" 连接
                  → "11-22-33-0-0-0-0-0"
"|"           → 字面量
${boxes}      → 结构体数组 provider:
                  for i in 0..4: 导航 &s->boxes[i] → 跑 "box" 子配方 → "x,y,w,h"
                  用 "|" 连接 4 个子串
                  → "100,200,300,400|110,210,310,410|0,0,0,0|0,0,0,0"
```

最终输出:
```
CAM001:1717171717|Alice|11-22-33-0-0-0-0-0|100,200,300,400|110,210,310,410|0,0,0,0|0,0,0,0
```

### 4.8 并发安全(RCU)

数组 provider 同样遵循 §3.4a RCU 三规定:
- 标量数组 provider 是 `LambdaProvider`,配方内私有(`RecipeB::ownedProviders`),不可变。
- 结构体数组 provider 若实现为 `ArrayStructBlockProvider`,同样配方内私有,持有 `shared_ptr<const RecipeB>` 指向子配方(引用计数保活)。
- `Engine` 稳定 store + `publishAll` 原子换 map。

热加载时旧配方(含其数组 provider + 子配方图)靠引用计数保活至在途 render 跑完。数组遍历不引入新的并发风险(循环在单个 render 调用内完成,无跨线程共享可变状态)。

## 5. 实现缺口与待决项(交由后续实现)

扩展数组支持需要改动的点(当前未做):

| 改动 | 说明 | 难度 |
|---|---|---|
| `gen_registry.py` 增加 `array` member 分支 | 标量数组:生成循环 `LambdaProvider`(复用已有 `makeProvider`)。结构体数组:生成 `registerStructArray` 调用。 | 中 |
| `NameRegistry` 增加 `registerStructArray` | 存「`IndexedNavigator` + 子配方名 + count + sep」声明(类似 `StructBlockDecl` 但带 count/sep)。 | 中 |
| 新增 `IndexedNavigator` | `const void* (*)(const void*, std::size_t i)` —— 带下标的导航器。当前 `Navigator` 是无下标的。 | 低 |
| 新增 `ArrayStructBlockProvider` | `ValueProvider` 子类:持有 `IndexedNavigator` + `shared_ptr<const RecipeB>` + count + sep;`get()` 循环 count 次导航 + 跑子配方 + sep 连接。配方内私有(RCU)。 | 中 |
| `Builder` Pass 2 处理结构体数组绑定 | 类似现有 struct-block 绑定,但创建 `ArrayStructBlockProvider`。 | 中 |
| `Validator` 校验 array member | `count`/`countField` 合法性、`elemSubRecipe` 存在性、标量数组 `elemType` 在 TYPE_CAST 里。 | 低 |
| 越界防护(长度漂移) | `count` 写死后,上游把 `ids[8]` 改成 `ids[4]` 不会编译失败(C 不检查 `s->ids[7]` 越界)。方案:codegen 生成 `static_assert(sizeof(s->ids) / sizeof(s->ids[0]) >= 8)` —— 编译期断言数组长度 ≥ count。**强烈推荐**,否则漂移兜底对数组长度无效。 | 低但重要 |
| `index` 暴露(`${i}`) | 把循环下标暴露给配方(如 1-based 编号)。需引入「局部变量绑定」机制(当前只有 `DeviceCtx` 一个副参数)。 | 高(超 v1 范围) |
| 动态长度(`countField`) | codegen 生成 `s->{countField}` 作为循环上界。需 schema 支持 `countField` + codegen 引用兄弟字段。 | 中 |

## 6. 速查:当前能用 vs 需扩展

| 上游字段形态 | 当前能否支持 | 怎么用 / 怎么扩 |
|---|---|---|
| `char name[32]`(C 字符串) | ✅ 已支持 | `{"field":"name","type":"cstr","fmt":"%s"}` |
| `char c`(单字符) | ✅ 已支持 | `{"field":"c","type":"int","fmt":"%c"}`(当 int 处理) |
| `int ids[8]`(标量数组) | ❌ 需扩展 | 加 `array` member(§3.1)+ 循环 provider(§3.2) |
| `Box boxes[4]`(结构体数组) | ❌ 需扩展 | 加 `block`+`array` + `ArrayStructBlockProvider`(§3.2/§5) |
| `PersonInfo* persons + count`(动态数组) | ❌ 需扩展 | 加 `countField`(§3.4) |
| `PersonInfo* person`(单指针) | ✅ 已支持 | `{"block":"person","ptr":true,"subRecipe":"person"}` |
| `Box rect`(单内嵌结构体) | ✅ 已支持 | `{"block":"rect","ptr":false,"subRecipe":"rect"}` |

## 7. 建议

1. **先加标量数组支持** —— 复用已有 `LambdaProvider`,只改 codegen 生成循环体 + `static_assert` 越界防护。改动小、收益大(覆盖 `feature_ids` 这类常见场景)。
2. **再加结构体数组** —— 需 `IndexedNavigator` + `ArrayStructBlockProvider`,但模式与现有 `StructBlockProvider` 同构,可参照实现。
3. **务必加 `static_assert` 越界防护** —— 否则数组长度漂移(改 `[8]`→`[4]`)不会编译失败,违背框架「编译期漂移兜底」的核心承诺。
4. **`index` 暴露 / 动态长度** 留作 v2 —— 需引入局部变量绑定机制 / 兄弟字段引用,超出当前架构,按需再扩。
