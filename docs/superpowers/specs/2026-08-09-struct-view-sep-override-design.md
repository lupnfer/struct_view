# `${name:sep=}` / `${name:isep=}` 分隔符覆盖 —— 设计 spec

- 日期: 2026-08-09
- 状态: 已批准 (设计阶段)
- 关联: `docs/superpowers/specs/2026-08-07-struct-view-name-convention-design.md`(命名约定 + 块展开规则)

## 1. 目标

当前 schema 里的 `sep`(数组元素间 / 块内字段间分隔符)是**固定值**,codegen 把它烤进生成的 provider 代码里。改分隔符要改 schema + 重跑 codegen + 重编译,无法热加载。

目标:**分隔符在 recipe 里用 `${name:sep=xxx}` 覆盖**,schema 里的 sep 降为**默认值**(fallback)。改分隔符只改 recipe(热加载),不动 schema。

## 2. 语法

### 2.1 两个覆盖参数

| 参数 | 覆盖什么 | 适用类型 |
|---|---|---|
| `:sep=xxx` | **主分隔符**:标量数组元素间 / 结构体块字段间 / 结构体数组元素间(arraySep) | 标量数组、结构体块、结构体数组 |
| `:isep=xxx` | **内部分隔符**:结构体数组元素内字段间(sep) | 仅结构体数组 |

### 2.2 组合与单独使用

```
${feat_ids:sep=-}           标量数组,元素间用 -
${rect:sep=,}               结构体块,字段间用 ,
${boxes:sep=|:isep=,}       结构体数组,元素间用 |,元素内字段间用 ,
${boxes:sep=|}              结构体数组,元素间用 |,元素内用 schema 默认
${boxes:isep=,}             结构体数组,元素间用 schema 默认,元素内用 ,
${boxes}                    全用 schema 默认(向后兼容)
```

`:sep=` 和 `:isep=` 可组合(顺序无关),也可单独用。不带的参数用 schema 默认。

### 2.3 标量字段/设备

`:sep=`/`:isep=` 对标量字段和设备 getter无意义(它们没有分隔符)。Validator 报错: `"sep/isep override on non-separable name: <name>"`。

## 3. schema 变化

schema 里的 `sep` 全部变为**默认值**(语义不变,但角色从「固定值」降为「fallback」):

- `block.sep` —— 块内字段间默认分隔符。
- `array.sep`(标量数组) —— 元素间默认分隔符。
- `array.sep`(结构体数组的 `block.sep`) —— 元素内字段间默认分隔符。
- `array.sep`(结构体数组的 `array` 对象) —— 元素间默认分隔符(arraySep)。

> 命名提醒:结构体数组有两层 —— `block.sep`(元素内字段间,默认 isep)+ `array.sep`(元素间,默认 sep)。`${boxes:sep=|}` 覆盖 `array.sep`;`${boxes:isep=,}` 覆盖 `block.sep`。

## 4. 分词器扩展(ConfigLoader)

### 4.1 Segment 加两个可选字段

```cpp
struct Segment {
    bool isRef;
    std::string text;         // name
    std::string sepOverride;  // 从 :sep=xxx 提取,空则用默认
    std::string isepOverride; // 从 :isep=xxx 提取,空则用默认(仅结构体数组)
};
```

### 4.2 分词规则

`${...}` 内容解析:
- 纯 name:`${feat_ids}` → sepOverride="", isepOverride=""
- 带参数:`${name:sep=xxx}` / `${name:isep=yyy}` / `${name:sep=xxx:isep=yyy}`
- 参数顺序无关:`${name:isep=yyy:sep=xxx}` 等价
- `=` 后的值到下一个 `:` 或 `}` 为止(不支持转义,分隔符通常是单字符如 `-`/`|`/`,`)

## 5. Builder 改动

编译 `${name}` 引用时,从 Segment 取 `sepOverride`/`isepOverride`:
- 若非空,用覆盖值替代 schema decl 的默认 sep 创建 provider。
- 若空,用 schema 默认(现状不变)。

具体:
- **标量数组**:provider 是 `LambdaProvider`(codegen 生成的循环代码,sep 烤在里面)。覆盖 sep 需要改为**运行时 sep** —— provider 不再烤死 sep,而是接受一个 sep 参数。这需要改 codegen 生成「sep 可配」的 provider,或改为用 `ArrayStructBlockProvider` 式的「字段列表 + 运行时 sep」机制。**选后者**:标量数组也改为持「格式器 lambda 列表 + sep」(类似 StructBlockProvider,但元素是标量字段不是结构体)。
  > 简化:标量数组的 provider 改为持 `std::function<std::string(const void*)>` (单元素格式器) + count + sep。codegen 生成格式器,Builder 注入 sep(覆盖或默认)。
- **结构体块**:StructBlockProvider 已持 `sep_`,Builder 用覆盖值(或默认)构造。
- **结构体数组**:ArrayStructBlockProvider 已持 `sep_`(元素内)+ `arraySep_`(元素间),Builder 用各自覆盖值(或默认)构造。

## 6. Validator 改动

- `:sep=`/`:isep=` 对标量字段/设备名 → 报错。
- `:isep=` 对非结构体数组名(标量数组/结构体块)→ 报错。
- `:sep=` 对标量数组/结构体块/结构体数组 → 合法。

## 7. 测试

| 层 | 测试 |
|---|---|
| ConfigLoader | `${name:sep=-}` / `${name:isep=,}` / `${name:sep=-:isep=,}` / `${name}`(无参数)分词正确 |
| Validator | `:sep=` 对标量字段报错;`:isep=` 对非数组报错;合法覆盖通过 |
| Builder | 覆盖 sep 的 provider 输出正确分隔符;不覆盖用默认 |
| Engine | `${ids:sep=*}` 输出 `11*22*33...`;`${boxes:sep=@:isep=,}` 输出 `1,2@3,4@...` |
| 端到端 | recipe 用覆盖语法,输出与预期一致 |

## 8. 影响范围

| 文件 | 改动 |
|---|---|
| ConfigAst.hpp | Segment 加 sepOverride/isepOverride |
| ConfigLoader.cpp | 分词 `${name:sep=xxx:isep=yyy}` |
| Validator.cpp | 校验 sep/isep 对非可分类型的报错 |
| Builder.cpp | 读覆盖值构造 provider;标量数组 provider 改运行时 sep |
| BuilderA.cpp | 同 Builder(Route A) |
| ValueProvider.hpp/.cpp | 标量数组 provider 改运行时 sep(若需要新 provider 类) |
| codegen | 标量数组生成「格式器 + count」(sep 不烤死) |
| examples + tests | recipe 用覆盖语法 |
