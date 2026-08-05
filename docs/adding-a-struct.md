# 新增元数据结构体 —— 操作指导

> 面向:需要在 struct_view 框架里接入一个**新的上游 C 结构体**(或给已有结构体加字段/数组)的开发者。
> 关联 spec:`docs/superpowers/specs/2026-08-04-struct-view-design.md`(主设计)、`docs/superpowers/specs/2026-08-05-struct-view-array-support.md`(数组扩展)。
> 参考示例:`examples/security/`(`Event`/`PersonInfo`/`Box`)。

---

## 1. 核心思路(先理解,再动手)

框架的分工是:**C 结构体布局知识写在 `schema.json` 描述文件里,构建期 codegen 据此生成注册代码,运行时配方(`recipes.json`)只引用 name**。所以接入一个新结构体,本质上是「照着上游 `.h` 写一份 schema 描述」,其余(codegen、注册、编译期校验)自动完成。

三类数据,三种 member 写法:

| 数据形态 | schema member 写法 | 生成的注册调用 |
|---|---|---|
| 标量字段(`uint64_t ts`) | `{"field": ..., "type": ..., "fmt": ...}` | `registerProvider`(标量 `LambdaProvider`) |
| 标量数组(`int ids[8]`) | `{"field": ..., "type": ..., "fmt": ..., "array": {count, sep}}` | `registerProvider`(循环 `LambdaProvider`) |
| 单结构体块(`Box rect` / `PersonInfo* p`) | `{"block": ..., "ptr": bool, "subRecipe": ...}` | `registerStruct`(`Navigator` + 子配方名) |
| 结构体数组(`Box boxes[4]`) | `{"block": ..., "array": {subRecipe, count, sep}}` | `registerStructArray`(`IndexedNavigator` + 子配方名 + count + sep) |
| 设备信息(`cameraId()`) | `device` 数组里 `{"getter": ..., "as": ...}` | `registerProvider`(下转 `DeviceCtx` 子类调 getter) |

字符数组 `char name[32]` 走**标量字段**的 `type: "cstr"`(C 字符串语义,`%s` 读到 `\0`)。

---

## 2. 完整步骤(以新增一个 `Track` 结构体为例)

假设上游给了一个新的轨迹结构体:

```c
typedef struct {
    uint64_t track_id;
    float    confidence;
    char     label[16];
    Box      bbox;          // 内嵌结构体(单结构体块)
    int      hist[4];       // 标量数组
} Track;
```

### 步骤 ①:把上游 `.h` 放进示例目录(或你的工程目录)

把结构体定义放进头文件(上游提供,只读,不改)。示例工程放 `examples/security/event.h`(可追加,或新建头)。本例把 `Track` 追加到 `event.h`。

> 关键:这个 `.h` 是 codegen 生成代码要 `#include` 的真实 C 定义 —— codegen 生成的 `s->track_id` / `s->bbox` 必须能在编译期解析到真实成员,否则编译失败(这就是「编译期漂移兜底」)。

### 步骤 ②:在 `schema.json` 里描述这个结构体

在 `structs` 数组里加一项,members 逐字段描述:

```json
{"name": "Track", "members": [
    {"field": "track_id",   "as": "tid",   "type": "uint64", "fmt": "%llu"},
    {"field": "confidence", "as": "conf",  "type": "float",   "fmt": "%.2f"},
    {"field": "label",      "as": "label", "type": "cstr",    "fmt": "%s"},
    {"block": "bbox",       "ptr": false,  "subRecipe": "box"},
    {"field": "hist",       "as": "hist",  "type": "int",     "fmt": "%d",
     "array": {"count": 4, "sep": ","}}
]}
```

字段语义:
- `field` = 真实 C 成员名(codegen 据此写 `s->field`);`as` = 暴露给配方的友好名(可省略,默认取 `field`)。
- `type` = `uint64`/`int`/`float`/`cstr` 之一(决定 `snprintf` 的类型转换)。
- `fmt` = `printf` 格式器(`%llu`/`%d`/`%.2f`/`%s`)。
- `block` = 结构体块,`ptr` = 该成员是指针(`PersonInfo* person` → `true`)还是内嵌(`Box rect` → `false`);`subRecipe` = 这块结构体展开时跑哪个子配方(子配方在 `recipes.json` 里定义,见步骤 ④)。
- `array` = 数组:`count`(固定长度)+ `sep`(元素间连接符)。结构体数组还要 `subRecipe`;标量数组复用外层 `type`/`fmt`。

### 步骤 ③:重新生成注册代码 `registry_gen.cpp`

```bash
python3 tools/gen_registry.py examples/security/schema.json examples/security/registry_gen.cpp
```

或直接重新构建(CMake 的 `add_custom_command` 会在 `schema.json` 变更时自动重生成):

```bash
cmake --build build
```

生成的 `registry_gen.cpp` 会包含 `reg.registerProvider("tid", ...)` / `reg.registerStruct("bbox", ...)` 等调用,每个数组带 `static_assert(std::extent_v<decltype(s->hist)> >= 4)` 长度漂移防护。

> **校验**:codegen 现在对畸形 member 会报错(M-5)。如果 schema 写错了(比如 `block` 拼成 `bloc`、数组缺 `count`),构建会失败并给出清晰错误信息,不会静默丢失字段。

### 步骤 ④:在 `recipes.json` 里写配方(引用 name)

对用户而言,所有 name 统一为 `${name}` 占位符,不区分标量/结构体块/数组。给 `Track` 写一个输出配方:

```json
{
  "recipes": [
    { "name": "track_line", "template": "${tid}|${label}|${conf}|${bbox}|${hist}" },
    { "name": "box",        "template": "${x},${y},${w},${h}" }
  ]
}
```

- `${bbox}` 展开跑 `box` 子配方(`Box` 的字段 x/y/w/h);`Box` 若有 `tags` 数组,子配方里可加 `${tags}`。
- `${hist}` 展开为 `11,22,33,44`(标量数组循环 + `,` 连接)。
- 结构体块的子配方(`box`)是普通配方,只是它的 name 在 schema 里被 `registerStruct` 关联了。**子配方和主配方写法完全一致**。

> **改配方不需重编译** —— `recipes.json` 是运行时热加载的(见步骤 ⑥)。只有改 `schema.json`(结构体布局)才需要重跑 codegen + 重编译。

### 步骤 ⑤:在 `main.cpp` 里装配数据并渲染

```cpp
#include "event.h"   // 含 Track 定义
#include "device_ctx.h"
#include <struct_view/Engine.hpp>
#include <struct_view/NameRegistry.hpp>
#include <struct_view/ConnectorLib.hpp>
#include "registry_gen.cpp"   // codegen 产物,含 registerStructViewNames

int main() {
    sv::NameRegistry reg;
    registerStructViewNames(reg);          // 注册所有 schema 描述的 name
    sv::ConnectorLib lib;                   // 连接符库(如有需要 lib.add("dash","-"))
    sv::Engine engine(reg, lib);

    // 加载配方(运行时热加载)
    std::ifstream f("examples/security/recipes.json");
    std::stringstream ss; ss << f.rdbuf();
    if (!engine.loadConfig(ss.str()).ok) { /* 处理错误 */ return 1; }

    // 构造上游数据
    Track t{123, 0.95, "person", {100,200,300,400}, {11,22,33,44}};
    MyDeviceCtx ctx;
    std::cout << engine.render("track_line", &t, ctx) << "\n";
    // 输出形如: 123|person|0.95|100,200,300,400|11,22,33,44
}
```

### 步骤 ⑥:运行时热加载(换配方不停机)

```cpp
// 运维触发:换配方只需重新 loadConfig,不重编译、不打断在途 render
engine.loadConfig(newRecipesJsonText);   // 原子替换,RCU 保活旧配方
```

`loadConfig` 失败(语法错/引用不存在的 name/循环依赖)会返回错误清单且**绝不半发布**,旧配方继续服务。

---

## 3. 各字段类型的 schema 写法速查

| 上游 C 字段 | schema member | 说明 |
|---|---|---|
| `uint64_t ts` | `{"field":"ts","type":"uint64","fmt":"%llu"}` | 标量,`as` 可省 |
| `int x` | `{"field":"x","type":"int","fmt":"%d"}` | 标量 |
| `float f` | `{"field":"f","type":"float","fmt":"%.2f"}` | 标量,cast 为 double |
| `char name[32]` | `{"field":"name","type":"cstr","fmt":"%s"}` | C 字符串,读到 `\0` |
| `int ids[8]` | `{"field":"ids","type":"int","fmt":"%d","array":{"count":8,"sep":"-"}}` | 标量数组,输出 `1-2-3-...` |
| `Box rect` | `{"block":"rect","ptr":false,"subRecipe":"box"}` | 内嵌结构体块 |
| `PersonInfo* p` | `{"block":"p","ptr":true,"subRecipe":"person"}` | 指针结构体块 |
| `Box boxes[4]` | `{"block":"boxes","array":{"subRecipe":"box","count":4,"sep":"\|"}}` | 结构体数组 |
| 设备 `cameraId()` | `device` 数组:`{"getter":"cameraId","as":"camera"}` | 需在 `DeviceCtx` 子类里定义 getter |

> 结构体数组里元素自身含数组(`boxes[4]`,每个 `Box` 有 `tags[2]`)是**天然支持**的 —— 外层 `boxes` 跑 `box` 子配方,子配方里 `${tags}` 是标量数组 provider,在元素指针上循环。无需额外配置。

---

## 4. 漂移兜底(为什么改了 `.h` 会编译失败)

codegen 生成的代码引用真实成员:`s->track_id`、`s->bbox`、`s->hist[i]`。如果上游 `.h` 改了:

- 改字段名(`track_id` → `tid`)→ `s->track_id` 编译失败(无此成员)。
- 删字段 → 同上。
- 缩数组长度(`hist[4]` → `hist[2]`)→ `static_assert(std::extent_v<decltype(s->hist)> >= 4)` 失败(2 >= 4 为 false)。

**这种漂移在构建期暴露,不会带到运行时**。`tests/test_drift.sh` 验证此机制。

> 修复:改完 `.h` 后,同步更新 `schema.json`(改字段名/长度),重跑 codegen,重新编译。

---

## 5. 常见问题

**Q: 新结构体不在我现有的 `examples/security/` 里,怎么建一个新工程?**
复制 `examples/security/` 目录结构:`event.h`(你的 C 结构体)、`device_ctx.h`(你的 `DeviceCtx` 子类)、`schema.json`、`recipes.json`、`main.cpp`。在 `CMakeLists.txt` 里仿照 `security_example` 目标加一个 `add_executable` + `add_custom_command`(指向你的 `schema.json`)。

**Q: 两个不同结构体有同名字段(如 `Event.name` 和 `Track.name`)怎么办?**
用 `as` 给其中一个起别名:`{"field":"name","as":"track_label",...}`。name 在 `NameRegistry` 里是全局唯一的。

**Q: 我想加一种新的标量类型(如 `bool`)?**
在 `tools/gen_registry.py` 的 `TYPE_CAST` 字典里加一行(`"bool": "(int)"` + 用 `%d`),然后在 schema 里用 `"type":"bool"`。框架核心不用改。

**Q: 动态长度数组(`T* arr + size_t n`)支持吗?**
当前不支持(留作 v2)。固定长度数组用 `array.count`;动态长度需要 `countField` 兄弟字段引用,见 array-support spec §3.4 / §9。

**Q: 配方里能用条件/循环语法吗?**
不能。配方是极简 `${name}` 模板 + 字面连接符,刻意不引入条件/循环(YAGNI)。数组遍历的「循环」藏在 provider 内部(codegen 生成),对用户透明。

---

## 6. 验证清单(接入新结构体后)

- [ ] `schema.json` 里每个 member 的 `field`/`block` 名与 `.h` 真实成员名一致。
- [ ] 数组 member 的 `count` 与 `.h` 数组长度一致(否则 `static_assert` 失败)。
- [ ] 结构体块的 `subRecipe` 在 `recipes.json` 里有对应配方。
- [ ] `registry_gen.cpp` 重新生成(CMake 自动,或手动跑 `gen_registry.py`)。
- [ ] `cmake --build build` 编译通过(漂移兜底在此生效)。
- [ ] `engine.loadConfig(recipes.json).ok` 为 true(校验通过)。
- [ ] `engine.render(...)` 输出与预期一致。
