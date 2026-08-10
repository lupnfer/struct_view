# Recipe Builder UI —— 设计 spec

- 日期: 2026-08-10
- 状态: 已批准 (设计阶段)
- 关联: `docs/usage-guide.md`(使用指导)、`docs/superpowers/specs/2026-08-07-struct-view-name-convention-design.md`(命名约定)

## 1. 目标

给 client 侧使用者提供一个 **Web 可视化界面**,替代手写 recipe JSON。client 在浏览器里看到维护者提供的 name 清单(带 desc),通过拖拽拼接 + 输入连接符自动组合字符串,实时预览输出,一键部署到服务侧。

## 2. 已确定的关键约束

| 维度 | 决定 |
|---|---|
| 技术栈 | Web 页面(单文件 HTML/JS/CSS,零构建零依赖) + 后端嵌入式 HTTP server(cpp-httplib 单头文件) |
| 核心交互 | 可视化拖拽拼接 —— name 清单 → 拖到组合区 → 自动生成模板 + 实时预览 |
| 预览 | 服务侧预览 API(POST /api/preview)—— 临时编译 + render,不替换线上配方 |
| 部署 | POST /api/deploy —— 正式 loadConfig,热加载生效 |
| 连接符 | 界面内 name 之间直接输入字面字符 |
| 用户子配方 | 界面内「另开标签页」拼接,保存为 r_<名>,可被其他配方拖入引用 |
| 分隔符覆盖 | name 拖入后可设 :sep= / :isep= 参数(弹出设置面板) |

## 3. 三层架构

```
浏览器 (前端)                    后台服务 (C++)
──────────                       ──────────
Recipe Builder UI                 struct_view Engine
  ├ name 清单(左侧)    ←GET /api/names→  NameRegistry + ConnectorLib + recipeDescs
  ├ 组合区(拖拽拼接)   ←POST /api/preview→  临时编译 + render(示例数据)
  ├ 预览输出           ←POST /api/deploy→   loadConfig(热加载)
  └ 子配方标签页
```

## 4. 后端 API(3 个)

### 4.1 GET /api/names —— 可用 name 清单

**响应:**
```json
{
  "names": [
    {"name": "time",      "type": "field",        "desc": "时间戳",     "canSep": false, "canIsep": false},
    {"name": "feat_ids",  "type": "scalar_array", "desc": "特征ID列表", "canSep": true,  "canIsep": false, "defaultSep": "-"},
    {"name": "person",    "type": "struct_block", "desc": "人体信息",   "canSep": true,  "canIsep": false, "defaultSep": "-"},
    {"name": "rect",      "type": "struct_block", "desc": "检测框",     "canSep": true,  "canIsep": false, "defaultSep": ","},
    {"name": "boxes",     "type": "struct_array", "desc": "检测框数组", "canSep": true,  "canIsep": true,  "defaultSep": "|", "defaultIsep": ","},
    {"name": "camera",    "type": "device",       "desc": "摄像头编号", "canSep": false, "canIsep": false},
    {"name": "dash",      "type": "connector",    "desc": "",           "canSep": false, "canIsep": false}
  ],
  "recipes": [
    {"name": "r_alarm_line", "desc": "告警行"}
  ]
}
```

**后端实现**:遍历 NameRegistry 的 entries_ + structDecls_ + structArrayDecls_ + scalarArrayDecls_ + ConnectorLib,组装 NameInfo 列表为 JSON。Engine 的 recipeDescs_ 提供已有配方清单。

### 4.2 POST /api/preview —— 预览(临时编译 + render)

**请求:**
```json
{
  "recipes": [
    {"name": "r_preview", "template": "${camera}:${time}|${feat_ids:sep=*}"}
  ],
  "recipeName": "r_preview"
}
```

**响应(成功):**
```json
{"ok": true, "output": "CAM001:1717171717|11*22*33*0*0*0*0*0"}
```

**响应(失败):**
```json
{"ok": false, "errors": [{"recipe": "r_preview", "message": "unknown name: xxx", "line": 0}]}
```

**后端实现**:
1. 用 Engine 的 NameRegistry + ConnectorLib,**临时**编译 recipe(parse → validate → Builder::compile)。
2. **不调 publishAll**(不影响线上配方)。
3. 用预注册的示例数据 render 一次。
4. 返回输出或错误清单。

**示例数据**:服务启动时准备一份示例 Event 结构体 + MyDeviceCtx,preview 专用。

### 4.3 POST /api/deploy —— 正式下发(loadConfig)

**请求:**
```json
{
  "recipes": [
    {"name": "r_alarm_line", "desc": "告警行", "template": "${camera}:${time}|${person}@${rect}|${feat_ids}|${boxes}"},
    {"name": "r_meta", "desc": "元数据", "template": "${camera}:${time}|${feat_ids}"}
  ]
}
```

**响应:**
```json
{"ok": true}
```
或:
```json
{"ok": false, "errors": [{"recipe": "r_alarm_line", "message": "recipe name must start with 'r_'", "line": 0}]}
```

**后端实现**:直接调 `engine.loadConfig(recipeJson)`,返回 LoadResult。成功 = 热加载生效;失败 = 旧配方继续服务(绝不半发布)。

## 5. 后端新增组件

### 5.1 NameRegistry::exportNames()

新增方法,导出所有可用 name 的元数据:

```cpp
struct NameInfo {
    std::string name;
    std::string type;        // "field" / "scalar_array" / "struct_block" / "struct_array" / "device" / "connector"
    std::string desc;
    bool canSep;             // 支持 :sep= 覆盖
    bool canIsep;            // 支持 :isep= 覆盖(仅 struct_array)
    std::string defaultSep;  // 默认分隔符(数组/块有)
    std::string defaultIsep; // 默认内部分隔符(仅 struct_array)
};
std::vector<NameInfo> exportNames() const;   // NameRegistry
```

Engine 暴露配方清单(已有 recipeDescs_ + describeRecipe())。

### 5.2 HTTP server(cpp-httplib)

新增 `src/server.cpp`,用 cpp-httplib 单头文件嵌入 HTTP server,注册 3 个路由:

```cpp
httplib::Server svr;
svr.Get("/api/names", ...);     // 导出 name 清单
svr.Post("/api/preview", ...);  // 临时编译 + render
svr.Post("/api/deploy", ...);   // loadConfig
svr.set_mount_point("/", "web"); // 静态文件(前端 HTML)
svr.listen("0.0.0.0", 8080);
```

### 5.3 示例数据

服务启动时预注册一份示例数据(供 preview 用):
```cpp
Event sampleEvent{1717171717, &samplePerson, {100,200,300,400,{7,8}}, {11,22,33,0,0,0,0,0}, {...}};
MyDeviceCtx sampleCtx;  // cameraId() = "CAM001"
```

### 5.4 JSON 序列化

后端需要把 NameInfo / LoadResult / preview output 序列化为 JSON。用已有的 nlohmann/json(vendored)。

## 6. 前端设计(单文件 HTML/JS/CSS)

### 6.1 页面布局

```
┌─────────────────────────────────────────────────────────┐
│ struct_view Recipe Builder                     [部署▼]  │
├──────────────┬──────────────────────────────────────────┤
│  可用 name   │  组合区 (当前配方: r_alarm_line)          │
│  ──────────  │  ┌─────┐ : ┌──────┐ | ┌──────┐ @ ┌────┐ │
│  📦 time     │  │time │   │camera│  │person│  │rect│  │
│     时间戳   │  └─────┘   └──────┘  └──────┘  └────┘  │
│  📦 camera   │       | ┌──────┐ | ┌──────┐             │
│  📦 person   │       │feat_ │  │boxes │              │
│  📦 rect     │       │ids   │  └──────┘             │
│  📦 feat_ids │       └──────┘                        │
│  📦 boxes    │  [字面连接符在 name 之间直接输入]         │
│  ────────    │  模板: ${camera}:${time}|${person}@...   │
│  🔗 dash     │  预览: CAM001:1717171717|Alice-30@...    │
│  ────────    │                                          │
│  📋 r_meta   │  [子配方标签页] [r_meta] [r_alarm_line]  │
│              │  [预览] [部署]                           │
└──────────────┴──────────────────────────────────────────┘
```

### 6.2 交互流程

1. **加载**:页面加载调 GET /api/names,左侧展示 name 清单(分组 + desc + 类型图标)。
2. **拖拽**:从左侧拖 name 到组合区,自动在模板插入 `${name}`。
3. **连接符**:在组合区 name 之间点击间隙,输入字面字符(如 `:` `|` `@`)。
4. **分隔符覆盖**:点击拖入的数组/块 name,弹出设置面板(sep/isep 输入框),设置后模板更新为 `${name:sep=xxx}`。
5. **实时预览**:组合区每次变化,自动 POST /api/preview,返回输出显示在预览区。
6. **子配方**:点「+ 新子配方」开新标签页,拼接 + 命名 r_<名> + desc,保存后出现在左侧。
7. **部署**:点「部署」POST /api/deploy,成功提示「已热加载」,失败显示错误清单。
8. **JSON 源码视图**:可切换到直接编辑 JSON(高级用户)。

### 6.3 模板自动生成

可视化操作自动生成模板字符串:
- 拖入 `time` → `${time}`
- name 后输入 `:` → `${time}:`
- 拖入 `camera` → `${time}:${camera}`
- 设 `feat_ids` 的 sep=`*` → `${feat_ids:sep=*}`

## 7. 文件结构

```
struct_view/
├── web/
│   └── index.html          # 前端单文件(HTML+JS+CSS)
├── src/
│   ├── server.cpp          # HTTP server(cpp-httplib,3 个 API)
│   └── server.hpp          # server 接口
├── include/struct_view/
│   └── NameRegistry.hpp    # 加 exportNames() + NameInfo
├── third_party/
│   └── httplib/
│       └── httplib.h       # vendored cpp-httplib 单头文件
├── examples/security/
│   └── server_main.cpp     # 服务入口(装配 Engine + 启动 HTTP server + 示例数据)
└── CMakeLists.txt          # 加 server 目标
```

## 8. 测试

| 层 | 测试 |
|---|---|
| exportNames | 返回的 name 清单正确(字段/块/数组/设备/连接符全覆盖) |
| /api/names | HTTP GET 返回正确 JSON |
| /api/preview | 临时编译 + render 返回正确输出;不替换 store |
| /api/deploy | loadConfig 成功/失败正确返回 |
| 前端 | 拖拽生成模板正确;预览实时更新;部署成功/失败提示 |
| 集成 | 完整流程:加载 name → 拖拽 → 预览 → 部署 → render 输出正确 |
