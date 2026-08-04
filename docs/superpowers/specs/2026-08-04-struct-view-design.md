# struct_view 设计文档 — 结构体转字符串框架

- 日期: 2026-08-04
- 状态: 已批准 (设计阶段)
- 作者: brainstorming 协作产出

## 1. 目标与背景

### 1.1 要解决的问题

安防场景下,上游视频结构化提取产出 **C 结构体数据**。需要一个框架,按用户诉求把这些结构体数据**自动转成字符串**。用户需要描述:字符串按什么顺序组合、字段间用什么连接符,并注入通用设备信息。

### 1.2 三大核心诉求

1. 结构体(安防视频结构化数据)自动转字符串,基于上游 C 结构化数据。
2. 用户描述字符串的组装规则:字段顺序、字段间连接符、通用设备信息;结构体里的数据都关联一个 **name**,name 不仅支持单个节点,还要支持**关键结构体(整块)**。
3. **易拓展、架构良好、用户易上手**。

### 1.3 已确定的关键约束(设计决策)

| 维度 | 决定 |
|---|---|
| 运行环境 / 实现语言 | C/C++ 同进程,直接操作内存中的 C 结构体 |
| 用户描述形式 | JSON 配置文件描述"组装配方" |
| name 注册机制 | 混合:代码注册"名字词汇表"(name→访问器,需 C 类型知识),配置只写"配方" |
| 连接符 | 启动时注册的"连接符库"(name→字面量/格式化器),属"服务提供" |
| 设备信息 | 运行时传入的"设备上下文"对象,值随每次调用带入 |
| 配置加载 | 动态运行时加载(可热加载/替换配方),框架始终服务最新版本 |
| 配方文件组织 | 同文件混排(主配方与子配方在同一份 JSON 内,靠 name 关联) |

> **"动态运行时加载"的语义**:配方可在运行时新增/替换(热加载),框架始终服务最新版本;**不是**"每次调用都重新解析传进来的配置文本"。解析/校验/绑定全部发生在加载时,热路径零解析。

## 2. 架构总览

### 2.1 两个时间维度 + 三类注册表

框架分为**加载时**(配置 → 不可变 Recipe)与**运行时**(每次调用直线遍历执行)两个维度。

三个注册表(启动时由代码填充):

| 注册表 | 内容 | 何时填 |
|---|---|---|
| `NameRegistry` | name → `ValueProvider`(标量字段访问器 或 复合结构块) | 启动时代码注册,需 C 类型知识 |
| `ConnectorLib` | 连接符名 → 字面量字符串 / 格式化器 | 启动时代码注册,属"服务提供" |
| `DeviceCtx` | 设备信息字段的固定接口(ID/名称/IP/通道等 getter) | 启动时定义接口,值运行时带入 |

### 2.2 加载时数据流

```
JSON 配置文本
   │  ConfigLoader.parse
   ▼
Config AST(语法树,未校验)
   │  Validator.walk(ast, NameRegistry, ConnectorLib, DeviceCtx)
   │    · 引用的 name 是否存在且类型匹配
   │    · 引用的连接符是否存在
   │    · 引用的设备字段是否存在
   │    · 子配方递归校验
   ▼
Builder.compile(已校验 AST)
   │  把 name/连接符/设备字段 解析绑定到具体访问器指针/字面量/getter
   ▼
不可变 Recipe = 有序 Step[](每步已可直接执行)
   │  RecipeStore.publish(name, recipe)  —— 原子指针替换
   ▼
对外可服务
```

关键:解析、校验、绑定全部发生在加载时。加载失败 → 配方不发布,旧版继续服务,错误前置暴露给运维而非延后到调用。

### 2.3 运行时数据流

```
render(recipeName, structPtr, deviceCtx)
   │  Engine 原子读取当前 Recipe 快照(按名查找)
   ▼
直线遍历 Step[],往输出 buffer 追加:
   · EmitField      → 调绑定的访问器(structPtr) 取值
   · EmitConnector  → 追加预查表的字面量
   · EmitDeviceCtx  → 调绑定的 getter(deviceCtx) 取值
   · EmitSubRecipe  → 导航到子结构体指针,跑子配方(递归)
   ▼
返回拼接好的 string
```

热路径上**无名字查找、无连接符查表、无每次解析、无每次分配(预留 buffer)**。

## 3. 核心数据结构

核心抽象围绕"加载时解析成可直接执行物"。以下为概念定义(展示设计意图,非最终 API 锁死)。

### 3.1 name 的两层模型(对应需求第 2 点核心)

name 不仅能绑单个字段,还能绑**一整块关键结构体**,而该结构体内部又是一条独立组装配方。本质上 name 指向"一段可执行的取值逻辑",分两类:

**类型 A — 标量字段(原始节点):** name 绑定到单个字段的读取 + 格式化。

```
NameRegistry["time"] → ValueProvider {
    从 Event* 取 timestamp 字段 → 按格式器转成字符串
}
```

**类型 B — 结构体块(关键结构体):** name 绑定到**一个子配方 + 一条"如何导航到该子结构体"的路径**。这块结构体被当作一个有名字的整体。

```
NameRegistry["person"] → CompositeValueProvider {
    navigate:  从 Event* 取 → event->person (PersonInfo*)
    subRecipe: [                  ← 这块结构体自己的组装规则
        EmitField("name",   person->name),
        EmitConnector("dash"),
        EmitField("age",    person->age),
    ]
}
```

配方里写 `${person}` 时,框架:① 导航到 `event->person` 子结构体指针;② 用该子指针跑 `subRecipe`;③ 把整块结果作为一个字符串片段拼进上层。子配方里还能再引用结构体块 name → 天然递归嵌套(如 person 里嵌 `${address}`,address 又是一块结构体)。

对**写配方的用户**:无需区分 A/B,都是 `${name}` 占位符,统一语法。区分只发生在**注册 name 的开发者**:标量字段注册访问器;结构体块注册"导航路径 + 子配方"。

### 3.2 双轨保留:Step 表示方式(待基准测试决断)

`Step` 的内部表示有两条候选路线,**两者在设计文档中均保留**,最终由方案对比 + 性能基准测试决定。这是明确的设计决策,不在纸面锁死。

**路线 A — 显式四类 Step(偏性能/直白):**

```cpp
enum class StepKind {
    Field,        // 标量字段:绑定了具体字段访问器 + 格式器
    Connector,    // 连接符:已查表的字面量(服务提供)
    Device,       // 设备信息:绑定了 DeviceCtx 的 getter
    SubRecipe,    // 结构体块:导航指针 + 子配方(递归)
};

struct Step {
    StepKind kind;
    // 加载时已绑定好,执行时直接调用(布局细节实现时定,
    // 可用 union 或 std::variant,设计阶段不锁死)
    std::variant<FieldBinding, ConnectorBinding, DeviceBinding, SubRecipeBinding> binding;
};
```

热路径零虚函数调用:Field 直接持有 `ValueProvider*`(一次虚调用取标量),SubRecipe 直接内联导航 + 子配方遍历(避免再绕一层 provider)。

**路线 B — 统一 `ValueProvider*` + 子配方递归(更简洁 / OOP):**

```cpp
struct Step {
    const ValueProvider* provider;   // 一切皆节点,统一接口
};
```

更简洁、更"一切皆节点",但结构体块递归会多一层虚调用(`StructBlockProvider` 内部再跑子配方)。

**决断准则:** 两者均实现,以真实安防结构体规模(嵌套层数、字段数、调用频率)跑基准对比。若性能差异在目标场景可忽略,选 B(简洁优先);若热路径为关键瓶颈,选 A。

### 3.3 Recipe — 不可变编译产物

```cpp
struct Recipe {
    std::string name;          // 配方名,render 时按此查找
    std::vector<Step> steps;   // 有序步骤,直线遍历
    // 不可变:发布后只读;热加载时整体替换,不改原地
};
```

加载时校验 + 编译产出一个 `Recipe`;一旦发布即只读。热加载 = 产新 `Recipe` + 原子替换指针,旧版等读者结束再释放。

### 3.4 ValueProvider — name 背后的取值逻辑(类型擦除)

注册表里 name 真正指向的对象。标量与结构体块**统一接口**,差异在实现:

```cpp
class ValueProvider {
public:
    virtual ~ValueProvider() = default;
    // engine 调它:给当前层结构体指针 + 上下文,产出自己的字符串片段
    virtual std::string get(const void* structPtr, const DeviceCtx& ctx) const = 0;
};

// 标量字段
class FieldProvider : public ValueProvider {
    /* 模板特化:绑定 T* ptr、字段偏移、格式器("%llu"/"%s" 等) */
    /* get(): ptr→取字段→格式化→返回 */
};

// 结构体块 —— 对应需求里的"关键结构体"
class StructBlockProvider : public ValueProvider {
    Navigator nav;            // 从父指针导航到子结构体指针(如 &event->person)
    const Recipe* subRecipe;  // 这块结构体自己的组装规则
    /* get(): nav(structPtr)→拿子指针→engine 跑 subRecipe→返回整块字符串 */
};
```

`StructBlockProvider` 持有子 `Recipe*` —— 递归嵌套的物理基础。结构体块本质上是"内嵌的、有导航入口的子执行器"。

### 3.5 三个注册表

```cpp
// name → ValueProvider(标量 或 结构体块),加载时校验用、编译后 ValueProvider 绑进 Step
class NameRegistry {
    std::unordered_map<std::string, std::unique_ptr<ValueProvider>> entries_;
public:
    void registerField (std::string name, /* 字段模板参数 */);
    void registerStruct(std::string name, Navigator nav, const Recipe* sub);
    const ValueProvider* lookup(const std::string& name) const;  // 加载时校验用
};

// 连接符名 → 字面量字符串/格式化器("服务提供")
class ConnectorLib {
    std::unordered_map<std::string, std::string_view> entries_;
public:
    void add(std::string name, std::string_view literal);  // dash→"-", pipe→"|"
    std::string_view get(const std::string& name) const;   // 加载时查表绑进 Step
};

// 设备信息:固定接口 + 运行时值
class DeviceCtx {
public:
    std::string cameraId() const;
    std::string deviceName() const;
    // ... 运行时每次调用传入实例,值随请求变
};
using DeviceGetter = std::string (DeviceCtx::*)() const;  // 成员函数指针,加载时绑定
```

### 3.6 引擎串接(概念示意)

```cpp
std::string Engine::render(const std::string& recipeName,
                           const void* structPtr,
                           const DeviceCtx& ctx) {
    const Recipe* r = store_.snapshot(recipeName);   // 原子读当前版本
    std::string out;
    out.reserve(estimatedLen_);                      // 预分配,避免多次扩容
    for (const Step& s : r->steps) {
        // 按 StepKind 分派 / 或统一调 provider->get(structPtr, ctx)
        // (依 3.2 双轨决断)
        ...
    }
    return out;
}
```

## 4. 配置格式与编译流水线

### 4.1 配置格式(JSON)

用户只需学一套 `${name}` 占位符语法,无论标量字段还是结构体块都统一引用。模板字符串风格,字段间字面字符即连接符,最易上手:

```json
{
  "recipes": [
    {
      "name": "alarm_line",
      "template": "${camera}:${time}|${person}@${rect}"
    },
    {
      "name": "person",
      "template": "${name}-${age}"
    },
    {
      "name": "rect",
      "template": "${x},${y},${w},${h}"
    }
  ]
}
```

**语法规则(全量,刻意极简):**

| 元素 | 写法 | 含义 |
|---|---|---|
| `${name}` | 占位符 | 引用注册表中的 name(标量字段 / 结构体块 / 设备字段) |
| 其余字符 | 字面量 | 连接符,原样输出 |

- 不引入条件分支、循环、表达式 —— YAGNI。需求只要求"按顺序组合 + 连接符 + 设备信息",不引入会带来解析复杂度和学习曲线的特性。扩展点预留(见第 6 节)。
- 结构体块的子配方也是普通配方,只是它的 name 在 `NameRegistry` 里被注册成 `StructBlockProvider`(导航 + 子配方)。对用户写法完全一致 —— 统一性是"易上手"的关键。
- **"连接符服务提供"的体现**:配置里的字面连接符(`:` `|` `@` `,` `-`)是用户配方的一部分,直接写。连接符库主要服务于**需要格式化逻辑的连接符**(如时间戳格式器、需统一编码的设备号格式器)—— 这些注册进 `ConnectorLib`(以普通 name 注册,如 `ts_fmt`),配方里同样用 `${ts_fmt}` 引用,语法与引用字段/结构体块完全一致。简单字面连接符走字面量通道,复杂格式化连接符走库通道,两者并存但**语法统一为 `${name}`**。

### 4.2 配方文件组织

**同文件混排**:主配方与子配方在同一份 JSON 的 `recipes` 数组内,靠 name 关联。一份配置自包含、可读、改一处即可;契合极简易上手,安防场景配方规模通常不大。

### 4.3 编译流水线(加载时,三段式)

```
① ConfigLoader.parse(text)        →  ConfigAst      纯语法解析,不查任何注册表
② Validator.validate(ast, regs)   →  validatedAst   查三表校验,收集所有错误
③ Builder.compile(ast, regs)      →  Recipe         绑定访问器/字面量/getter
       └→ RecipeStore.publish(name, recipe)          原子替换发布
```

设计要点:

- **解析与校验分离**:解析只管语法对不对,校验只管引用存不存在、类型匹不匹配。职责单一,各自可独立测试。校验时**一次性收集全部错误**(而非遇到第一个就停),用户改配置一次看全所有问题 —— "易上手"的体验关键。
- **校验检查项(穷举):**
  - 占位符引用的 name 在 `NameRegistry` / `DeviceCtx` 中存在
  - 引用的连接符在 `ConnectorLib` 中存在
  - 结构体块 name 指向的子配方确实存在、无循环引用(检测 recipe 间依赖环)
  - 类型匹配(如时间戳字段配了对应格式器)
- **Builder 绑定而非查找**:校验过的 AST 里每处引用,Builder 把它替换成可直接执行的绑定(`ValueProvider*` / 字面量 / `DeviceGetter` / 导航 + 子配方指针)。这一步过后,执行路径上再无任何名字字符串。
- **发布原子**:`RecipeStore` 内部按 name 维护 `std::shared_ptr<const Recipe>`,热加载时 `store(name, newRecipe)` 原子替换。调用方拿 `shared_ptr` 快照,遍历期间旧版不被释放 —— 无锁热加载标准做法。

## 5. 错误处理

加载失败**绝不半发布**:任一段失败 → 该配方不进 `RecipeStore`,旧版继续服务。

| 阶段 | 失败处理 |
|---|---|
| ① parse | 语法错 → 返回错误清单(行号 + 原因),不进 ② |
| ② validate | 收集所有引用/类型/循环错误 → 全部返回,不进 ③ |
| ③ compile | 校验已过则理论上必成功;防御性检查仍返回错误,不发布 |
| publish | 不失败(原子替换) |

调用方永远只能拿到**完整有效的 Recipe** 或**旧版本**之一,不存在"半成品配方"对外服务。

运行时 `render` 的防御:配方名不存在 → 返回空串或既定错误标识(不崩溃);结构体指针为空且无导航保护 → 返回错误标识。具体策略实现时定,原则是**热路径绝不抛异常、绝不崩溃**。

## 6. 扩展点与"易上手"接口

### 6.1 开发者扩展面(加新结构体/字段时动代码)

遵循"改词汇表要写代码,改配方只改配置"。扩展点全部通过**注册宏 + 模板**暴露,无需改框架核心:

```cpp
// 启动期:填充三个注册表(框架核心一无所知具体业务)

// 标量字段:绑结构体类型 + 字段 + 格式器
REGISTER_FIELD("time", Event, timestamp, "%llu");
REGISTER_FIELD("name", PersonInfo, name,  "%s");
REGISTER_FIELD("age",  PersonInfo, age,   "%d");
REGISTER_FIELD("x",    Box, x, "%d");   // y/w/h 同理

// 结构体块:绑导航路径 + 子配方名(子配方在同份配置里定义)
REGISTER_STRUCT("person", Event, person, "person");   // → person 子配方
REGISTER_STRUCT("rect",   Event, rect,   "rect");     // → rect 子配方

// 连接符库:服务提供
REGISTER_CONNECTOR("dash", "-");
REGISTER_CONNECTOR("colon", ":");

// 设备信息接口:启动时定义 getter,值运行时带
// (DeviceCtx 是用户提供的类型,框架只认其 getter)
```

扩展场景的工作流(全部隔离,互不影响):

| 要加什么 | 动什么 | 框架核心 |
|---|---|---|
| 新字段 | 加一行 `REGISTER_FIELD` | 不动 |
| 新结构体块 | 加 `REGISTER_STRUCT` + 配置加子配方 | 不动 |
| 新连接符 | 加一行 `REGISTER_CONNECTOR` | 不动 |
| 新设备信息 | `DeviceCtx` 加 getter + 注册 | 不动 |
| 新组装格式 | **只改 JSON 配置,不重编译** | 不动 |

后两行是"易上手 / 易拓展"的精髓 —— **新增一种输出格式零编译**,加新结构体也只是加注册行,框架核心 `Engine` / `Recipe` / `Step` 全程不知情。

### 6.2 用户使用面(最小心智负担)

用户只需懂三件事:

1. `${name}` 引用任何已注册的东西(字段、结构体块、设备信息 —— 语法统一,不区分)。
2. 其余字符是连接符,原样输出。
3. 结构体块内部用同名子配方展开(用户写子配方和写主配方没区别)。

不需要懂:访问器、导航、虚函数、Recipe、Step、注册表内部、热加载。这些全是开发者/框架内部的事。

### 6.3 公开 API(最简)

```cpp
class Engine {
public:
    // 启动期:注入已填充好的两个注册表(NameRegistry, ConnectorLib)
    Engine(NameRegistry&, ConnectorLib&);

    // 运行期:加载/重载配置 → 编译 → 原子发布;返回错误清单(成功则空)
    LoadResult loadConfig(std::string_view jsonText);

    // 运行期:渲染 —— 唯一热路径调用,只读快照、无锁、零查找、零解析
    std::string render(const std::string& recipeName,
                       const void* structPtr,
                       const DeviceCtx& ctx) const;
};
```

`loadConfig` 返回 `LoadResult`(成功/失败 + 全量错误清单),让运维一次性看全问题。`render` 是只读热路径。

## 7. 框架边界与依赖

- **C++17**(`std::variant` / `std::string_view` / `std::shared_ptr` 可用)。
- **JSON 解析依赖**:用轻量单文件库(如 cJSON / nlohmann::json),不引入大依赖。解析只在加载时发生,热路径零解析。
- **无外部运行时依赖**:热路径纯标准库 + 内存操作,适合安防嵌入式同进程场景。
- **线程模型**:`RecipeStore` 用 `std::shared_ptr` 原子替换实现无锁热加载;`Engine::render` 只读快照,多线程可并发 render 同一/不同配方,互不阻塞。

## 8. 测试策略

- **解析层**:各类 JSON 语法(合法/非法模板、转义、空配置)单测。
- **校验层**:引用不存在的 name、类型不匹配、循环子配方、缺连接符 —— 每类错误独立用例,**验证全量错误收集**(一次返回多个错误)。
- **编译层**:AST→Recipe 绑定正确性,绑定的访问器/getter 指向预期。
- **执行层**:完整安防示例(Event/PersonInfo/Box 嵌套),验证拼接结果字节级正确;嵌套递归(结构体块内再嵌结构体块)。
- **热加载**:并发 render 同时替换配方,验证旧版读者不被破坏、新版即时生效、无内存竞争(用线程消毒器验证)。
- **基准测试(对应 3.2 双轨)**:路线 A(显式四类 Step)vs 路线 B(统一 ValueProvider*),在真实安防结构体规模下对比热路径耗时,以此决断 Step 表示方式。基准结果记入实现说明。

## 9. 端到端示例

上游 C 结构体:

```c
typedef struct {
    uint64_t timestamp;          // 时间戳
    PersonInfo* person;          // 人体信息(关键结构体)
    Box rect;                    // 检测框(关键结构体)
} Event;

typedef struct {
    char name[32];
    int   age;
} PersonInfo;

typedef struct {
    int x, y, w, h;
} Box;
```

> 设备信息(如 camera_id)不放进业务结构体,归 `DeviceCtx` —— 它是"服务提供"的通用设备信息,与具体事件结构体解耦。

开发者注册:

```cpp
REGISTER_FIELD("time",    Event, timestamp, "%llu");
REGISTER_STRUCT("person", Event, person,  "person");
REGISTER_STRUCT("rect",   Event, rect,    "rect");
REGISTER_DEVICE("camera", DeviceCtx, camera_id);   // 设备信息,运行时带值
```

用户配置:

```json
{
  "recipes": [
    { "name": "alarm_line", "template": "${camera}:${time}|${person}@${rect}" },
    { "name": "person",     "template": "${name}-${age}" },
    { "name": "rect",       "template": "${x},${y},${w},${h}" }
  ]
}
```

调用与产出:

```
render("alarm_line", &event, deviceCtx)
→ "CAM001:1717171717|张三-30@100,200,300,400"
```

其中 `${person}` 整块展开为 `张三-30`(子配方),`${rect}` 整块展开为 `100,200,300,400`(子配方)。

## 10. 明确的 YAGNI / 不做项

- 不做条件分支、循环、表达式(配置保持极简模板语法)。
- 不做跨文件 include(配方同文件混排)。
- 不做每请求重新解析传进来的配置文本(加载时编译,运行时零解析)。
- 不做 codegen / JIT(与动态加载 + 安防嵌入式场景冲突)。
- Step 表示方式(A/B 双轨)不在纸面锁死,待基准决断。

## 11. 待决断项(交由实现阶段 + 基准测试)

1. **Step 内部表示**:路线 A(显式四类)vs 路线 B(统一 `ValueProvider*`)—— 双轨实现,基准对比决断。
2. **`Step` 绑定布局**:`union` vs `std::variant` —— 实现时择一。
3. **`render` 防御策略**:配方名不存在 / 空指针导航时返回空串还是错误标识 —— 实现时定。
4. **JSON 库选型**:cJSON vs nlohmann::json —— 实现时按部署环境择一。
