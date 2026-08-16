# 跨后端语义决策与回归集（X4 结构性笔记）

## 1. 问题形态

同一 JVM 语义（移位掩码、JNI marshalling、NPE 时机、异常表序、栈槽序）在
4 个后端（StackC / RegC / VMC4 / LLVM）各自实现。语义知识沉淀在各自的发射
代码里，无法传导：StackC 作为正确性基线先做对，保护型后端各自追赶时状态
不一——2026-08 审计发现的移位掩码就是典型：

| 后端 | JVM &31/&63 掩码（JLS 15.19） | 状态来源 |
|---|---|---|
| StackC | ✅ `stackc_mir_arith.cpp`（INT/LONG 全有） | 基线 |
| VMC4 | ✅ `vm_interpreter_emit.cpp`（b&31 / b&63） | 独立实现 |
| RegC | ❌ 裸移位（C count≥位宽 UB） | 漂移 |
| LLVM | ❌ 裸 shl/ashr/lshr（poison）；且 long 移位 i32 计数未 zext（非法定 IR，无 long 移位用例而未暴露） | 漂移 + 验证层注释错误声称等价 |

缺的不是知识而是**跨后端语义回归**：超界/负数移位计数在 JVM 是定义行为，
但 mtest_set 与全部 E2E 都没有这类用例（in-range 移位在 C/LLVM 上恰好与
JVM 一致，掩码缺失不可见）。

## 2. 已落地的收敛手段

1. **共享决策单源**：`mj2x-core/src/mj2x/codegen/jvm_semantics.hpp`——
   `kJvmShiftMaskI32/I64` + `jvmShiftMask(SSAType)`，RegC（C 表达式）与
   LLVM（IR `and`）共用同一决策。新增易漂移语义决策时先落这里，再由各
   发射端消费，不再各自硬编码。
2. **四后端语义回归集**（mtest_set semantic fixture，矩阵自动覆盖）：
   - `g13.shift_out_of_range`：int/long × shl/shr/ushr × 超界(32/33/64/65)/
     负数(-1) 计数。**移位计数走参数**——全常量方法会被 javac/mj2x 常量
     折叠吃掉，测不到发射端掩码。
   - `g12.string_compare`：equals/compareTo/equalsIgnoreCase（字符串比较
     是另一类易漂移点）。
   - 接入：矩阵 StackC/RegC 列自动；LLVM 列 `llvmSupports` 白名单；VMC4 列
     `vmc4_backend_e2e_test.ShiftMaskAndStringCompareRoundTrip`（4 dispatcher
     × drift 全开）。
   - 真值契约：`mtest_set_integration_test` 先在真 JVM（`-Xverify:all`）上
     断言 expectedStdout，杜绝手算错误混进基线。
3. **StackC 文本静态化的防碰撞**：`staticizeStackText` 对节点文本做子串
   改写，必须跳过字符串字面量与 `//` 注释（indy 的 bsm 元数据字符串会经
   NewStringUTF 内联进方法体，是 class 文件可控的碰撞面）——已加
   inString/escape/inComment 状态机 + emit-shape 单测。

## 3. 后续方向（roadmap 候选）

共享常量只是第一步。结构性解法是把「语义决策」（掩码、槽序、异常表序、
marshalling 形态）在共享 RegIR 层**显式化为可断言的中间表示**：语义决策
成为 IR 节点/属性后，各后端只做「形态映射」不做「语义决定」，四后端可以
对同一 RegIR 断言相同语义标签（类似 VMC4 describe() 的语义标签表）。落地
路径建议：

1. 语义决策枚举（shift-mask/arith-wrap/div-rem-trap/npe-checkpoint/
   exception-region-order）挂到 RegIR 指令属性；
2. 四后端发射时断言属性存在（缺失 = 发射端 bug，fail-closed）；
3. mtest_set 语义回归集按标签扩展（新决策 → 新 fixture）。

当前状态：仅移位掩码完成第 1 步的常量化形态（`jvm_semantics.hpp`）；
完整 IR 化留待后续 PR，本笔记记录决策路径与判据。

## 4. 关联

- ldc Handle/MethodType 展开的栈序修正与单测/E2E 见
  `preprocessor/ldc_preprocessor.cpp` 头注释与
  `unittest/src/preprocessor/ldc_preprocessor_test.cpp`（迷你栈校验器）。
- indy 单对象快路径栈形态锁：`StackCBackendTest.InvokeDynamicOneObjectFastPathStackShape`。
- 历史背景：2026-08 语义审计（X1 ldc 展开 VerifyError / X2 移位掩码缺失 /
  X4 结构性根因）。
