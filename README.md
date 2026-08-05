# struct_view

> C/C++ 同进程的「结构体 → 字符串」组装框架,面向安防场景视频结构化提取数据。

上游(安防视频结构化提取)产出 C 结构体数据;本框架按用户在 JSON 配置里描述的**组装配方**(字段顺序 + 连接符 + 设备信息),把结构体数据自动转成字符串,并支持运行时热加载配方。核心诉求:**易拓展、架构良好、用户易上手**。

---

## 核心特性

- **加载时编译、运行时直线执行** —— 配置在加载时解析→校验→编译成不可变 `Recipe`,运行时热路径零解析、零名字查找。
- **name 两层模型** —— name 既能绑单个标量字段,也能绑一整块「关键结构体」(子配方递归嵌套);对用户统一为 `${name}` 占位符。
- **RCU 无锁热加载** —— 换配方不打断在途 render、不撕裂输出、读者之间不互斥(TSan 验证 0 数据竞争)。
- **构建期 codegen** —— 描述文件 `schema.json` → Python 工具 → `registry_gen.cpp`,引用真实 `&T::field`,结构体布局漂移即时编译失败。
- **双轨 Step 表示 + 基准决断** —— Route A(显式四类 Step)与 Route B(统一 `ValueProvider*`)并存,基准对比后选定默认路径。

---

## 架构总览

框架分**两个时间维度**:

```
加载时(低频,运维触发)                    运行时(高频,每帧调用)
┌─────────────────────────────┐         ┌──────────────────────────┐
│ JSON 配置文本                │         │ render(recipe, struct, ctx)│
│   ↓ ConfigLoader.parse       │         │   ↓ snapshot (shared_lock) │
│ ConfigAst(未校验)            │         │ 不可变 Recipe 快照          │
│   ↓ Validator.validate       │         │   ↓ 直线遍历 Step[]         │
│ 校验(收集全部错误 + 环检测) │         │   ↓ 拼接 → string           │
│   ↓ Builder.compile          │         └──────────────────────────┘
│ 不可变 Recipe(引用计数图)   │
│   ↓ RecipeStore.publishAll   │  原子换 map(一写锁)
└─────────────────────────────┘
```

三个注册表(启动期由代码 / codegen 填充):

| 注册表 | 内容 | 何时填 |
|---|---|---|
| `NameRegistry` | name → 标量字段 provider;结构体块声明(nav + 子配方名) | codegen 生成的 `registerStructViewNames()` |
| `ConnectorLib` | 连接符名 → 字面量/格式化器(「服务提供」) | 启动期代码 `lib.add(...)` |
| `DeviceCtx` | 设备信息 getter 接口(值运行时带入) | 用户子类化,每次 render 传实例 |

---

## 目录结构

```
struct_view/
├── CMakeLists.txt              # 构建脚本:glob src/tests,每测试一 exe + security_example + bench_routes
├── README.md                   # 本文件
├── required.md                 # 原始需求(中文)
│
├── include/struct_view/        # 公共头(框架 API,全部 #pragma once,命名空间 sv)
│   ├── Errors.hpp              #   ConfigError / LoadResult —— 错误模型(全量收集)
│   ├── DeviceCtx.hpp           #   sv::DeviceCtx 基类(用户子类化加 getter)
│   ├── ValueProvider.hpp       #   ★ 类型擦除取值层次:ValueProvider/LambdaProvider/makeProvider/Navigator/ConnectorProvider/StructBlockProvider
│   ├── Recipe.hpp              #   ★ Recipe 表示:Route B(StepB/RecipeB)+ Route A(StepA/RecipeA/StepKind + 四种 Binding)
│   ├── RecipeRunner.hpp        #   runRecipeB / runRecipeA 自由函数(引擎 + 结构体块递归共用)
│   ├── RecipeStore.hpp         #   ★ 模板 RecipeStore<RecipeT>:pimpl + shared_mutex,publish/publishAll/snapshot(RCU 热加载)
│   ├── NameRegistry.hpp        #   名字词汇表:registerProvider/registerStruct + StructBlockDecl + lookup/structDecls
│   ├── ConnectorLib.hpp        #   连接符库:name → literal
│   ├── ConfigAst.hpp           #   配置 AST:Segment{isRef,text} / RecipeAst / ConfigAst
│   ├── ConfigLoader.hpp        #   JSON 解析 + ${name} 模板分词(纯语法,不查注册表)
│   ├── Validator.hpp           #   校验:全量错误收集 + 结构体块依赖环路检测
│   ├── Builder.hpp             #   ★ Route B 编译器:AST → RecipeB(3-pass,配方内私有结构体块)
│   ├── BuilderA.hpp            #   Route A 编译器:AST → RecipeA(显式四类 Step)
│   └── Engine.hpp              #   ★ 公开 API:loadConfig/render(Route B)+ loadConfigA/renderA(Route A)
│
├── src/                        # 实现(一头一 cpp,职责单一)
│   ├── ValueProvider.cpp       #   仅 StructBlockProvider::get(导航 + 跑子配方)
│   ├── RecipeRunner.cpp        #   runRecipeB / runRecipeA(引擎遍历核心)
│   ├── NameRegistry.cpp        #   注册表实现
│   ├── ConnectorLib.cpp        #   连接符库实现
│   ├── ConfigLoader.cpp        #   nlohmann/json 解析 + 模板分词器
│   ├── Validator.cpp           #   三项检查 + DFS 环检测
│   ├── Builder.cpp             #   ★ Route B 3-pass 编译
│   ├── BuilderA.cpp            #   Route A 3-pass 编译
│   └── Engine.cpp              #   ★ loadConfig 流水线 + render 热路径
│
├── tools/
│   └── gen_registry.py         # ★ 构建期 codegen:schema.json → registry_gen.cpp
│
├── examples/security/          # 端到端安防示例(Event/PersonInfo/Box)
│   ├── event.h                 #   上游 C 结构体(timestamp + PersonInfo* + Box)
│   ├── device_ctx.h            #   MyDeviceCtx : sv::DeviceCtx(cameraId())
│   ├── schema.json             #   描述文件(字段/格式器/结构体块标注 ptr)
│   ├── recipes.json            #   用户配方(alarm_line/person/rect)
│   ├── registry_gen.cpp        #   codegen 产物(已入库,可由 custom command 重生成)
│   └── main.cpp                #   装配 + 渲染 demo
│
├── bench/
│   └── bench_routes.cpp        # Route A vs Route B 基准(2M 次 render,Release -O2)
│
├── tests/                      # doctest 单测 + 脚本测试(每文件一 exe,共享 main.cpp)
│   ├── main.cpp                #   共享 doctest main(DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
│   ├── test_scaffold.cpp       #   脚手架(验证 vendored 头可编译)
│   ├── test_value_provider.cpp #   ValueProvider/makeProvider/Navigator/ConnectorProvider
│   ├── test_connector_lib.cpp  #   连接符库
│   ├── test_name_registry.cpp  #   名字注册表
│   ├── test_config_loader.cpp  #   JSON 解析 + 分词
│   ├── test_validator.cpp      #   全量错误收集 + 环检测
│   ├── test_recipe_store.cpp   #   publish/snapshot/publishAll 原子性
│   ├── test_builder.cpp        #   Route B 编译 + 结构体块绑定
│   ├── test_engine.cpp         #   loadConfig + render 端到端
│   ├── test_hot_reload.cpp     #   ★ 并发热加载(render × 4 线程 + 200 次重载,TSan 验证)
│   ├── test_example_e2e.cpp    #   安防示例精确输出断言
│   ├── test_route_a.cpp        #   Route A 与 Route B 输出一致性(parity)
│   ├── test_codegen.py         #   codegen 输出形状(类型转换/ptr/设备下转)
│   ├── test_drift.sh           #   ★ 漂移检测:头文件改名 → 生成代码编译失败
│   ├── drift_header.h          #   漂移测试基线头(被脚本就地改写)
│   ├── drift_schema.json       #   漂移测试 schema
│   ├── drift_gen.cpp           #   ⚠ 漂移测试运行时生成(未入库,gitignore 忽略)
│   └── drift_main.cpp          #   ⚠ 同上(运行时生成)
│
├── third_party/                # vendored 单文件库(仅构建期/测试用,热路径零依赖)
│   ├── nlohmann/json.hpp       #   JSON 解析(仅加载时)
│   └── doctest/doctest.h       #   测试框架
│
└── docs/superpowers/
    ├── specs/
    │   ├── 2026-08-04-struct-view-design.md             # 设计 spec(含 §3.4a RCU 三条规定)
    │   └── 2026-08-04-struct-view-benchmark-result.md   # 基准结果 + 路径决断
    └── plans/
        └── 2026-08-05-struct-view.md                    # 实现计划(16 任务)
```

> `tests/drift_gen.cpp`、`tests/drift_main.cpp` 由 `test_drift.sh` 运行时生成,不入库。

---

## 关键功能实现提取

### 1. 加载时编译流水线(parse → validate → compile → publish)

`Engine::loadConfig` 串起四段,任一段失败立即返回错误、**绝不半发布**(旧配方继续服务):

```cpp
// src/Engine.cpp
LoadResult Engine::loadConfig(std::string_view jsonText) {
    LoadResult lr;
    auto parsed = ConfigLoader::parse(jsonText);          // ① 纯语法
    if (!parsed.ok) { lr.errors.push_back({"", "parse error: " + parsed.parseError, 0}); return lr; }
    auto verrors = Validator::validate(parsed.ast, names_, connectors_);  // ② 全量校验
    if (!verrors.empty()) { lr.ok = false; lr.errors = std::move(verrors); return lr; }
    auto built = Builder::compile(parsed.ast, names_, connectors_);       // ③ 编译成不可变 Recipe
    if (!built.ok) { lr.ok = false; lr.errors = std::move(built.errors); return lr; }
    store_.publishAll(std::move(built.recipes));           // ④ 原子发布(见下)
    lr.ok = true;
    return lr;
}
```

- **解析与校验分离**:`ConfigLoader` 只管语法(JSON 合法 + `recipes` 数组),`Validator` 只管引用存不存在。职责单一,各自可独立测试。
- **全量错误收集**:`Validator` 一次性返回所有错误(而非遇到第一个就停),用户改配置一次看全问题。

### 2. name 两层模型:标量字段 + 结构体块递归

name 指向「一段可执行的取值逻辑」,分两类,**对用户统一为 `${name}`**:

**类型 A — 标量字段**:`makeProvider(lambda)` 包裹一个 `(const void*, const DeviceCtx&) -> string` 的 lambda(codegen 按字段类型生成 `snprintf` 转换)。

**类型 B — 结构体块(关键结构体)**:`StructBlockProvider` 持有导航器 + 子配方,`get()` 导航到子结构体指针后跑子配方(递归):

```cpp
// include/struct_view/ValueProvider.hpp
class StructBlockProvider : public ValueProvider {
    Navigator nav_;                          // 父→子结构体指针
    std::shared_ptr<const RecipeB> sub_;     // 这块结构体自己的组装规则(引用计数,见 §3)
public:
    StructBlockProvider(Navigator nav, std::shared_ptr<const RecipeB> sub)
        : nav_(std::move(nav)), sub_(std::move(sub)) {}
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

// src/ValueProvider.cpp
std::string StructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    const void* child = nav_.navigate(structPtr);
    return runRecipeB(*sub_, child, ctx);    // 跑子配方 → 整块字符串
}
```

子配方里还能再引用结构体块 name → 天然递归嵌套(如 `person` 里嵌 `address`)。

### 3. RCU 无锁热加载(并发安全核心)

这是框架最关键也最易错的部分。要求:**换配方时,正在跑的 render 不被打断、不撕裂、不崩溃,render 之间不互斥**。

做法是标准 RCU:**配方是不可变、引用计数的图;只换指针,不换对象;旧图靠 `shared_ptr` 延后释放**。三条规定(spec §3.4a):

**(1) 结构体块绑定配方内私有,不全局共享重绑。**
Builder 编译时为**每份配方**实例化自己的 `StructBlockProvider`(存在 `RecipeB::ownedProviders`),`sub_` 用 `shared_ptr<const RecipeB>` 指向**自己的**子配方,编译期一次性绑定、运行期只读。重载时新配方拥有新 provider + 新子配方;旧配方被读者快照保活 → 它拥有的旧 provider + 旧子配方一起保活。**绝不重绑共享对象**(否则并发 render 会读到「旧父 + 新子」撕裂输出)。

```cpp
// src/Builder.cpp —— Pass 2:为每个结构体块引用创建配方内私有 provider
for (auto& p : pending) {
    const StructBlockDecl* decl = findDecl(names, p.blockName);
    auto it = byName.find(decl->subRecipeName);
    // shared_ptr 拷贝:父配方现在共拥有其子配方(引用计数 +1)
    auto sbp = std::make_unique<StructBlockProvider>(decl->nav, it->second);
    p.rb->steps[p.stepIdx].provider = sbp.get();
    p.rb->ownedProviders.push_back(std::move(sbp));   // 由该配方独占拥有
}
```

**(2) Engine 持有稳定 store,按配方 publishAll,不整体替换 store。**
`store_` 是一个长期存活的 `RecipeStore` 对象(其 `shared_mutex` + 内部表**从不被销毁**)。`publishAll` 在**一把写锁**下原子换整张 map:

```cpp
// include/struct_view/RecipeStore.hpp —— pimpl 让 store 可移动(shared_mutex 不可移动)
template <typename RecipeT>
class RecipeStore {
    struct Impl {
        std::unordered_map<std::string, std::shared_ptr<const RecipeT>> map;
        mutable std::shared_mutex mtx;
    };
    std::unique_ptr<Impl> pimpl_;
public:
    void publishAll(std::unordered_map<std::string, std::shared_ptr<const RecipeT>> newMap) {
        std::unique_lock<std::shared_mutex> lk(pimpl_->mtx);
        pimpl_->map = std::move(newMap);    // 原子换整张 map,无半发布
    }
    std::shared_ptr<const RecipeT> snapshot(const std::string& name) const {
        std::shared_lock<std::shared_mutex> lk(pimpl_->mtx);   // 读者不互斥
        auto it = pimpl_->map.find(name);
        return it == pimpl_->map.end() ? nullptr : it->second;  // 拷贝 shared_ptr,refcount+1
    }
};
```

> 关键反例(已修复的 bug):早期版本写 `store_ = std::move(built.store)` 整体替换 store 对象,会销毁旧 store 的 `shared_mutex` —— 而读者正加着读锁 → 崩溃。TSan 抓到该数据竞争后改为 `publishAll`。

**(3) 配方对象不可变。** `shared_ptr<const RecipeB>` 在类型层面保证发布后只读;所有可变状态在编译期绑定。读者 `snapshot` 拿到的 `shared_ptr` 副本让整张图(父 + 其结构体块 + 子配方)保活,直到所有在途 render 跑完才释放。

并发验证见 `tests/test_hot_reload.cpp`(4 线程持续 render + 主线程 200 次重载,TSan 0 race)。

### 4. 构建期 codegen + 编译期漂移兜底

`tools/gen_registry.py` 读 `schema.json`,产出 `registry_gen.cpp`(`void registerStructViewNames(sv::NameRegistry&)`)。字段带 `type` 标签(`uint64`/`int`/`float`/`cstr`)以生成类型安全的 `snprintf` 转换;结构体块带 `ptr` 标志区分指针成员(`s->block`)与内嵌成员(`&s->block`):

```python
# tools/gen_registry.py —— 字段:类型安全的 snprintf 转换
def field_line(struct_name, member):
    cast = TYPE_CAST[member["type"]]   # uint64→(unsigned long long), cstr→(const char*) ...
    return f'''    reg.registerProvider("{as_name}", sv::makeProvider(
        [](const void* p, const sv::DeviceCtx&) -> std::string {{
            const {struct_name}* s = static_cast<const {struct_name}*>(p);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "{fmt}", {cast}s->{field});
            return std::string(buf);
        }}));'''

# 结构体块:ptr 决定导航写法
deref = f"static_cast<const void*>(s->{block})" if ptr else f"static_cast<const void*>(&s->{block})"
```

**漂移兜底**:生成的代码引用真实 `s->field`,上游 `.h` 改字段名/删字段 → 生成代码编译失败 → 漂移即时暴露在构建期,不带到运行时。`tests/test_drift.sh` 证明此机制(基线编译通过,改名后编译失败)。

### 5. 双轨 Step 表示 + 基准决断

spec §3.2 要求两条 Step 表示并存,由基准对比决断:

- **Route B(默认)**:`StepB { const ValueProvider* provider; }` —— 一切皆节点,每个 segment 一次虚调用。代码简洁。
- **Route A**:`StepA { StepKind; std::variant<Field/Connector/Device/SubRecipe> }` —— 显式四类,`ConnectorBinding` 直接持字面量(无虚调用、无堆),`SubRecipeBinding` 内联导航 + 子配方指针(不经 `StructBlockProvider` 虚)。

`tests/test_route_a.cpp` 是一致性门:两条路径输出必须逐字节相同。`bench/bench_routes.cpp` 在 Release -O2 下跑 2,000,000 次 render 对比。

**决断结果**(见 `docs/.../benchmark-result.md`,Apple M5 / Apple clang 21):

| Route | 耗时 | us/render |
|---|---|---|
| B(ValueProvider*) | ~635 ms | 0.317 |
| A(四类 Step) | ~618 ms | 0.309 |
| 加速比 | **1.02x** | |

Route A 仅快约 2%(远低于 spec 的 10%「可忽略」阈值)—— 因两者绝大部分耗时在相同的 `snprintf` 格式化 + `std::string` 堆分配(39 字符超 SSO),淹没了 Route A 省下的 ~8 次虚调用。**决定:保留 Route B 为默认**(`render`),Route A(`renderA`)留作一致性参照。复杂度不值得为 2% 买单。

---

## 构建与测试

```bash
# 依赖:C++17 编译器、CMake ≥ 3.14、Python 3(仅 codegen 构建)。vendored 库已随仓库。

# 构建(Debug)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 全部单测(12 个 doctest exe)
ctest --test-dir build --output-on-failure

# codegen 单测
python3 tests/test_codegen.py

# 漂移检测脚本
./tests/test_drift.sh

# ThreadSanitizer 验证热加载并发安全(可选但推荐)
cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build_tsan
ctest --test-dir build_tsan --output-on-failure    # 期望 0 ThreadSanitizer 警告

# 基准(Release -O2)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_routes
./build/bench_routes
```

---

## 端到端示例

```bash
./build/security_example
# 输出: CAM001:1717171717|Alice-30@100,200,300,400
```

装配链路:`schema.json` → `gen_registry.py` → `registry_gen.cpp`(`registerStructViewNames`)→ 填充 `NameRegistry` → `Engine.loadConfig(recipes.json)` → `render("alarm_line", &event, ctx)`。

其中 `${person}` 整块展开为 `Alice-30`(子配方),`${rect}` 整块展开为 `100,200,300,400`,`${camera}` 是设备 getter(下转 `MyDeviceCtx`),`${time}` 是 `uint64` 字段。

---

## 扩展工作流

| 要加什么 | 动什么 | 重编译? |
|---|---|---|
| 新标量字段 | `schema.json` 加一行 `field`(含 type/fmt) | 是(codegen→编译,`&T::field` 校验) |
| 新结构体块 | `schema.json` 加 `block` + `recipes.json` 加子配方 | 是 |
| 新连接符 | `ConnectorLib` 加一行 `lib.add(...)` | 是 |
| 新设备信息 | `DeviceCtx` 子类加 getter + `schema.json` 的 `device` 加一行 | 是 |
| **新组装格式** | **只改 `recipes.json`** | **否(运行时热加载)** |

最后一行是「易上手 / 易拓展」的精髓:**新增一种输出格式零编译**;加新结构体也只是描述文件加一行 + 重跑 codegen,框架核心 `Engine`/`Recipe`/`Step` 全程不知情。

---

## 设计与计划文档

- 设计 spec:`docs/superpowers/specs/2026-08-04-struct-view-design.md`(含 §3.4a RCU 三条规定、§6.1 codegen、§3.2 双轨)
- 基准决断:`docs/superpowers/specs/2026-08-04-struct-view-benchmark-result.md`
- 实现计划:`docs/superpowers/plans/2026-08-05-struct-view.md`(16 任务,2 阶段)
