# struct_view 核心逻辑 UML (Mermaid)

> 用 Mermaid 语法,可在 GitHub/IDE 直接渲染,无需 PlantUML 服务器。

## 1. 注册流程(启动期):schema → codegen → NameRegistry

```mermaid
sequenceDiagram
    participant 维护者
    participant gen_schema.py
    participant gen_registry.py
    participant registry_gen.cpp
    participant NameRegistry
    participant ConnectorLib

    维护者->>gen_schema.py: event.h
    Note over gen_schema.py: 正则解析 typedef struct<br/>推导 type/ptr/array/block
    gen_schema.py-->>维护者: schema.json (初始骨架)

    维护者->>维护者: 补充 desc / sep / fields / deviceCtxType

    维护者->>gen_registry.py: schema.json
    Note over gen_registry.py: validate_member() 校验<br/>生成 register* 调用<br/>fmt 自动推导 / ptr 默认 false
    gen_registry.py-->>维护者: registry_gen.cpp

    维护者->>registry_gen.cpp: #include (编译期)
    Note over registry_gen.cpp: static_assert(extent_v >= count)<br/>漂移检查
    registry_gen.cpp->>NameRegistry: registerStructViewNames(reg)

    Note over NameRegistry: entries_: name → ValueProvider (标量字段/设备)<br/>structDecls_: name → StructBlockDecl (nav+fields+sep)<br/>structArrayDecls_: name → StructArrayDecl (nav+fields+count+sep+arraySep)<br/>scalarArrayDecls_: name → ScalarArrayDecl (elemFn+count+sep)<br/>descs_: name → desc

    维护者->>ConnectorLib: add("dash", "-")
    维护者->>ConnectorLib: add("pipe", "|")
```

## 2. 配置流程(运行期 loadConfig):recipe JSON → Engine → RecipeStore

```mermaid
sequenceDiagram
    participant Client
    participant Engine
    participant ConfigLoader
    participant Validator
    participant Builder
    participant NameRegistry
    participant RecipeStore

    Client->>Engine: loadConfig(recipeJson)
    Engine->>ConfigLoader: parse(jsonText)

    Note over ConfigLoader: 分词器解析 ${name:sep=xxx:isep=yyy}<br/>→ Segment{isRef, text, sepOverride, isepOverride}

    ConfigLoader-->>Engine: ConfigAst {recipes: [{name, desc, segments}]}

    Engine->>Validator: validate(ast, names, connectors)
    Validator->>NameRegistry: lookup / isStructBlock / isStructArray / isScalarArray
    NameRegistry-->>Validator: name 是否存在 + 类型

    Note over Validator: 校验 r_ 前缀<br/>block fields 存在性<br/>sep/isep 合法性<br/>用户子配方循环检测

    Validator-->>Engine: errors[] (空=通过)

    Engine->>Builder: compile(ast, names, connectors)
    Builder->>NameRegistry: findStructBlockDecl / findStructArrayDecl / findScalarArrayDecl
    NameRegistry-->>Builder: decl (nav + fields + sep + count + ...)

    Note over Builder: Pass 1: 编译每个 recipe → StepB{provider}<br/>　标量字段 → names.lookup(vp)<br/>　标量数组 → pendingScalar<br/>　结构体块 → pending<br/>　结构体数组 → pendingArray<br/>　连接符 → ConnectorProvider<br/>　用户子配方 → pendingSub<br/><br/>Pass 2: StructBlockProvider(nav, fields, sep)<br/>Pass 2b: ArrayStructBlockProvider(nav, fields, count, sep, arraySep)<br/>Pass 2c: ScalarArrayProvider(elemFn, count, sep)<br/>Pass 2d: SubRecipeProvider(subRecipe)<br/><br/>sep/isep 覆盖: 用 seg.sepOverride 替代 decl 默认值

    Builder-->>Engine: Result {recipes: map{name, shared_ptr{RecipeB}}}

    Engine->>RecipeStore: publishAll(recipes)

    Note over RecipeStore: 原子换 map (一把写锁)<br/>旧配方靠 shared_ptr 引用计数保活<br/>→ RCU: 在途 render 不被打断

    Engine-->>Client: LoadResult {ok, errors}
```

## 3. 渲染流程(热路径):render → RecipeStore snapshot → runRecipeB

```mermaid
sequenceDiagram
    participant 调用方
    participant Engine
    participant RecipeStore
    participant runRecipeB
    participant StepB.provider

    调用方->>Engine: render("r_alarm_line", &event, ctx)
    Engine->>RecipeStore: snapshot("r_alarm_line")
    RecipeStore-->>Engine: shared_ptr{const RecipeB} (RCU 快照)

    Engine->>runRecipeB: runRecipeB(recipe, structPtr, ctx)

    loop 每个 StepB
        runRecipeB->>StepB.provider: provider->get(structPtr, ctx)

        alt 标量字段 (LambdaProvider)
            Note over StepB.provider: snprintf / std::string(cstr)
            StepB.provider-->>runRecipeB: "1717171717"
        else 连接符 (ConnectorProvider)
            StepB.provider-->>runRecipeB: ":" / "|" / "@"
        else 结构体块 (StructBlockProvider)
            Note over StepB.provider: nav.navigate(structPtr) → child<br/>loop fields[]: fieldProvider->get(child, ctx)
            StepB.provider-->>runRecipeB: "Alice-30"
        else 标量数组 (ScalarArrayProvider)
            Note over StepB.provider: loop count: elemFn(structPtr, i)
            StepB.provider-->>runRecipeB: "11-22-33-0-0-0-0-0"
        else 结构体数组 (ArrayStructBlockProvider)
            Note over StepB.provider: loop count: nav→child<br/>loop fields[]: fieldProvider->get(child, ctx)
            StepB.provider-->>runRecipeB: "100,200,300,400,1-2|..."
        else 用户子配方 (SubRecipeProvider)
            StepB.provider->>runRecipeB: runRecipeB(*sub_, structPtr, ctx) (递归)
            runRecipeB-->>StepB.provider: 子配方输出
            StepB.provider-->>runRecipeB: 子配方输出
        end
    end

    runRecipeB-->>Engine: "CAM001:1717171717|Alice-30@..."
    Engine-->>调用方: 返回字符串
```

## 4. 类图:核心类型关系

```mermaid
classDiagram
    class NameRegistry {
        -entries_: map~string, unique_ptr~ValueProvider~~
        -structDecls_: vector~pair~string, StructBlockDecl~~
        -structArrayDecls_: vector~pair~string, StructArrayDecl~~
        -scalarArrayDecls_: vector~pair~string, ScalarArrayDecl~~
        -descs_: map~string, string~
        +registerProvider(name, vp, desc)
        +registerStruct(name, nav, fields, sep, desc)
        +registerStructArray(name, nav, fields, count, sep, arraySep, desc)
        +registerScalarArray(name, elemFn, count, sep, desc)
        +lookup(name) const ValueProvider*
        +describe(name) const string&
        +exportNames() vector~NameInfo~
        +findStructBlockDecl(name) const StructBlockDecl*
        +findStructArrayDecl(name) const StructArrayDecl*
        +findScalarArrayDecl(name) const ScalarArrayDecl*
        +isStructBlock(name) bool
        +isStructArray(name) bool
        +isScalarArray(name) bool
    }

    class StructBlockDecl {
        +nav: Navigator
        +fieldNames: vector~string~
        +sep: string
        +desc: string
    }

    class StructArrayDecl {
        +nav: IndexedNavigator
        +fieldNames: vector~string~
        +count: size_t
        +sep: string
        +arraySep: string
        +desc: string
    }

    class ScalarArrayDecl {
        +elemFn: function
        +count: size_t
        +sep: string
        +desc: string
    }

    class NameInfo {
        +name: string
        +type: string
        +desc: string
        +canSep: bool
        +canIsep: bool
        +defaultSep: string
        +defaultIsep: string
    }

    class ValueProvider {
        <<abstract>>
        +get(structPtr, ctx) string
    }

    class LambdaProvider {
        -fn: F
    }

    class ConnectorProvider {
        -literal_: string
    }

    class ScalarArrayProvider {
        -elemFn_: function
        -count_: size_t
        -sep_: string
    }

    class StructBlockProvider {
        -nav_: Navigator
        -fieldProviders_: vector~const ValueProvider*~
        -sep_: string
    }

    class ArrayStructBlockProvider {
        -nav_: IndexedNavigator
        -fieldProviders_: vector~const ValueProvider*~
        -count_: size_t
        -sep_: string
        -arraySep_: string
    }

    class SubRecipeProvider {
        -sub_: shared_ptr~const RecipeB~
    }

    class Navigator {
        -fn_: function
        +navigate(parent) const void*
    }

    class IndexedNavigator {
        -fn_: function
        +navigate(parent, i) const void*
    }

    class StepB {
        +provider: const ValueProvider*
    }

    class RecipeB {
        +name: string
        +steps: vector~StepB~
        +ownedProviders: vector~unique_ptr~ValueProvider~~
    }

    class RecipeStore~RecipeT~ {
        -pimpl_: unique_ptr~Impl~
        +publish(name, recipe)
        +publishAll(map)
        +snapshot(name) shared_ptr~const T~
    }

    class ConfigLoader {
        +parse(jsonText) Result
    }

    class ConfigAst {
        +recipes: vector~RecipeAst~
    }

    class Segment {
        +isRef: bool
        +text: string
        +sepOverride: string
        +isepOverride: string
    }

    class Validator {
        +validate(ast, names, connectors) vector~ConfigError~
    }

    class Builder {
        +compile(ast, names, connectors) Result
    }

    class Engine {
        -names_: NameRegistry&
        -connectors_: ConnectorLib&
        -store_: RecipeStore~RecipeB~
        -recipeDescs_: map~string, string~
        +loadConfig(jsonText) LoadResult
        +render(recipeName, structPtr, ctx) string
        +describeRecipe(name) const string&
    }

    class ConnectorLib {
        -entries_: map~string, string~
        +add(name, literal)
        +get(name) optional~string~
    }

    ValueProvider <|-- LambdaProvider
    ValueProvider <|-- ConnectorProvider
    ValueProvider <|-- ScalarArrayProvider
    ValueProvider <|-- StructBlockProvider
    ValueProvider <|-- ArrayStructBlockProvider
    ValueProvider <|-- SubRecipeProvider

    NameRegistry o-- ValueProvider : entries_ owns
    NameRegistry o-- StructBlockDecl
    NameRegistry o-- StructArrayDecl
    NameRegistry o-- ScalarArrayDecl
    StructBlockProvider --> Navigator
    ArrayStructBlockProvider --> IndexedNavigator
    StructBlockProvider --> ValueProvider : fieldProviders_ refs
    RecipeB *-- StepB
    RecipeB *-- ValueProvider : ownedProviders owns
    Engine --> NameRegistry : ref
    Engine --> ConnectorLib : ref
    Engine --> RecipeStore : store_
    Engine ..> ConfigLoader : loadConfig uses
    Engine ..> Validator : loadConfig uses
    Engine ..> Builder : loadConfig uses
    Builder ..> NameRegistry : compile reads
    Builder ..> ValueProvider : creates
    RecipeStore o-- RecipeB : shared_ptr owns
```

## 5. 完整时序图:注册 + 配置 + 渲染 + 热加载

```mermaid
sequenceDiagram
    actor 维护者
    actor Client
    participant gen_schema.py
    participant gen_registry.py
    participant NameRegistry
    participant ConnectorLib
    participant Engine
    participant ConfigLoader
    participant Validator
    participant Builder
    participant RecipeStore

    rect rgb(240, 248, 255)
    Note over 维护者,NameRegistry: 启动期: 注册
    维护者->>gen_schema.py: event.h
    gen_schema.py-->>维护者: schema.json (初始骨架)
    维护者->>维护者: 补充 desc/sep/fields
    维护者->>gen_registry.py: schema.json
    gen_registry.py-->>维护者: registry_gen.cpp
    维护者->>NameRegistry: registerStructViewNames(reg)
    维护者->>ConnectorLib: add("dash","-")
    end

    rect rgb(255, 245, 238)
    Note over Client,RecipeStore: 运行期: 配置下发
    Client->>Engine: loadConfig(recipeJson)
    Engine->>ConfigLoader: parse(jsonText)
    ConfigLoader-->>Engine: ConfigAst
    Engine->>Validator: validate(ast, reg, lib)
    Validator-->>Engine: errors[] (空=OK)
    Engine->>Builder: compile(ast, reg, lib)
    Builder->>NameRegistry: findStructBlockDecl/findScalarArrayDecl/...
    NameRegistry-->>Builder: decls
    Builder-->>Engine: Result {recipes map}
    Engine->>RecipeStore: publishAll(recipes)
    Engine-->>Client: LoadResult {ok:true}
    end

    rect rgb(240, 255, 240)
    Note over Client,Engine: 运行期: 渲染
    Client->>Engine: render("r_alarm_line", &event, ctx)
    Engine->>RecipeStore: snapshot("r_alarm_line")
    RecipeStore-->>Engine: shared_ptr~RecipeB~
    Note over Engine: runRecipeB(recipe, &event, ctx)
    Engine-->>Client: "CAM001:1717171717|Alice-30@..."
    end

    rect rgb(255, 255, 240)
    Note over Client,RecipeStore: 运行期: 热加载
    Client->>Engine: loadConfig(newRecipeJson)
    Engine->>RecipeStore: publishAll(newRecipes)
    Note over RecipeStore: 原子替换,旧配方 RCU 保活
    Engine-->>Client: LoadResult {ok:true}
    end
```
