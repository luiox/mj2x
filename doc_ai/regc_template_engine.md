# RegC 模板化源码生成引擎（方案 B：嵌入式 C++ DSL）

> 状态：设计中（issue #781 的实现设计；方案 A 外部模板文件见 issue #783，当前不实施）
> 适用范围：RegC emitter（`regc_emitter.cpp`）优先铺开；CSourceStream 是 StackC/RegC/vmc4 共享工具，API 变更不保证向后兼容（用户确认 ABI 可不兼容），编译错误驱动同步修调用点。
> 修订规则：先改本文档，再改代码（与 codegen_style.md 一致）。

## 1. 问题与目标

### 1.1 现状痛点（issue #781 背景）

- `regc_emitter.cpp` 仍有 708 处 `out.append(...)` 手拼：`"    {\n"`、`"    } else {\n"`、`"(" + sig + ")" + expr + "(" + args + ")"`。括号、缩进、else 匹配关系散落在字符串字面量里，编译器无法检查。本会话已因此类错误反复返工。
- 开关散落：`RegCEmitter` 构造参数（traceCallLog/resolvedDispatch/classIndex/...）与 `RegCCommentLevel` 枚举各自为政，"要不要生成某段代码"的决策无法集中查询、配置与打日志。
- 表达式构建全是手拼，无纯函数构建器，无法单独单测。

### 1.2 目标

1. **结构即数据**：括号/缩进/else 匹配由数据结构（RAII + lambda 块）保证，错位在编译期不可能发生。
2. **开关系统**：集中的 `CodeOptions`，贯穿所有发射点，来源可配置（CLI/backend 配置）。
3. **表达式构建器**：纯函数，独立单测，消灭手拼括号。
4. **模板化**：雷同片段提为命名模板复用。
5. **引擎先守门**：引擎核心 + 独立单元测试先完成，迁移基于稳定 API。

### 1.3 非目标

- 外部模板文件（方案 A，issue #783 触发条件满足前不做）。
- 改变生成代码语义（迁移全程生成代码行为等价，13-probe 全过为准）。
- 一次性重写 emitter（分步迁移，每步编译 + 快测试组 + 括号配平断言）。

## 2. 分层模型（三层 → 五层）

沿用 codegen_style.md §3 的三层（排版/拼接/驻留），在排版层之上细化发射层：

| 层 | 工具 | 职责 |
|---|---|---|
| L0 排版 | `CSourceStream`（已有） | 缩进/换行/RAII 守卫；底层 `Utf8StringBuilder` |
| L1 语句 | `CSourceStream` 语句辅助（已有 + 补充） | `emitLine/assign/declare_var/ret/call/call_fn_ptr/while_/for_` + 新增 `switch_/case_/goto_/label_/comment` |
| L2 表达式 | `CExpr`（新，`c_expr.hpp`） | 纯函数构建 C 表达式文本（括号内聚） |
| L3 结构 | `CodeTemplate` + `CodeOptions`（新） | 命名模板复用；开关查询与作用域覆盖 |
| L4 指令 | `RegCEmitter` 指令发射器（迁移目标） | 消费 RegIR，组合 L2/L3 |

拼接层（Utf8StringBuilder）/驻留层（Arena）职责不变。

## 3. 构件详设

### 3.1 L2 `CExpr`（表达式构建器，`mj2x-core/src/mj2x/codegen/c_expr.hpp`）

- 全部为 **纯函数**，返回 `std::string`（短生命周期值对象，非拼接容器，不违反 codegen_style.md §3.3；产物仍只经 CSourceStream 入流）。
- 每个函数内部自行处理括号内聚：调用方永远不需要写 `"(" + ... + ")"`。

```
std::string exprCall(fn, args...);                          // fn(a, b)
std::string exprCallArgs(fn, const std::vector<std::string>&); // 动态数量参数
std::string exprCast(type, expr);                           // (type)expr
std::string exprFnPtrCall(fnPtrSig, fnExpr, args...);       // ((sig)fn)(a, b)
std::string exprMember(obj, member);                        // obj.member
std::string exprIndex(arr, idx);                            // arr[idx]
std::string exprBinop(op, lhs, rhs);                        // lhs op rhs（不额外括号，调用方按需 exprParen）
std::string exprTernary(cond, then, els);                   // cond ? t : e
std::string exprParen(expr);                                // (expr)
std::string exprNull();                                     // (void*)0 统一空指针写法
```

约定：嵌套调用结果已含必要括号（`exprCall("f", exprCast("int32_t", "x"))` 直接可用）；`exprBinop` 不隐式加括号，优先级由调用方用 `exprParen` 显式表达（生成代码可读性优先）。

### 3.2 L3 `CodeOptions`（开关系统，`mj2x-core/src/mj2x/codegen/c_options.hpp`）

设计决策：**强类型字段，不用字符串表**。开关数量有限（<20），编译期检查价值高于"免改结构扩展"；字符串表拼写错误静默退化为默认值是事故源头。

```
struct CodeOptions {
    // 来自现有 RegCEmitter 构造参数（收敛点）
    RegCCommentLevel commentLevel = RegCCommentLevel::Summary;
    bool traceCallLog        = false;
    bool resolvedDispatch    = false;
    bool emitDirectCalls     = false;   // classIndex != nullptr 时派生
    bool emitTypeAsserts     = true;    // 示例新开关：类型断言代码
    bool emitDebugComments   = false;   // 示例新开关：逐指令调试注释
    bool emitCacheTrace      = true;    // cache 槽溯源注释
    // ...

    // 作用域覆盖：构造时保存旧值，析构恢复（RAII，支持嵌套）
    struct ScopeOverride;
    ScopeOverride overrideTemporarily(bool& field, bool value);
};
```

- 收敛：`RegCEmitter` 构造参数改为接收 `const CodeOptions&`（内部按需拷贝），`RegCCachePool` 注释级别仍独立（TU 级，二者保持同步赋值）。
- 传播：`CSourceStream` 不直接持有 options；发射函数按需读取 `state.opts`（`RegCFunctionEmitState` 挂 `CodeOptions` 引用），跨层 helper 只传需要的 bool 或引用。
- 组合：`t.iff(state.opts.emitTypeAsserts, ...)` —— 开关就是 iff 的布尔源，不发明新语法。

### 3.3 L3 `CodeTemplate`（命名模板，`mj2x-core/src/mj2x/codegen/c_template.hpp`）

```
class CodeTemplate {
public:
    static CodeTemplate define(std::string_view name, CBody body);
    void emit(CSourceStream& out) const;                     // 无参实例化
    CodeTemplate& dependsOn(std::string_view optionName);    // 依赖声明（文档/校验）
    std::string_view name() const;
private:
    std::string name_;
    std::function<void(CSourceStream&)> body_;
    std::vector<std::string> dependencies_;
};
```

- 参数化：`define` 时以 lambda 捕获参数（值捕获），`emit(out)` 即带参实例化；需要动态参数时 `emit` 不接受参数，改用 `define(name, [&](CSourceStream& o){ ... })` 现场捕获——模板本身只是**命名 + 复用 + 可验证**单元。
- 注册表：`CodeTemplateRegistry`（`std::map<std::string, CodeTemplate>`），通用片段放 codegen 层，RegC 专用片段放 emitter 侧；单测可遍历验证模板存在性与输出。
- 首批模板候选：edge copies 序列、异常表检查序列、JNI 内联调用序列、类型转换序列、resolved call site 快路径检查。

### 3.4 L1 `CSourceStream` 补充（`c_source.hpp`）

```
void switch_(expr, body);     // switch (expr) { ... }
void case_(label, body);      // case L: { ... }
void goto_(label);            // goto label;
void label_(name);            // label:
void comment(text);           // // text（缩进随上下文；内容不经转义，调用方负责 escapeCommentIdent）
```

`switch_/case_` 与现有 `block/iff/while_/for_` 同构（花括号 + IndentGuard 由流管理）。case 语句体需要 `break` 或 `goto` 时在 body lambda 内显式发出。

### 3.5 守门测试：括号配平断言

`codegen` 测试工具新增 `assertBalancedC(text)`：词法级扫描（跳过字符串字面量与 `//`、`/* */` 注释），断言 `() {} []` 成对且顺序合法。**所有**表达式/结构单测与后续迁移后的生成文本均过此断言——这是"根治括号问题"的可执行守门。

## 4. 单元测试（引擎先行，迁移前完成）

新文件 `mj2x/unittest/src/codegen/codegen_expr_test.cpp`（CExpr/CodeOptions/CodeTemplate）与扩展 `codegen_test.cpp`（CSourceStream 补充语句）：

- CExpr：每个构建器基础输出；嵌套组合（`exprCall(f, exprCast(...))`）括号平衡；空参/多参边界。
- CSourceStream：`switch_/case_/goto_/label_/comment` 输出结构 + 与 `iff/block` 混排；嵌套缩进正确。
- CodeOptions：默认值；`ScopeOverride` 作用域内改变、析构恢复、嵌套恢复顺序。
- CodeTemplate：define/emit 输出；参数捕获实例化；注册表去重；dependsOn 声明可读。
- `assertBalancedC`：合法/非法样例（字符串内括号不计数、注释内不计数）。

验收：`xmake run mj2x_unittest --gtest_filter="CSourceStreamTest.*:CodegenExprTest.*:CodegenTemplateTest.*:CodegenOptionsTest.*"` 全绿，且与现有 401 测试无冲突。

## 5. 迁移路线（分步，每步语义等价）

| Step | 范围 | 责任 |
|---|---|---|
| 0 | 引擎核心 + 单测 + 本文档 + dev_plan | 主 agent |
| 1 | L2 表达式层：emitIntrinsicCall/emitCachedCall/emitResolvedInstanceCall/operandExpr 组合处 → CExpr | sub agent A |
| 2 | L1 语句层：708 处 append 中 assign/declare_var/ret/call/call_fn_ptr 可替换点 | sub agent B |
| 3 | L3 结构层：emitTerminator（jump/branch/switch）、emitFunction 骨架、edge copies → switch_/case_/goto_/label_ + block | sub agent C |
| 4 | RegCEmitter 构造参数收敛为 CodeOptions，接入 CLI 配置 | 主 agent |
| 5 | 全量验证：13-probe + 401 单测 + 括号配平扫描全部生成文本 | 主 agent |

每个 step 验收：编译通过 + `RegCTest.*:RegCIntrinsicTest.*` 快测试组全绿 + 生成文本过 `assertBalancedC`。

## 6. 文档联动

- 本文档为设计总纲；`codegen_style.md` §3 的三层模型升级为五层（§2 表格），§3.3 禁止条款补充"手拼 `(`/`)` 表达式文本"。
- `dev_plan.md` 维护本分支迁移进度。
