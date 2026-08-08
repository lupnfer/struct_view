# 命名约定 + 结构体块展开规则 + desc —— 设计 spec

- 日期: 2026-08-07
- 状态: 已批准 (设计阶段)
- 关联: `docs/superpowers/specs/2026-08-04-struct-view-design.md`(主设计)、`2026-08-05-struct-view-array-support.md`(数组扩展)
- 关联实现: `tools/gen_registry.py`、`include/struct_view/{NameRegistry,ValueProvider,ConfigAst,Validator,Builder,Engine}.hpp` 及对应 .cpp

## 1. 目标与背景

### 1.1 要解决的问题

两个痛点:

1. **三类 name 不可辨**:原生字段名(`time`/`x`)、用户组合的子配方、顶层配方名(`alarm_line`)在 `${...}` 引用和配置里看不出区别,用户无法一眼识别「这是原生结构体字段」还是「用户拼的产物」。
2. **结构体块的展开规则在 recipe 里重复描述**:`Box` 在 schema 里已声明有 x/y/w/h/tags 字段,recipe 还要写一个子配方 `${x},${y},${w},${h}(${tags})` 重新描述一遍怎么组合 —— schema 和 recipe 职责重叠,维护成本翻倍。

### 1.2 设计决策(已定)

| 维度 | 决定 |
|---|---|
| 命名机制 | 仅前缀约定 + desc 字段,不新增「组合字段」机制 |
| 配方名前缀 | `r_`(强制,Validator 校验) |
| 原生字段名 | 无前缀(保持 `x`/`time`);且禁止 `r_` 开头(避免与配方名混淆) |
| desc 存储 | 写在 schema.json/recipes.json 里(贴着 name,维护成本最低) |
| desc 用途 | 给 client 显示用,中文,不参与 `${...}` 引用 |
| desc 获取 | codegen 带进生成代码,运行时存 registry/Engine,client 查询 API 获取 |
| 结构体块展开规则 | **收进 schema**(`block` 加 `sep`+`fields`),去掉 `subRecipe`;recipe 不再为结构体块写子配方(去重) |
| 用户组合子配方 | **保留** —— 用户可定义不绑结构体的可复用子配方(如 `r_meta` = `${camera}:${time}`),被 `${r_meta}` 引用 |
| 子配方命名 | 用户子配方也用 `r_` 前缀(它是用户拼的产物) |

### 1.3 职责边界(核心)

**schema.json 只描述原生结构体**(字段/块/设备 + 块展开规则 sep+fields + desc),**完全不碰配方**。
**recipes.json 只描述配方**(顶层配方 + 用户组合子配方 + 配方 desc),**不重复描述结构体块内部**。

结构体块的展开规则(字段顺序 + sep)归 schema;用户自由拼接的可复用片段归 recipe。两者职责清晰分离,不再重叠。

## 2. 命名约定

### 2.1 三类 name

| name 类 | 来源 | 命名规则 | 示例 |
|---|---|---|---|
| 原生字段名 | schema `field`/`as`(绑 C 成员或设备 getter) | 无前缀;**禁止 `r_` 开头** | `x`、`time`、`feat_ids`、`camera` |
| 结构体块名 | schema `block` | 无前缀;**禁止 `r_` 开头** | `person`、`rect`、`boxes` |
| 配方名 | recipes.json `name`(顶层配方 + 用户子配方) | **`r_` 前缀**(强制) | `r_alarm_line`、`r_meta` |

一眼可辨:`r_` 开头 → 配方(用户拼的产物);否则 → 原生字段/块(绑 C 结构体)。

### 2.2 Validator 强制校验

- 配方名必须 `r_` 开头,否则报错 `"recipe name must start with 'r_': <name>"`。
- schema 里 `field`/`as`/`block` 名若以 `r_` 开头,报错 `"field/block name must not start with 'r_' (reserved for recipes): <name>"`。
- 全量收集(和现有错误一起一次性返回)。

## 3. desc 字段(描述,给 client 显示)

### 3.1 形态

desc 是可选的中文描述,贴着 name 写:

**schema.json(字段/块/设备 desc):**
```json
{"field": "x", "desc": "检测框横坐标", "type": "int", "fmt": "%d"}
{"block": "person", "desc": "人体信息", "ptr": true, "sep": "-", "fields": ["name", "age"]}
{"getter": "cameraId", "as": "camera", "desc": "摄像头编号"}
```

**recipes.json(配方 desc):**
```json
{"name": "r_alarm_line", "desc": "告警行", "template": "..."}
```

- desc 可选(缺则无描述,查询返回空串)。
- desc 不参与 `${...}` 引用 —— 元数据,不是 name。
- desc 支持中文(UTF-8)。

### 3.2 运行时存储 + 查询 API

**字段/块/设备 desc 存 NameRegistry**(codegen 带进生成代码):
```cpp
// include/struct_view/NameRegistry.hpp(追加)
class NameRegistry {
    std::unordered_map<std::string, std::string> descs_;   // name → desc
public:
    // 现有 register* 方法各加一个 desc 参数(默认空串,向后兼容);registerStruct/Array
    // 签名同时改为接收 fields+sep(取代 subRecipeName,见 §8.1)。
    void registerProvider(std::string name, std::unique_ptr<ValueProvider> vp, std::string desc = "");
    void registerStruct(std::string name, Navigator nav,
                        std::vector<std::string> fieldNames, std::string sep, std::string desc = "");
    void registerStructArray(std::string name, IndexedNavigator nav,
                             std::vector<std::string> fieldNames, std::size_t count,
                             std::string sep, std::string arraySep, std::string desc = "");
    const std::string& describe(const std::string& name) const;  // 返回 desc,无则空串
};
```
- desc 参数默认空串,向后兼容(现有调用不传 desc 仍编译)。codegen 改为总是传 desc(从 schema 读取,缺则 `""`)。
- `describe(name)` 返回 `const std::string&`(引用,避免拷贝;查不到返回静态空串)。
- desc 存储与 provider 存储解耦,不影响 render 热路径。

**配方 desc 存 Engine**(loadConfig 解析 recipes.json 时抽取):
```cpp
// include/struct_view/Engine.hpp(追加)
class Engine {
    std::unordered_map<std::string, std::string> recipeDescs_;   // 配方名 → desc
public:
    LoadResult loadConfig(std::string_view jsonText);   // 解析时抽 desc 存进 recipeDescs_
    const std::string& describeRecipe(const std::string& recipeName) const;  // 返回配方 desc,无则空串
};
```
- 配方 desc 随 `loadConfig` 热加载替换(与 publishAll 同时机换 map,保持一致性)。
- `describeRecipe` 返回配方 desc,无则空串。

## 4. 结构体块展开规则收进 schema(核心改动)

### 4.1 schema 的 block 加 sep + fields,去掉 subRecipe

**改前(当前):**
```json
{"block": "person", "ptr": true, "subRecipe": "person"}
{"block": "rect",   "ptr": false, "subRecipe": "box"}
{"block": "boxes", "array": {"subRecipe": "box", "count": 4, "sep": "|"}}
```

**改后:**
```json
{"block": "person", "desc": "人体信息", "ptr": true, "sep": "-", "fields": ["name", "age"]}
{"block": "rect",   "desc": "检测框",   "ptr": false, "sep": ",", "fields": ["x", "y", "w", "h", "tags"]}
{"block": "boxes",  "desc": "检测框数组", "sep": "|", "fields": ["x", "y", "w", "h", "tags"],
 "array": {"count": 4, "sep": "|"}}
```

语义:
- `fields` = 块内字段的**展开顺序**(列表)。每个元素是该结构体里的字段名(原生 field 名,如 `x`/`name`/`tags`)。
- `sep` = 块内字段间的**分隔符**(join 语义,首字段前不加)。`rect` 的 `sep: ","` → `100,200,300,400,7-8`。
- 结构体数组的 `array.sep` = **元素间**分隔符(boxes 之间用 `|`);块的 `sep` = **块内字段间**分隔符。两个 sep 语义不同,各管一层。
- **`subRecipe` 字段删除** —— 块展开规则在 schema,不再引用 recipe。
- `fields` 里的字段名必须在所属结构体的 members 里存在(Validator 校验,见 §6)。

### 4.2 运行时:StructBlockProvider 改为持「字段访问器列表 + sep」

当前 `StructBlockProvider` 持 `Navigator + shared_ptr<const RecipeB> sub_`(导航到子结构体后跑子配方)。改为持**导航器 + 字段访问器列表 + sep**:

```cpp
// include/struct_view/ValueProvider.hpp(修改 StructBlockProvider)
class StructBlockProvider : public ValueProvider {
    Navigator nav_;                                    // 父→子结构体指针
    std::vector<const ValueProvider*> fieldProviders_; // 子结构体各字段的访问器(按 fields 顺序)
    std::string sep_;                                  // 字段间分隔符
public:
    StructBlockProvider(Navigator nav, std::vector<const ValueProvider*> fields, std::string sep);
    std::string get(const void* structPtr, const DeviceCtx& ctx) const override;
};

// src/ValueProvider.cpp
std::string StructBlockProvider::get(const void* structPtr, const DeviceCtx& ctx) const {
    if (fieldProviders_.empty()) return {};   // defensive:空字段列表返回空串
    const void* child = nav_.navigate(structPtr);
    std::string out;
    for (std::size_t i = 0; i < fieldProviders_.size(); ++i) {
        if (i) out += sep_;                              // join 语义
        out += fieldProviders_[i]->get(child, ctx);      // 每个字段在子结构体上取值
    }
    return out;
}
```

要点:
- **`fieldProviders_` 是 `const ValueProvider*` 列表** —— 指向 NameRegistry 里该结构体各字段的 provider(`x`/`y`/`tags` 等)。这些 provider 已注册(绑真实 C 成员),在子结构体指针上 `get()` 即可取值。
- **Builder 负责组装** —— 编译时,对每个块声明,从 NameRegistry 查 `fields` 列表里每个字段名的 provider,按顺序塞进 `fieldProviders_`。若某字段名不存在,报错。
- **Route A 的 `SubRecipeBinding` 同理改** —— 不再持子配方,改为持「导航器 + 字段访问器列表 + sep」。`runRecipeA` 的 `SubRecipe` case 改为遍历字段列表。
- **`ArrayStructBlockProvider`/`ArrayStructBlockProviderA` 同理** —— 持字段访问器列表 + sep(元素内) + count + array.sep(元素间)。

> 注:`StructBlockProvider::get` 现有的 `if (!sub_)` 防御改为 `if (fieldProviders_.empty()) return {};`(空字段列表时返回空串)。

### 4.3 去掉的机制

- **`subRecipe` 字段**(schema block 不再有)。
- **结构体块对应的子配方**(recipes.json 不再为 `person`/`rect`/`boxes` 写子配方)。
- **Builder Pass 2 的「子配方绑定」**(`b.second->bind(it->second)` 那段)—— 不再有子配方可绑。
- **Validator 的「子配方存在性」检查**(`struct block subRecipe not found`)—— 不再有 subRecipe。
- **Validator 的「结构体块→子配方循环依赖」检测** —— 结构体块不再引用子配方,无环可循环。但**用户子配方间的循环**(见 §5)仍要检测。

## 5. 用户组合子配方(保留)

### 5.1 形态

用户可定义不绑结构体的可复用子配方,被 `${r_<name>}` 引用:

```json
{
  "recipes": [
    {"name": "r_meta", "desc": "元数据片段", "template": "${camera}:${time}|${feat_ids}"},
    {"name": "r_alarm_line", "desc": "告警行", "template": "${r_meta}|${person}@${rect}|${boxes}"}
  ]
}
```
- `r_meta` 不绑结构体(无 `forBlock`/无 `fields`),是纯用户拼接片段,可被 `${r_meta}` 引用。
- `r_alarm_line` 是顶层配方(被 `render("r_alarm_line")` 调用),它引用 `${r_meta}`(用户子配方)+ `${person}`/`${rect}`/`${boxes}`(结构体块,展开规则在 schema)。
- 用户子配方和顶层配方在 recipes.json 里**形态相同**(都有 name+template+desc),区别只在「是否被 `${...}` 引用」—— 一个配方可同时被引用和被 render 调用。

### 5.2 运行时(不变)

用户子配方仍走现有 RecipeB 机制:Builder 编译成 RecipeB,`StepB.provider` 指向各被引用 name 的 provider(字段/块/设备/其他子配方)。结构体块 provider(`StructBlockProvider`)现在是「字段列表 + sep」版本(§4.2),但对外仍是 `ValueProvider`,被 StepB 引用的方式不变。

### 5.3 Validator 循环检测(用户子配方间)

结构体块不再引用子配方(无环),但**用户子配方之间可能循环**(如 `r_a` 引用 `r_b`,`r_b` 引用 `r_a`)。Validator 保留循环检测,但图改为「用户子配方 → 引用的 name → 若该 name 是用户子配方,递归」。结构体块/字段不参与(它们是叶子)。

## 6. Validator 校验项(汇总)

在现有检查基础上调整:

1. **`r_` 前缀强制**:配方名必须 `r_` 开头;字段/块名禁止 `r_` 开头(§2.2)。
2. **block 的 fields 字段必须存在**:每个 block 的 `fields` 列表里的字段名,必须在所属结构体的 members 里存在。否则报错 `"block field not found in struct: <field> (in struct <struct>)"`。
3. **block 必须有 sep + fields**(若声明为 block):缺 sep 报错,缺 fields 报错。
4. **用户子配方循环检测**:`r_a` 引用 `r_b` 引用 `r_a` → 报错(§5.3)。
5. **`${name}` 引用解析**:被引用的 name 必须是「字段(lookup)」「块(structDecls)」「结构体数组(structArrayDecls)」「连接符(connectors)」「用户子配方(byName)」之一。否则报 `"unknown name"`。
   - **结构体块现在在 structDecls 里**(和现在一样),`${person}` 引用它 —— 但展开规则不再来自子配方,而来自 schema 的 fields+sep(Builder 组装)。
6. **去掉的检查**:subRecipe 存在性、结构体块→子配方循环(§4.3)。

## 7. 配置解析(ConfigLoader)

`RecipeAst` 加 desc 字段:
```cpp
struct RecipeAst {
    std::string name;
    std::string desc;   // 新增
    std::vector<Segment> segments;
};
```
`ConfigLoader::parse` 里 `ra.desc = rc.value("desc", "");`。

**schema 解析**(codegen 侧):`gen_registry.py` 读 block 的 `sep`/`fields`/`desc`,生成新的注册调用(见 §8)。

## 8. codegen 改动

### 8.1 block 注册签名调整

`NameRegistry::registerStruct` / `registerStructArray` 签名改为接收 fields + sep(取代 subRecipeName):

```cpp
// 单结构体块
void registerStruct(std::string name, Navigator nav,
                    std::vector<std::string> fieldNames, std::string sep,
                    std::string desc = "");
// 结构体数组
void registerStructArray(std::string name, IndexedNavigator nav,
                         std::vector<std::string> fieldNames, std::size_t count,
                         std::string sep, std::string arraySep,
                         std::string desc = "");
```
- `fieldNames` = schema 里 block 的 `fields` 列表(字段名,Builder 据此查 NameRegistry 拿 provider)。
- `sep` = 块内字段间分隔符;`arraySep` = 结构体数组的元素间分隔符(仅 registerStructArray)。
- `desc` 可选。

### 8.2 生成的注册代码(示意)

```cpp
// schema: {"block":"person","ptr":true,"sep":"-","fields":["name","age"],"desc":"人体信息"}
reg.registerStruct("person", sv::Navigator([](const void* p)->const void*{
        const Event* s = static_cast<const Event*>(p); return s->person;
    }), {"name", "age"}, "-", "人体信息");

// schema: {"block":"boxes","sep":"|","fields":["x","y","w","h","tags"],"array":{"count":4,"sep":"|"},"desc":"检测框数组"}
reg.registerStructArray("boxes", sv::IndexedNavigator([](const void* p, std::size_t i)->const void*{
        const Event* s = static_cast<const Event*>(p); return &s->boxes[i];
    }), {"x", "y", "w", "h", "tags"}, 4, "|", "|", "检测框数组");
```

### 8.3 field/device 注册带 desc

`field_line`/`field_array_line`/`device_line` 读 member 的 `desc`(缺则 `""`),传给 `registerProvider`:
```cpp
reg.registerProvider("x", sv::makeProvider(...), "检测框横坐标");
```

## 9. 现有示例迁移(破坏性)

### 9.1 schema.json

- 每个 `block` 去掉 `subRecipe`,加 `sep` + `fields` + `desc`。
- 字段加 `desc`(可选)。

### 9.2 recipes.json

- 删除 `r_person`/`r_box`/`r_box_arr`(结构体块子配方 —— 冗余,展开规则在 schema 了)。
- 顶层配方 `r_alarm_line` 保留,template 不变(它引用 `${person}`/`${rect}`/`${boxes}`,这些块现在按 schema 规则展开)。
- 若有用户组合子配方(如 `r_meta`),保留。
- 配方名加 `r_` 前缀;加 `desc`。

### 9.3 main.cpp / 测试

- `render("alarm_line", ...)` 改为 `render("r_alarm_line", ...)`。
- registry_gen.cpp 重新生成。

## 10. 测试策略

| 层 | 测试 | 验证 |
|---|---|---|
| Validator | `test_validator.cpp` 加 case | 配方名无 `r_` → 报错;字段名 `r_` 开头 → 报错;block fields 字段不存在 → 报错;用户子配方循环 → 报错;合规通过 |
| NameRegistry | `test_name_registry.cpp` 加 case | `registerProvider(name, vp, desc)` 存 desc;`describe(name)` 返回正确 desc;`registerStruct` 新签名(fields+sep)存声明 |
| StructBlockProvider | `test_value_provider.cpp` 改 case | 块 provider 持字段列表+sep,`get()` 按 fields 顺序 + sep 拼接(不再跑子配方) |
| Builder | `test_builder.cpp` 改 case | 块编译为字段列表 provider(不再绑定子配方);用户子配方仍正常编译 |
| Engine | `test_engine.cpp` 加 case | `loadConfig` 后 `describeRecipe("r_alarm_line")` 返回 "告警行";`describe("x")` 返回 "检测框横坐标" |
| codegen | `test_codegen.py` 改 case | 生成的 `registerStruct` 带 fields+sep+desc;`registerProvider` 带 desc |
| 端到端 | `test_example_e2e.cpp` + `main.cpp` 迁移 | 配方名 `r_` 前缀 + desc,块按 schema 规则展开,输出正确 |
| 热加载 | `test_hot_reload.cpp` | 迁移到 `r_` 前缀 + 新块机制,TSan 0 race |
| Route A | `test_route_a.cpp` | SubRecipeBinding 改为字段列表,parity 与 Route B |

## 11. 影响范围(改动清单)

| 文件 | 改动 |
|---|---|
| `tools/gen_registry.py` | block 分支生成 `registerStruct(name, nav, fields, sep, desc)`(不再 subRecipe);field/device 带 desc;读 schema 的 fields/sep/desc |
| `include/struct_view/NameRegistry.hpp` + .cpp | registerStruct/registerStructArray 签名改(fields+sep 取代 subRecipe);加 descs_ 存储 + describe() |
| `include/struct_view/ValueProvider.hpp` + .cpp | StructBlockProvider 改为持字段列表+sep(不再持子配方);ArrayStructBlockProvider/A 同理 |
| `include/struct_view/Recipe.hpp` | Route A 的 SubRecipeBinding 改为字段列表+sep(不再持 RecipeA*) |
| `src/RecipeRunner.cpp` | runRecipeA 的 SubRecipe case 改为遍历字段列表 |
| `include/struct_view/ConfigAst.hpp` | RecipeAst 加 desc 字段 |
| `src/ConfigLoader.cpp` | 解析 recipe 的 desc 字段 |
| `src/Validator.cpp` | 加 r_ 前缀校验 + block fields 存在性 + 用户子配方循环检测;去掉 subRecipe 检查 |
| `src/Builder.cpp` + `src/BuilderA.cpp` | 块编译改为组装字段列表 provider(不再绑定子配方);Pass 2 的子配方绑定去掉 |
| `include/struct_view/Engine.hpp` + .cpp | 加 recipeDescs_ + describeRecipe();loadConfig 解析配方 desc |
| `examples/security/*` | schema 加 sep/fields/desc 去 subRecipe;recipes 删子配方加 r_ 前缀;main/test 迁移 |
| 所有测试 | 迁移到新签名 + r_ 前缀 + desc 断言 |

## 12. 明确的 YAGNI / 不做项

- **不做「组合字段」新机制**(ss = a+b+x+y 作为字段级 provider) —— 用户组合走子配方。
- **不让 block 支持 template 自由排版**(只能 fields + sep) —— 块展开规则刻意收进 schema,要自由排版就用用户子配方。
- **desc 不参与 `${...}` 引用** —— 纯元数据。
- **不给原生字段/块加 `r_` 前缀** —— 它们是基准,无前缀;只有配方(用户产物)加 `r_`。
