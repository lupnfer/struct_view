# struct_view 使用指导

> 本文档面向两类角色:
> - **[指导 A —— 维护者](#指导-a-维护者新增上游结构体)**:服务侧开发者,负责接入新的上游 C 结构体、维护 schema/registry、提供能力描述给 client。
> - **[[指导 B —— Client 侧使用者](#指导-b-client-侧使用者组合字符串下发)]**:业务方,基于维护者提供的能力描述,编写 recipe 配方下发到服务侧,组合出所需字符串。

---

## 指导 A:维护者 —— 新增上游结构体

### A1. 整体职责

维护者负责:
1. 把上游 C 结构体定义(`.h`)接入工程。
2. 照 `.h` 手写镜像描述文件 `schema.json`(声明字段/块/数组/设备 + 展开规则 + desc)。
3. 构建期跑 codegen 生成注册代码 `registry_gen.cpp`(自动)。
4. 定义 `DeviceCtx` 子类(设备信息 getter)。
5. 装配 `Engine` + 加载 recipe 配方。
6. **向 client 提供能力描述**(可用 name 清单 + desc),让 client 据此写 recipe。

### A2. 新增一个上游结构体 —— 完整步骤

以新增 `Track` 结构体为例:

```c
// 上游提供(只读,不改)
typedef struct {
    uint64_t track_id;
    float    confidence;
    char     label[16];
    Box      bbox;          // 内嵌结构体(单结构体块)
    int      hist[4];       // 标量数组
} Track;
```

#### 步骤 ①:把 `.h` 放进工程

把结构体定义追加到现有头文件(如 `examples/security/event.h`),或新建头文件。

> 这个 `.h` 是 codegen 生成代码要 `#include` 的真实 C 定义 —— 生成的 `s->track_id` 等必须能在编译期解析到真实成员,否则编译失败(编译期漂移兜底)。

#### 步骤 ②:在 `schema.json` 里描述

在 `structs` 数组里加一项,逐 member 描述:

```json
{"name": "Track", "members": [
    {"field": "track_id",   "as": "tid",   "desc": "轨迹ID",   "type": "uint64", "fmt": "%llu"},
    {"field": "confidence", "as": "conf",  "desc": "置信度",   "type": "float",   "fmt": "%.2f"},
    {"field": "label",      "as": "label", "desc": "标签名",   "type": "cstr",    "fmt": "%s"},
    {"block": "bbox",       "desc": "检测框", "ptr": false, "sep": ",", "fields": ["x", "y", "w", "h"]},
    {"field": "hist",       "as": "hist",  "desc": "直方图",   "type": "int", "fmt": "%d",
     "array": {"count": 4, "sep": "-"}}
]}
```

各字段含义:

| 键 | 必填 | 含义 |
|---|---|---|
| `field` | 标量字段必填 | 真实 C 成员名(codegen 据此写 `s->field`) |
| `as` | 可选 | 暴露给配方的友好名(省略则取 `field`);不能以 `r_` 开头 |
| `desc` | 可选 | 中文描述(给 client 显示用) |
| `type` | 标量字段必填 | `uint64` / `int` / `float` / `cstr` |
| `fmt` | 标量字段必填 | printf 格式器(`%llu` / `%d` / `%.2f` / `%s`) |
| `block` | 结构体块必填 | C 成员名(数组或单元素) |
| `ptr` | 结构体块 | `true`(指针成员 `T*`)/ `false`(内嵌成员 `T`) |
| `sep` | 结构体块必填 | 块内字段间默认分隔符(可被 recipe `:sep=` 覆盖) |
| `fields` | 结构体块必填 | 块内字段展开顺序(列表) |
| `array` | 数组必填 | `{"count": N, "sep": "x"}` —— count=长度(编译期漂移防护),sep=默认分隔符 |
| `device` | 设备信息 | 在 `device` 数组里声明 `{"getter": "...", "as": "...", "desc": "..."}` |

> **校验**:codegen 会对畸形 schema 报错(如 `block` 缺 `fields`/`sep`、字段名以 `r_` 开头、数组缺 `count`)。构建期即暴露,不会带到运行时。

#### 步骤 ③:重新生成注册代码(自动)

```bash
python3 tools/gen_registry.py schema.json registry_gen.cpp
```

或直接 `cmake --build build`(CMake 的 `add_custom_command` 在 `schema.json` 变更时自动重生成)。

生成的 `registry_gen.cpp` 包含 `void registerStructViewNames(sv::NameRegistry& reg)`,内部调用:
- `registerProvider("tid", makeProvider(...), "轨迹ID")` —— 标量字段
- `registerStruct("bbox", Navigator(...), {"x","y","w","h"}, ",", "检测框")` —— 结构体块
- `registerScalarArray("hist", elemFn(...), 4, "-", "直方图")` —— 标量数组

每个数组带 `static_assert(std::extent_v<decltype(s->hist)> >= 4)` 长度漂移防护。

#### 步骤 ④:如需设备信息,定义 DeviceCtx 子类

```cpp
// device_ctx.h
struct MyDeviceCtx : sv::DeviceCtx {
    std::string cameraId() const { return actualCameraId; }
    std::string serverName() const { return "srv-01"; }
    // ... 运行时每次 render 调用传入实例,值随请求变
};
```

在 schema 的 `device` 数组里声明:
```json
"device": [
    {"getter": "cameraId",  "as": "camera",  "desc": "摄像头编号"},
    {"getter": "serverName", "as": "server", "desc": "服务器名"}
]
```

#### 步骤 ⑤:装配 Engine(服务启动时)

```cpp
// main.cpp / 服务启动
sv::NameRegistry reg;
registerStructViewNames(reg);          // codegen 生成的函数

sv::ConnectorLib lib;                   // 连接符库(服务提供)
lib.add("dash", "-");
lib.add("pipe", "|");

sv::Engine engine(reg, lib);

// 加载 recipe(可从文件读,也可从 client 下发)
engine.loadConfig(recipeJsonText);
```

#### 步骤 ⑥:向 client 提供能力描述

维护者需要告诉 client「有哪些 name 可用 + 每个 name 是什么」。通过 `describe()` / `describeRecipe()` API 查询,或直接导出 schema 的 name + desc 清单:

**可用 name 分类:**

| name 来源 | 命名规则 | 示例 | recipe 引用方式 |
|---|---|---|---|
| 标量字段 | schema `field`/`as`,无前缀 | `tid`、`conf`、`label`、`time` | `${tid}` |
| 标量数组 | schema `field`/`as` + `array` | `hist`、`feat_ids` | `${hist}` 或 `${hist:sep=*}` |
| 结构体块 | schema `block`,无前缀 | `bbox`、`person`、`rect` | `${bbox}` 或 `${bbox:sep=-}` |
| 结构体数组 | schema `block` + `array` | `boxes` | `${boxes}` 或 `${boxes:sep=\|:isep=,}` |
| 设备信息 | schema `device.as` | `camera`、`server` | `${camera}` |
| 连接符 | `ConnectorLib.add` 注册 | `dash`、`pipe` | `${dash}` |
| 用户子配方 | recipes.json `name`,`r_` 前缀 | `r_meta`、`r_alarm_line` | `${r_meta}`(被其他配方引用)或 `render("r_alarm_line")` |

> **关键规则**:name 不以 `r_` 开头的是原生字段/块/设备(绑 C 结构体);以 `r_` 开头的是配方(用户组合产物)。

**desc 查询 API(给 client 显示用):**
```cpp
// 查字段/块/设备的 desc
reg.describe("tid");       // → "轨迹ID"
reg.describe("bbox");      // → "检测框"
reg.describe("camera");    // → "摄像头编号"

// 查配方的 desc
engine.describeRecipe("r_alarm_line");  // → "告警行"
```

---

### A3. 漂移兜底

上游 `.h` 改动时:
- **改字段名/删字段** → codegen 生成的 `s->field` 编译失败。
- **缩数组长度**(`hist[4]`→`hist[2]`)→ `static_assert(std::extent_v<decltype(s->hist)> >= 4)` 失败。

修复:改完 `.h` 后同步更新 `schema.json`,重跑 codegen,重新编译。

### A4. 维护者 Checklist

- [ ] `.h` 放入工程,codegen 能 `#include` 到。
- [ ] `schema.json` 里每个 member 的 `field`/`block` 名与 `.h` 真实成员名一致。
- [ ] 数组 member 的 `count` 与 `.h` 数组长度一致(否则 `static_assert` 失败)。
- [ ] 结构体块的 `fields` 里的字段名在所属结构体的 members 里存在。
- [ ] 字段/块名不以 `r_` 开头(留给配方)。
- [ ] `registry_gen.cpp` 重新生成。
- [ ] `cmake --build build` 编译通过。
- [ ] 向 client 提供可用 name 清单(+ desc)。

---

## 指导 B:Client 侧使用者 —— 组合字符串下发

### B1. 整体职责

Client 侧使用者负责:
1. 从维护者获取**可用 name 清单**(字段/块/数组/设备/连接符 + desc)。
2. 编写 **recipe 配方**(JSON),描述字符串怎么组合。
3. 把 recipe JSON **下发给服务侧**(通过 `loadConfig` 热加载)。
4. 调用 `render` 获取拼接好的字符串。

> **不需要写 C++ 代码、不需要重编译**。改配方只改 JSON,热加载即生效。

### B2. recipe 配方格式

```json
{
  "recipes": [
    {
      "name": "r_<配方名>",
      "desc": "<可选描述>",
      "template": "<模板字符串>"
    }
  ]
}
```

- `name`:必须以 `r_` 开头(标识这是用户组合产物,与原生字段区分)。
- `desc`:可选,中文描述(给显示用)。
- `template`:模板字符串,用 `${name}` 引用可用 name,其余字符原样输出。

### B3. 模板语法

| 语法 | 含义 | 示例 |
|---|---|---|
| `${name}` | 引用 name,用 schema 默认分隔符展开 | `${time}` → `1717171717` |
| `${name:sep=xxx}` | 覆盖主分隔符 | `${hist:sep=*}` → `11*22*33*44` |
| `${name:isep=yyy}` | 覆盖内部分隔符(仅结构体数组) | `${boxes:isep=,}` → 元素内字段用 `,` |
| `${name:sep=xxx:isep=yyy}` | 两个都覆盖 | `${boxes:sep=@:isep=,}` |
| 其余字符 | 原样输出(字面连接符) | `:` `|` `@` `-` 等 |

### B4. 可引用的 name 类型

| name 类型 | `${name}` 展开为 | `:sep=` 覆盖 | `:isep=` 覆盖 |
|---|---|---|---|
| 标量字段 | 格式化后的单值 | 不适用(报错) | 不适用 |
| 标量数组 | 元素按默认 sep 连接 | 元素间分隔符 | 不适用(报错) |
| 结构体块 | 块内字段按默认 sep 连接 | 字段间分隔符 | 不适用(报错) |
| 结构体数组 | 元素按默认 arraySep 连接,每元素内字段按默认 sep 连接 | 元素间分隔符 | 元素内字段间分隔符 |
| 设备信息 | getter 返回值 | 不适用 | 不适用 |
| 连接符 | 注册的字面量 | 不适用 | 不适用 |
| 用户子配方 | 跑子配方模板 | 不适用 | 不适用 |

> 判断 name 类型:**不以 `r_` 开头的是原生字段/块/设备**(维护者提供);**以 `r_` 开头的是配方**(用户组合)。

### B5. 完整示例

假设维护者提供了以下可用 name:

| name | 类型 | desc | 默认展开 |
|---|---|---|---|
| `time` | 标量字段 | 时间戳 | `1717171717` |
| `camera` | 设备信息 | 摄像头编号 | `CAM001` |
| `person` | 结构体块 | 人体信息 | `Alice-30`(name-age,sep=`-`) |
| `rect` | 结构体块 | 检测框 | `100,200,300,400,7-8`(x,y,w,h,tags,sep=`,`) |
| `feat_ids` | 标量数组 | 特征ID列表 | `11-22-33-0-0-0-0-0`(sep=`-`) |
| `boxes` | 结构体数组 | 检测框数组 | `100,200,300,400,1-2\|110,...`(元素间`\|`,元素内`,`) |
| `dash` | 连接符 | — | `-` |

#### 示例 1:基本配方

```json
{
  "recipes": [
    {
      "name": "r_alarm_line",
      "desc": "告警行",
      "template": "${camera}:${time}|${person}@${rect}|${feat_ids}|${boxes}"
    }
  ]
}
```

输出:
```
CAM001:1717171717|Alice-30@100,200,300,400,7-8|11-22-33-0-0-0-0-0|100,200,300,400,1-2|...
```

#### 示例 2:覆盖分隔符

```json
{
  "recipes": [
    {
      "name": "r_custom_line",
      "desc": "自定义分隔",
      "template": "${camera}:${time}|${feat_ids:sep=*}|${boxes:sep=@:isep=.}"
    }
  ]
}
```

- `${feat_ids:sep=*}` → 元素间用 `*`(覆盖默认 `-`):`11*22*33*0*0*0*0*0`
- `${boxes:sep=@:isep=.}` → 元素间用 `@`(覆盖默认 `|`),元素内字段间用 `.`(覆盖默认 `,`):`100.200.300.400.1-2@110.210.310.410.3-4@...`

输出:
```
CAM001:1717171717|11*22*33*0*0*0*0*0|100.200.300.400.1-2@110.210.310.410.3-4@...
```

#### 示例 3:用户组合子配方(可复用片段)

```json
{
  "recipes": [
    {
      "name": "r_meta",
      "desc": "元数据片段",
      "template": "${camera}:${time}|${feat_ids}"
    },
    {
      "name": "r_full",
      "desc": "完整行",
      "template": "${r_meta}|${person}@${rect}|${boxes}"
    }
  ]
}
```

- `r_meta` 是用户组合子配方(不绑结构体,纯拼接),可被 `${r_meta}` 引用。
- `r_full` 引用 `${r_meta}` + 其他 name。

输出(`r_full`):
```
CAM001:1717171717|11-22-33-0-0-0-0-0|Alice-30@100,200,300,400,7-8|100,200,300,400,1-2|...
```

### B6. 下发到服务侧

Client 把 recipe JSON 通过服务侧提供的接口传入:

```cpp
// 服务侧接口(维护者实现)
LoadResult lr = engine.loadConfig(clientProvidedRecipeJson);
if (!lr.ok) {
    // 返回错误清单给 client(一次性全部返回)
    for (auto& e : lr.errors) {
        // e.recipe, e.message, e.line
    }
}
```

**错误处理**:
- `loadConfig` 失败时**旧配方继续服务**(绝不半发布)。
- 错误一次性全部返回(不停止于第一个),client 改一次看全所有问题。
- 常见错误:recipe 名没 `r_` 前缀、引用了不存在的 name、`:sep=` 用在标量字段上、用户子配方循环引用。

### B7. 热加载

```cpp
// 换配方不停机,不打断在途 render
engine.loadConfig(newRecipeJson);   // 原子替换
```

- 新配方立即生效,旧配方靠引用计数保活至在途 render 跑完。
- 改分隔符只改 recipe(`:sep=`),不动 schema、不重编译。

### B8. Client Checklist

- [ ] 所有配方名以 `r_` 开头。
- [ ] `${name}` 引用的 name 在维护者提供的清单里。
- [ ] `:sep=` 只用在标量数组/结构体块/结构体数组上(标量字段/设备会报错)。
- [ ] `:isep=` 只用在结构体数组上(其他类型会报错)。
- [ ] 用户子配方不循环引用(`r_a` 引用 `r_b`,`r_b` 引用 `r_a` → 报错)。
- [ ] 下发后检查 `loadConfig` 返回的 `ok` + `errors`。

---

## 附录:完整数据流

```
维护者侧                                    Client 侧
─────────                                   ─────────
上游 .h (C 结构体)                          
    ↓ 手写镜像                               
schema.json (描述文件)                       
    ↓ codegen 自动                           
registry_gen.cpp (注册代码)                  
    ↓ 编译                                   
NameRegistry (name 词汇表)                   
    ↓                                        

ConnectorLib (连接符库)                      recipe JSON (配方)
    ↓                                         ↓
Engine ← ← ← ← ← ← ← ← ← ← ← ← ← ← ← ←  loadConfig(下发)
    ↓                                         ↓
render("r_xxx", &struct, ctx)  ← ← ← ← ←  调用(或服务侧自动调用)
    ↓
拼接好的字符串
```

**职责边界**:
- 维护者:schema(结构体布局 + 默认呈现) + codegen + registry + Engine 装配。
- Client:recipe(组合规则 + 分隔符覆盖) → 下发 → render。

**热加载边界**:
- 改 schema(结构体布局) → 重跑 codegen + 重编译(布局静态)。
- 改 recipe(组合/分隔符) → 热加载,不重编译(呈现动态)。
