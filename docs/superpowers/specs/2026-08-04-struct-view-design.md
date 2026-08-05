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
| name 注册机制 | 构建期 codegen 生成"名字词汇表":描述文件(JSON)→ Python 工具 → `REGISTER_*` 代码(引用 `&T::field`,编译期校验);配置只写"配方"(运行时热加载)。两者正交 |
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
| `NameRegistry` | name → `ValueProvider`(标量字段访问器 或 复合结构块) | 启动时由 **codegen 生成的代码**注册(见 6.1),需 C 类型知识 |
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
    Navigator nav;                          // 从父指针导航到子结构体指针(如 &event->person)
    std::shared_ptr<const Recipe> subRecipe; // 这块结构体自己的组装规则(引用计数,见下)
    /* get(): nav(structPtr)→拿子指针→engine 跑 *subRecipe→返回整块字符串 */
};
```

`StructBlockProvider` 持有子 `shared_ptr<const Recipe>` —— 递归嵌套的物理基础,也是热加载并发安全的关键(见 §3.4a)。结构体块本质上是"内嵌的、有导航入口的子执行器"。

### 3.4a 所有权与并发安全(RCU 模型)—— 必读

热加载要满足:**加载时换配方,正在跑的 render 不被打断、不撕裂**。这要求每个配方版本是一张**自包含、不可变、引用计数**的图,读者靠 `shared_ptr` 快照保活旧版,直到跑完才释放。三条规定:

1. **结构体块绑定配方内私有,不全局共享重绑。** NameRegistry 注册的 StructBlockProvider 只声明"导航 + 子配方名"(供校验);Builder 编译时为**每份配方**实例化**自己的** StructBlockProvider,持有 `shared_ptr<const Recipe>` 指向**自己的**子配方。重载时新配方拥有新 provider + 新子配方;旧配方被读者快照保活 → 它拥有的旧 provider + 旧子配方一起保活。**绝不重绑共享对象的 `sub_`**(否则并发 render 会读到"旧父 + 新子"的撕裂输出,且 `sub_` 读/写本身是数据竞争)。

2. **Engine 持有稳定 store,按配方 publish,不整体替换 store。** `store_` 是一个长期存活的 `RecipeStore` 对象(其 `shared_mutex` + 内部表从不被销毁)。`loadConfig` 成功后对每个配方调 `store_.publish(name, recipe)`(写锁下原子换该 name 的 `shared_ptr`);**禁止** `store_ = std::move(...)` 整体替换 store(会销毁旧 store 的锁,而读者正加着读锁 → 崩溃)。

3. **配方对象不可变。** `Recipe` 发布后只读;所有可变状态(子配方指针)在编译期一次性绑定,运行期只读。读者快照拿到的 `shared_ptr<const Recipe>` 保证整张图(父 + 其结构体块 + 子配方)引用计数保活,直至所有在途 render 跑完才释放。

> 这三条共同构成标准 RCU:publish 只换指针不改对象,snapshot 只读不改对象,旧图靠引用计数延后释放。spec §7 的"无锁热加载"由此成立。

### 3.5 三个注册表

```cpp
// 结构体块声明(注册表里存这个,供 Validator 校验 + Builder 编译用)
struct StructBlockDecl {
    Navigator nav;                 // 父→子结构体指针导航
    std::string subRecipeName;     // 该块展开时跑哪个子配方(编译期由 Builder 解析为 shared_ptr)
};
// name → ValueProvider(标量字段) 或 结构体块声明(nav + 子配方名),加载时校验用
// 注意:register* 调用不由人手写 —— 由构建期 codegen 从描述文件生成(见第 6.1 节)
// 注意:结构体块在此只存"声明"(导航 + 子配方名),供 Validator 校验存在性/环路;
//       编译期 Builder 为每份配方实例化自己的 StructBlockProvider(配方内私有,见 §3.4a)。
class NameRegistry {
    std::unordered_map<std::string, std::unique_ptr<ValueProvider>> entries_;       // 标量字段
    std::vector<std::pair<std::string, StructBlockDecl>> structDecls_;              // 结构体块声明
public:
    void registerField (std::string name, /* 字段模板参数 */);
    void registerStruct(std::string name, Navigator nav, std::string subRecipeName); // 仅声明
    const ValueProvider* lookup(const std::string& name) const;  // 标量字段,加载时校验用
    const std::vector<std::pair<std::string, StructBlockDecl>>& structDecls() const; // Builder/Validator 用
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
- **发布原子(按配方,非整体替换)**:`RecipeStore` 是 Engine 持有的长期对象(其 `shared_mutex` 与内部表从不被销毁),内部按 name 维护 `std::shared_ptr<const Recipe>`。`loadConfig` 成功后 Builder 对每个配方调 `store.publish(name, newRecipe)`(写锁下原子换该 name 的 `shared_ptr`);**严禁** `store = std::move(...)` 整体替换 store 对象。调用方 `snapshot` 拿 `shared_ptr` 副本,遍历期间旧配方整张图不被释放 —— RCU 无锁热加载(详见 §3.4a)。

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

### 6.1 词汇表生成:描述文件 + 构建期 codegen

词汇表(`NameRegistry` 的填充)不由人手写 `REGISTER_*` 宏 —— 那是堆易错样板代码。改为**单一声明式描述文件 + 构建期 Python codegen 工具**自动生成注册代码。

**两段式生成链:**

```
上游 .h (结构体布局源)          ← 真理之源,人只读不改
   │  开发者照 .h 手写镜像
   ▼
描述文件 (schema.json)          ← 声明式:字段布局 + 元数据(友好名/格式器/块标注)
   │  构建期:python gen_registry.py schema.json
   ▼
registry_gen.cpp               ← 生成的 REGISTER_* 调用(引用 &T::field)
   │  C++ 编译
   ▼
启动期填充 NameRegistry          ← 运行时对象,与 recipe 热加载正交
```

**描述文件格式(JSON,与 recipe 配置同系,复用一套解析心智):**

```json
{
  "structs": [
    {
      "name": "Event",
      "members": [
        { "field": "timestamp", "as": "time", "fmt": "%llu" },
        { "block": "person",    "subRecipe": "person" },
        { "block": "rect",      "subRecipe": "rect" }
      ]
    },
    {
      "name": "PersonInfo",
      "members": [
        { "field": "name", "as": "name", "fmt": "%s" },
        { "field": "age",  "as": "age",  "fmt": "%d" }
      ]
    },
    {
      "name": "Box",
      "members": [
        { "field": "x", "fmt": "%d" }, { "field": "y", "fmt": "%d" },
        { "field": "w", "fmt": "%d" }, { "field": "h", "fmt": "%d" }
      ]
    }
  ],
  "device": [
    { "getter": "cameraId", "as": "camera" }
  ]
}
```

字段语义:

- `field` = 真实 C 成员名(codegen 据此写 `&Struct::field`);`as` = 暴露给配方的友好名,省略则默认取 `field`。
- `block` = 结构体块,带 `subRecipe` 指向同份 recipe 配置里的子配方名。**指针 vs 内嵌由 codegen 用模板自动判定**(描述文件不写,减小心智负担)—— codegen 生成 `REGISTER_STRUCT` 时根据成员类型选择内嵌导航或指针解引用。
- `device` = 设备信息 getter(codegen 生成 `REGISTER_DEVICE`)。
- `fmt` = 标量格式器(`%llu` / `%s` / `%d` …)。

**codegen 产物(示意,registry_gen.cpp 头注"请勿手改"):**

```cpp
// 自动生成 — 请勿手改
REGISTER_FIELD ("time",   Event,      timestamp, "%llu");
REGISTER_STRUCT("person", Event,      person,    "person");
REGISTER_STRUCT("rect",   Event,      rect,      "rect");
REGISTER_FIELD ("name",   PersonInfo, name,      "%s");
REGISTER_FIELD ("age",    PersonInfo, age,       "%d");
REGISTER_FIELD ("x",      Box,        x,         "%d");
REGISTER_FIELD ("y",      Box,        y,         "%d");
REGISTER_FIELD ("w",      Box,        w,         "%d");
REGISTER_FIELD ("h",      Box,        h,         "%d");
REGISTER_DEVICE("camera", DeviceCtx,  cameraId);
```

**编译期同步兜底(关键安全保障):** 生成的代码引用真实字段 `&T::field`。上游 `.h` 一旦改字段名 / 删字段 / 改类型 → 生成代码编译失败 → 漂移即时暴露在构建期,**不会带到运行时**。描述文件与 `.h` 的一致性由 C++ 编译器兜底,无需运行时反射或偏移手填。

**为何不直接解析 .h:** 解析真实 C 头文件需 libclang(重)或自写解析器(脆弱,宏/预处理难处理),且 `.h` 不含格式器 / 块标注等元数据,仍需补注解。手写镜像描述文件 + 编译期校验,是最轻量且类型安全的折中 —— 多一份镜像,换回零 C 解析依赖 + 编译期漂移兜底。

### 6.2 开发者扩展工作流

扩展场景的工作流(全部隔离,互不影响):

| 要加什么 | 动什么 | 重编译? | 框架核心 |
|---|---|---|---|
| 新字段 | 描述文件加一行 `field` | 是(codegen→编译,`&T::field` 校验) | 不动 |
| 新结构体块 | 描述文件加 `block` + recipe 配置加子配方 | 是 | 不动 |
| 新连接符 | `ConnectorLib` 加一行 | 是 | 不动 |
| 新设备信息 | `DeviceCtx` 加 getter + 描述文件 `device` 加一行 | 是 | 不动 |
| **新组装格式** | **只改 recipe JSON 配置** | **否(热加载)** | 不动 |

结构体布局相对静态(上游改结构体本就要重编译),故词汇表走构建期 codegen 合理;配方频繁变更,走运行时热加载。两者各得其所。最后一行是"易上手 / 易拓展"的精髓 —— **新增一种输出格式零编译**;加新结构体也只是描述文件加一行 + 重跑 codegen,框架核心 `Engine` / `Recipe` / `Step` 全程不知情。

### 6.3 用户使用面(最小心智负担)

用户只需懂三件事:

1. `${name}` 引用任何已注册的东西(字段、结构体块、设备信息 —— 语法统一,不区分)。
2. 其余字符是连接符,原样输出。
3. 结构体块内部用同名子配方展开(用户写子配方和写主配方没区别)。

不需要懂:访问器、导航、虚函数、Recipe、Step、注册表内部、codegen、热加载。这些全是开发者/框架内部的事。

### 6.4 公开 API(最简)

```cpp
class Engine {
public:
    // 启动期:注入已填充好的两个注册表
    //   NameRegistry —— 由 codegen 生成的 initNameRegistry() 填充(见 6.1)
    //   ConnectorLib —— 启动期代码填充
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
- **构建期 codegen 工具**:Python 脚本(`gen_registry.py`),构建期运行,读描述文件产出 `registry_gen.cpp`。仅构建期依赖,不进运行时。
- **无外部运行时依赖**:热路径纯标准库 + 内存操作,适合安防嵌入式同进程场景。
- **线程模型(RCU)**:每份配方是不可变、引用计数的图(父配方持有其结构体块的 `shared_ptr`,结构体块持有子配方的 `shared_ptr`)。`RecipeStore` 是 Engine 持有的长期对象,`publish` 在写锁下原子换某 name 的 `shared_ptr`;`snapshot` 在读锁下返回 `shared_ptr` 副本(读者之间不互斥)。在途 render 持旧配方快照,旧图靠引用计数保活至跑完才释放。`Engine::render` 只读快照,多线程可并发 render 同一/不同配方,与 `loadConfig` 并发亦互不阻塞、不撕裂(三条 RCU 规定见 §3.4a)。务必遵守:① 结构体块绑定配方内私有,不重绑共享对象;② store 稳定,按配方 publish,不整体替换 store;③ 配方对象发布后只读。

## 8. 测试策略

- **解析层**:各类 JSON 语法(合法/非法模板、转义、空配置)单测。
- **校验层**:引用不存在的 name、类型不匹配、循环子配方、缺连接符 —— 每类错误独立用例,**验证全量错误收集**(一次返回多个错误)。
- **编译层**:AST→Recipe 绑定正确性,绑定的访问器/getter 指向预期。
- **执行层**:完整安防示例(Event/PersonInfo/Box 嵌套),验证拼接结果字节级正确;嵌套递归(结构体块内再嵌结构体块)。
- **热加载**:并发 render 同时替换配方,验证旧版读者不被破坏、新版即时生效、无内存竞争(用线程消毒器验证)。
- **codegen 层(对应 6.1)**:描述文件 → 生成的 `registry_gen.cpp` 内容正确(字段名/格式器/块标注映射);生成的代码引用的 `&T::field` 在 `.h` 改名后**编译失败**(验证漂移兜底);指针 vs 内嵌结构体块的导航代码分支正确。
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

开发者照 `.h` 写描述文件(schema.json,见 6.1):

```json
{
  "structs": [
    { "name": "Event", "members": [
        { "field": "timestamp", "as": "time", "fmt": "%llu" },
        { "block": "person",    "subRecipe": "person" },
        { "block": "rect",      "subRecipe": "rect" }
    ]},
    { "name": "PersonInfo", "members": [
        { "field": "name", "fmt": "%s" },
        { "field": "age",  "fmt": "%d" }
    ]},
    { "name": "Box", "members": [
        { "field": "x", "fmt": "%d" }, { "field": "y", "fmt": "%d" },
        { "field": "w", "fmt": "%d" }, { "field": "h", "fmt": "%d" }
    ]}
  ],
  "device": [ { "getter": "cameraId", "as": "camera" } ]
}
```

构建期 codegen(`python gen_registry.py schema.json`)产出 `registry_gen.cpp`:

```cpp
// 自动生成 — 请勿手改
REGISTER_FIELD ("time",   Event,      timestamp, "%llu");
REGISTER_STRUCT("person", Event,      person,    "person");
REGISTER_STRUCT("rect",   Event,      rect,      "rect");
REGISTER_FIELD ("name",   PersonInfo, name,      "%s");
REGISTER_FIELD ("age",    PersonInfo, age,       "%d");
REGISTER_FIELD ("x",      Box,        x,         "%d");
REGISTER_FIELD ("y",      Box,        y,         "%d");
REGISTER_FIELD ("w",      Box,        w,         "%d");
REGISTER_FIELD ("h",      Box,        h,         "%d");
REGISTER_DEVICE("camera", DeviceCtx,  cameraId);   // 设备信息,运行时带值
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
- 不做**运行时** codegen / JIT(与动态加载 + 安防嵌入式场景冲突)。注意:构建期 codegen(描述文件→`registry_gen.cpp`)**做**,且不冲突 —— 它只是构建期代码生成器产出普通 .cpp,不涉及运行时编译/JIT。
- 不直接解析真实 C `.h` 提取布局(用手写镜像描述文件 + 编译期 `&T::field` 校验兜底,免 C 解析依赖)。
- Step 表示方式(A/B 双轨)不在纸面锁死,待基准决断。

## 11. 待决断项(交由实现阶段 + 基准测试)

1. **Step 内部表示**:路线 A(显式四类)vs 路线 B(统一 `ValueProvider*`)—— 双轨实现,基准对比决断。
2. **`Step` 绑定布局**:`union` vs `std::variant` —— 实现时择一。
3. **`render` 防御策略**:配方名不存在 / 空指针导航时返回空串还是错误标识 —— 实现时定。
4. **JSON 库选型**:cJSON vs nlohmann::json —— 实现时按部署环境择一。
5. **codegen 指针/内嵌自动判定**:结构体块成员是 `T*`(指针)还是 `T`(内嵌)的判定来源 —— 从描述文件显式标注,还是 codegen 读 `.h` 推断(若读 .h 则引入轻量 C 解析,违背 6.1"不解析 .h"原则;倾向描述文件显式标注 `ptr: true/false`)。
