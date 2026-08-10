# struct_view 核心逻辑 UML

> 用 PlantUML 语法描述。可用 [PlantUML 在线渲染器](http://www.plantuml.com/plantuml/) 或 VS Code PlantUML 插件查看。

## 1. 注册流程(启动期):schema → codegen → NameRegistry

```plantuml
@startuml
!theme plain
title 注册流程: schema.json → codegen → NameRegistry

participant "维护者" as Dev
participant "gen_schema.py" as GenSch
participant "gen_registry.py" as GenReg
participant "registry_gen.cpp" as RegCpp
participant "NameRegistry" as Reg
participant "ConnectorLib" as Lib

Dev -> GenSch: event.h
GenSch -> GenSch: 正则解析 typedef struct
GenSch --> Dev: schema.json (初始骨架, type/ptr/array 自动推导)

Dev -> Dev: 补充 desc / sep / fields / deviceCtxType

Dev -> GenReg: schema.json
GenReg -> GenReg: validate_member() 校验
GenReg -> GenReg: 生成 register* 调用
GenReg --> Dev: registry_gen.cpp

Dev -> RegCpp: #include (编译期)
RegCpp -> RegCpp: static_assert(extent_v >= count) 漂移检查
RegCpp --> Reg: registerStructViewNames(reg)

note over Reg
  NameRegistry 内部存储:
  - entries_: name → ValueProvider (标量字段/设备)
  - structDecls_: name → StructBlockDecl (nav + fields + sep + desc)
  - structArrayDecls_: name → StructArrayDecl (nav + fields + count + sep + arraySep)
  - scalarArrayDecls_: name → ScalarArrayDecl (elemFn + count + sep)
  - descs_: name → desc (描述,给 UI 显示)
end note

Dev -> Lib: add("dash", "-")
Dev -> Lib: add("pipe", "|")

@enduml
```

## 2. 配置流程(运行期 loadConfig):recipe JSON → Engine → RecipeStore

```plantuml
@startuml
!theme plain
title 配置流程: loadConfig (parse → validate → compile → publish)

participant "Client / UI" as Client
participant "Engine" as Eng
participant "ConfigLoader" as Loader
participant "Validator" as Val
participant "Builder" as Bld
participant "NameRegistry" as Reg
participant "RecipeStore" as Store

Client -> Eng: loadConfig(recipeJson)
Eng -> Loader: parse(jsonText)
Loader --> Eng: ConfigAst {recipes: [{name, desc, segments}]}

note over Loader
  分词器解析 ${name:sep=xxx:isep=yyy}
  → Segment{isRef, text, sepOverride, isepOverride}
end note

Eng -> Val: validate(ast, names, connectors)
Val -> Reg: lookup / isStructBlock / isStructArray / isScalarArray
Reg --> Val: name 是否存在 + 类型
Val -> Val: 校验 r_ 前缀 + block fields 存在性 + sep/isep 合法性 + 用户子配方循环
Val --> Eng: errors[] (空=通过)

Eng -> Bld: compile(ast, names, connectors)
Bld -> Reg: findStructBlockDecl / findStructArrayDecl / findScalarArrayDecl
Reg --> Bld: decl (nav + fields + sep + count + ...)

note over Bld
  Pass 1: 编译每个 recipe → StepB{provider}
    - 标量字段 → names.lookup(vp)
    - 标量数组 → pendingScalar
    - 结构体块 → pending
    - 结构体数组 → pendingArray
    - 连接符 → ConnectorProvider
    - 用户子配方 → pendingSub

  Pass 2: 结构体块 → StructBlockProvider(nav, fields, sep)
  Pass 2b: 结构体数组 → ArrayStructBlockProvider(nav, fields, count, sep, arraySep)
  Pass 2c: 标量数组 → ScalarArrayProvider(elemFn, count, sep)
  Pass 2d: 用户子配方 → SubRecipeProvider(subRecipe)

  sep/isep 覆盖: 用 seg.sepOverride / seg.isepOverride 替代 decl 默认值
end note

Bld --> Eng: Result {recipes: map<name, shared_ptr<RecipeB>>}

Eng -> Store: publishAll(recipes)
note over Store
  原子换 map (一把写锁)
  旧配方靠 shared_ptr 引用计数保活
  → RCU: 在途 render 不被打断
end note

Eng --> Client: LoadResult {ok, errors}

@enduml
```

## 3. 渲染流程(热路径):render → RecipeStore snapshot → runRecipeB

```plantuml
@startuml
!theme plain
title 渲染热路径: render(recipeName, structPtr, ctx)

participant "调用方" as Caller
participant "Engine" as Eng
participant "RecipeStore" as Store
participant "runRecipeB" as Run
participant "StepB.provider" as Prov

Caller -> Eng: render("r_alarm_line", &event, ctx)
Eng -> Store: snapshot("r_alarm_line")
Store --> Eng: shared_ptr<const RecipeB> (RCU 快照)

Eng -> Run: runRecipeB(recipe, structPtr, ctx)

loop 每个 StepB
  Run -> Prov: provider->get(structPtr, ctx)

  alt 标量字段 (LambdaProvider)
    Prov -> Prov: snprintf / std::string(cstr)
    Prov --> Run: "1717171717"
  else 连接符 (ConnectorProvider)
    Prov --> Run: ":" / "|" / "@"
  else 结构体块 (StructBlockProvider)
    Prov -> Prov: nav.navigate(structPtr) → child
    loop fields[]
      Prov -> Prov: fieldProvider->get(child, ctx)
    end
    Prov --> Run: "Alice-30"
  else 标量数组 (ScalarArrayProvider)
    loop count
      Prov -> Prov: elemFn(structPtr, i)
    end
    Prov --> Run: "11-22-33-0-0-0-0-0"
  else 结构体数组 (ArrayStructBlockProvider)
    loop count
      Prov -> Prov: nav.navigate(structPtr, i) → child
      loop fields[]
        Prov -> Prov: fieldProvider->get(child, ctx)
      end
    end
    Prov --> Run: "100,200,300,400,1-2|..."
  else 用户子配方 (SubRecipeProvider)
    Prov -> Run: runRecipeB(*sub_, structPtr, ctx) (递归)
    Run --> Prov: 子配方输出
    Prov --> Run: 子配方输出
  end
end

Run --> Eng: "CAM001:1717171717|Alice-30@..."
Eng --> Caller: 返回字符串

@enduml
```

## 4. 类图:核心类型关系

```plantuml
@startuml
!theme plain
title 核心类图

package "注册表" {
  class NameRegistry {
    - entries_: map<string, unique_ptr<ValueProvider>>
    - structDecls_: vector<pair<string, StructBlockDecl>>
    - structArrayDecls_: vector<pair<string, StructArrayDecl>>
    - scalarArrayDecls_: vector<pair<string, ScalarArrayDecl>>
    - descs_: map<string, string>
    + registerProvider(name, vp, desc)
    + registerStruct(name, nav, fields, sep, desc)
    + registerStructArray(name, nav, fields, count, sep, arraySep, desc)
    + registerScalarArray(name, elemFn, count, sep, desc)
    + lookup(name): const ValueProvider*
    + describe(name): const string&
    + exportNames(): vector<NameInfo>
    + findStructBlockDecl / findStructArrayDecl / findScalarArrayDecl
    + isStructBlock / isStructArray / isScalarArray
  }

  class StructBlockDecl {
    + nav: Navigator
    + fieldNames: vector<string>
    + sep: string
    + desc: string
  }

  class StructArrayDecl {
    + nav: IndexedNavigator
    + fieldNames: vector<string>
    + count: size_t
    + sep: string
    + arraySep: string
    + desc: string
  }

  class ScalarArrayDecl {
    + elemFn: function<string(const void*, size_t)>
    + count: size_t
    + sep: string
    + desc: string
  }

  class NameInfo {
    + name: string
    + type: string
    + desc: string
    + canSep: bool
    + canIsep: bool
    + defaultSep: string
    + defaultIsep: string
  }
}

package "ValueProvider 层次" {
  abstract class ValueProvider {
    + get(structPtr, ctx): string
  }

  class LambdaProvider {
    - fn: F
  }

  class ConnectorProvider {
    - literal_: string
  }

  class ScalarArrayProvider {
    - elemFn_: function
    - count_: size_t
    - sep_: string
  }

  class StructBlockProvider {
    - nav_: Navigator
    - fieldProviders_: vector<const ValueProvider*>
    - sep_: string
  }

  class ArrayStructBlockProvider {
    - nav_: IndexedNavigator
    - fieldProviders_: vector<const ValueProvider*>
    - count_: size_t
    - sep_: string
    - arraySep_: string
  }

  class SubRecipeProvider {
    - sub_: shared_ptr<const RecipeB>
  }

  ValueProvider <|-- LambdaProvider
  ValueProvider <|-- ConnectorProvider
  ValueProvider <|-- ScalarArrayProvider
  ValueProvider <|-- StructBlockProvider
  ValueProvider <|-- ArrayStructBlockProvider
  ValueProvider <|-- SubRecipeProvider
}

package "导航器" {
  class Navigator {
    - fn_: const void*(*)(const void*)
    + navigate(parent): const void*
  }
  class IndexedNavigator {
    - fn_: const void*(*)(const void*, size_t)
    + navigate(parent, i): const void*
  }
}

package "Recipe" {
  class StepB {
    + provider: const ValueProvider*
  }
  class RecipeB {
    + name: string
    + steps: vector<StepB>
    + ownedProviders: vector<unique_ptr<ValueProvider>>
  }
  class RecipeStore<T> {
    - pimpl_: unique_ptr<Impl>
    + publish(name, recipe)
    + publishAll(map)
    + snapshot(name): shared_ptr<const T>
  }
}

package "配置流水线" {
  class ConfigLoader {
    + parse(jsonText): Result {ok, parseError, ast}
  }
  class ConfigAst {
    + recipes: vector<RecipeAst>
  }
  class RecipeAst {
    + name: string
    + desc: string
    + segments: vector<Segment>
  }
  class Segment {
    + isRef: bool
    + text: string
    + sepOverride: string
    + isepOverride: string
  }
  class Validator {
    + validate(ast, names, connectors): vector<ConfigError>
  }
  class Builder {
    + compile(ast, names, connectors): Result {ok, errors, recipes}
  }
}

package "Engine" {
  class Engine {
    - names_: NameRegistry&
    - connectors_: ConnectorLib&
    - store_: RecipeStore<RecipeB>
    - recipeDescs_: map<string, string>
    + loadConfig(jsonText): LoadResult
    + render(recipeName, structPtr, ctx): string
    + describeRecipe(name): const string&
  }
  class ConnectorLib {
    - entries_: map<string, string>
    + add(name, literal)
    + get(name): optional<string>
  }
}

NameRegistry o-- ValueProvider : entries_ owns
NameRegistry o-- StructBlockDecl
NameRegistry o-- StructArrayDecl
NameRegistry o-- ScalarArrayDecl
StructBlockProvider --> Navigator
ArrayStructBlockProvider --> IndexedNavigator
StructBlockProvider --> ValueProvider : fieldProviders_ references
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

@enduml
```

## 5. 时序图:完整流程(注册 + 配置 + 渲染)

```plantuml
@startuml
!theme plain
title 完整流程: 注册 → 配置 → 渲染

actor 维护者 as Dev
actor Client as Cli
participant "gen_schema.py" as GS
participant "gen_registry.py" as GR
participant "NameRegistry" as Reg
participant "ConnectorLib" as Lib
participant "Engine" as Eng
participant "ConfigLoader" as CL
participant "Validator" as Val
participant "Builder" as Bld
participant "RecipeStore" as RS

== 启动期: 注册 ==

Dev -> GS: event.h
GS --> Dev: schema.json (初始骨架)
Dev -> Dev: 补充 desc/sep/fields
Dev -> GR: schema.json
GR --> Dev: registry_gen.cpp
Dev -> Reg: registerStructViewNames(reg)
Dev -> Lib: add("dash","-")

== 运行期: 配置下发 ==

Cli -> Eng: loadConfig(recipeJson)
Eng -> CL: parse(jsonText)
CL --> Eng: ConfigAst
Eng -> Val: validate(ast, reg, lib)
Val --> Eng: errors[] (空=OK)
Eng -> Bld: compile(ast, reg, lib)
Bld -> Reg: findStructBlockDecl/findScalarArrayDecl/...
Reg --> Bld: decls
Bld --> Eng: Result {recipes map}
Eng -> RS: publishAll(recipes)
Eng --> Cli: LoadResult {ok:true}

== 运行期: 渲染 ==

Cli -> Eng: render("r_alarm_line", &event, ctx)
Eng -> RS: snapshot("r_alarm_line")
RS --> Eng: shared_ptr<RecipeB>
Eng -> Eng: runRecipeB(recipe, &event, ctx)
Eng --> Cli: "CAM001:1717171717|Alice-30@..."

== 运行期: 热加载 ==

Cli -> Eng: loadConfig(newRecipeJson)
Eng -> RS: publishAll(newRecipes)
note right: 原子替换,旧配方 RCU 保活
Eng --> Cli: LoadResult {ok:true}

@enduml
```
