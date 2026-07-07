# mj2x — 已实现功能清单 (CAPABILITIES)

> 本文档列举 mj2x 当前**已实现且可用**的功能。
> 用法见 `doc/user_guide.md`;路线图见 `doc/roadmap.md`;待办见 `dev_plan.md`。
>
> 能力边界对应 `dev.3` 的实现(2026-07-06 冻结),权威范围见 `doc/mvp_release_scope.md`。

---

## 1. 输入 / 输出

| 能力 | 状态 | 验证 |
|---|---|---|
| 单 `.class` 输入 → 改写后 `.class` + emitted C | ✅ | CBackend / RegC E2E |
| `.jar` 输入 → 改写后 `.jar` + emitted C | ✅ | `mj2x_unittest` JAR pipeline 测试 |
| JAR 输出保留 entry 顺序和非 class 资源 payload | ✅ | JAR fixture 测试 |
| class entry 在 native method 注入后重新写回 | ✅ | JAR pipeline 测试 |

---

## 2. CBackend(广覆盖正确性后端)

定位:正确性基线后端,覆盖广,输出仍是 stack-style(`union jvalue stack[]` + `sp`)。

**已覆盖的 javac 输出形状**:

- 算术:整数 / 长整型 / 浮点运算
- 类型转换:`i2b` / `i2s` / `i2c` / `i2l` 等窄化与拓宽
- 控制流:`if*` / `goto` / switch (`tableswitch` / `lookupswitch`)
- 字段读写:`getfield` / `putfield` / `getstatic` / `putstatic`
- 对象:`new` / `checkcast` / `instanceof`
- 方法调用:`invokevirtual` / `invokespecial` / `invokestatic` / `invokeinterface`
- 数组:`newarray` / `anewarray` / `multianewarray` / `arraylength` / `*aload` / `*astore`
- 异常:`athrow` / try-catch / finally / monitor (`monitorenter` / `monitorexit`)

**验证**:25 个 CBackend 真 E2E(Java → mj2x → C → DLL → Java 行为对比)。

**已知限制**:

| 限制 | 说明 |
|---|---|
| JNI field/method ID 未缓存 | 每次 `GetMethodID` / `GetFieldID`,性能差 |
| 栈全用 `union jvalue` 数组 | 性能低于寄存器方案,后续可优化 |
| `INVOKEDYNAMIC` fail-closed | 生成 `UnsupportedOperationException`,不直译 lambda / 方法句柄 |
| JSR / RET 不纳入 | 废弃字节码,由 preprocessor / 输入规范处理 |
| int/long DIV/REM 除零 | C 层是 UB,非完整 JVM `ArithmeticException` trap 模型 |
| `<init>` / `<clinit>` 不被 MarkAll 标记 | 构造器保留原字节码,不走 native |

---

## 3. RegC(保护导向无栈后端)

定位:保护导向后端,抗 c2j 分析。流水线:L1 stack MIR → SSA → RegIR → RegC。

**结构断言(输出保证)**:RegC 输出不出现 `cstack` / `stack[]` / `union jvalue` / `sp--` 这类 stack-machine anchor。

**已覆盖的 E2E 形状**:

- 纯计算(算术、转换)
- 分支 / 循环
- switch
- 字段读写
- 对象创建与操作
- 方法调用
- 数组操作
- 窄类型(`i2b` / `i2s` / `i2c`)
- throw
- try / catch
- null trap
- integer div / rem trap(走 R3 runtime ABI)

**CLI**:`--backend regc`,支持 `.class` 与 `.jar` 输入输出路径。

---

## 4. R3 Runtime 基础

| 模块 | 状态 |
|---|---|
| runtime core(crypto / kernel / stringpool / antidebug / antijvmti) | ✅ 单测模块 |
| RegC / R3 ABI 冻结 | ✅ 当前冻结在 div / rem / barrier helper |
| runtime-backed div / rem trap | ✅ RegC native E2E 覆盖 |

---

## 5. 生成 xmake 工程

`--generate-project <dir>` 把转 C 结果连同 `xmake.lua` / `.gitignore` / 原 class / (RegC) runtime 源码打包成独立可编译工程。

> 注:该功能当前在分支 `feat/mj2x/xmake-project-generator`,实现为 emit 后字符串改写,待 golden test 建立后重构到 codegen 层。详见分支提交说明。

---

## 6. 验证入口

```bash
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake build mj2x_runtime_test
xmake run mj2x_unittest
xmake run mj2x_runtime_test
```

能力到测试的追溯见 `../doc_ai/mvp_backend_verification.md`。
