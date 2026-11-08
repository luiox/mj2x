# mj2x 路线图

> mj2x 是 morpher 的 bytecode -> native 保护工具线。它的主线不是"把 Java 全量
> AOT"，而是把选定方法/类的语义迁到 native，减少字节码和 JNI 可还原锚点。
> 当前迭代：**v2.2（转正与可用化）**，依据 issue #787 §3；施工计划见
> [dev_plan.md](../dev_plan.md)。

---

## 1. 定位

mj2x 是蓝队保护线，必须和 mjdbg 红队报告形成闭环：

```text
选择 Java 方法/类
  -> 翻译成 native / protected sample
  -> mjdbg 观察 trace 和 native/JNI surface
  -> weakness report
  -> mj2x 调整后端、runtime、intrinsic 或变换 pass
```

三后端并行存在：

- **StackC**：广覆盖、正确性优先，适合作为"能跑"的通用后端和回归基线。
- **RegC**：保护导向，目标是消除 stack-style anchor，减少 c2j 可还原面。
- **VMC4**：VM 虚拟化（保护导向，性能非关键），编译期随机化 + 执行期 dispatch
  drift；v2.2 已完成"可用版"（对象/字段/数组/调用/异常/intrinsic 全覆盖，
  真实 jar 转化率 100%）。

---

## 2. 能力路线图

| 能力 | 目标 | 当前方向 / 状态 |
|---|---|---|
| 输入/输出 | `.class` / `.jar` 输入，改写 class，输出 C/native project | 保持 class/JAR 顺序、资源、manifest 语义稳定 |
| 方法选择 | 全量标记、注解筛选、未来配置筛选 | 用户注解和 CLI 筛选并行 |
| L1 MIR | 从 bytecode 提升到栈式中间表示 | 支撑 StackC 和后续 SSA/RegIR |
| StackC 后端 | 广覆盖 JNI C 后端，正确性优先 | 覆盖 javac 常见形状、异常、monitor、invokedynamic 桥 |
| SSA / RegIR | 消除 stack slot，建立保护导向 IR | 服务 RegC、DCE、MBA、CFF 和整类下沉 |
| RegC 后端 | 寄存器式 C/C++ 输出，无 `cstack` / `stack[]` / `sp` anchor | 保护主线；native 互调默认走 JNI 保栈帧语义，`--regc-direct-calls` 可选直调 |
| LLVM 后端 | textual LLVM IR 输出（对象/数组/调用/异常/monitor/ldc + intrinsic + 保护特性对齐 RegC） | 已转正并二次扩面（默认开启）；String 类方法面与整类下沉留待 v2.3+ 视需求 |
| Codegen 工具层 | CSourceStream、IncludeSet、mangle、literal helper、CodeTemplate 五层模型 | 已落地 |
| Runtime ABI | div/rem/barrier、helper、异常/错误桥接 | 保持 ABI 可测试、可版本化 |
| Runtime core | crypto、kernel、stringpool、antidebug、antijvmti | 反侦测体系已落地（Linux antidebug + enforcement + 检查点注入 + agent 检测） |
| JDK intrinsic | 包装类、Math/Random、String/StringBuilder、位操作、System.arraycopy | 位操作 intrinsic 补全；System.arraycopy intrinsic 已完成；LLVM 侧 intrinsic 扩面对齐 RegC 面（Math/包装类/Random/StringBuilder·StringBuffer 切片直调 mj2x-rt；String 类方法面留待整类下沉） |
| String/UTF-16 | Java String 语义、StringBuilder/StringBuffer | 依赖 libca UTF-16 或 mj2x 最小垫片 |
| L4 变换 pass | DCE、常量传播、MBA、CFF、opaque predicate | 只有 RegC/C 运行时 live 验证稳定后再扩 |
| 整类下沉 | 算法密集类内部方法全 native，边界保留 JNI | 先做无反射、无动态类加载、intrinsic 覆盖充分的类 |
| Protected samples | StackC/RegC 样本、expected trace surface、hardening history | 必须给 mjdbg 攻击 |
| safe-env（JNI 表保护） | 快照 + 入口恢复，`--regc-safe-env`（#748） | **已实现**；mjdbg 侧 JNI-hook 分析工具（#756）作常态化 E2E 对手，进行中 |
| VMC4 可用版 | 完整方法语义的 VM 后端 | **已完成**（对象/字段/数组/调用/异常/intrinsic，转化率 100%）；执行期 dispatch drift 低风险档已落地（升级档待 mjdbg 对抗验证） |
| VMC4 实例粒度 | class / method / program 三档（program = 全程序共享一个 VM，opcode 空间全局统一） | **program 粒度已落地**；三粒度测试锚定（class/method/program 实例共享/独立语义） |
| VMC4 指令级变换 | super-ops 融合 / 指令碎片化 / 寄存器文件定长化 / 块乱序 | **已落地**（`--vmc4-super-ops` / `--vmc4-fragment` / `--vmc4-regfile` / `--vmc4-shuffle-blocks`） |
| 用户 intrinsic | 任意 Java 静态调用点降级为用户 native symbol 直调 | **已完成**（三后端统一 ABI + CLI JSON 配置；文档 `doc/user-intrinsic.rst`） |
| 自建测试集 | mtest_set 语义样本 + 三后端矩阵 + jar 整体 E2E | **第一批已完成**（Wave1-3 G1-G15 样本；用户 intrinsic 三后端 E2E；多类 jar 整体程序三后端 E2E）；**四后端语义回归集已建**（超界/负数移位、字符串比较等锚定用例）；**性能回归已并入测试集**（benchmark `-Regression` 断言 + `mj2x_integration_test` 门控用例）；mjdbg 对抗闭环待做 |
| 跨后端语义一致性 | 同一 JVM 语义四后端不漂移（掩码/槽序/异常表序） | 掩码决策已单源化（`codegen/jvm_semantics.hpp`，RegC/LLVM 共用）；RegIR 层语义显式化（决策挂指令属性、发射端断言、按标签扩回归集）为 v2.3+ 候选 |
| RegC 性能 | 追平/反超 JNIC（os30）基线 | **已完成第一轮**（JDK17/21 双矩阵，0.88x-1.16x）；SSA 方法内联默认开启；System.arraycopy intrinsic 已完成；后续按 v2.3+ 视需求推进 |
| 混淆与保护变换族 | MBA/CFF/opaque/立即数隐藏/调用间接化/computed jumps/POD struct/作用域收窄 | **已落地**（`--regc-obfuscate` + `--regc-*` 增强档 + `--obf-scope-include/exclude`，见 `doc/user-guide.rst` §4.0.1） |
| 配置系统 | 参数注册表单源 + 全量参数配置化 + JSON Schema 导出 | **P0+P1 已完成**（单源 ParamDef 表驱动 CLI/config/help/schema + preset 档 + 生效配置回显）；GUI 工具待做（见 dev_plan D） |
| Release packaging | CLI、runtime、注解包、样本和验证脚本 | v2.1 已纳入发布矩阵；v2.2 持续 |

---

## 3. v2.2 优先级（issue #787）

已完成：VMC4 可用版（转化率 100%）、反侦测体系、异常语义精确化（指令序号区间
判定）、RegC 性能第一轮（intrinsic 补全 / 异常检查稀疏化 / resolved dispatch
双槽缓存 / 发射器模板化 / SSA 内联 / System.arraycopy intrinsic）。

进行中/待做：

1. **mjdbg 侧 JNI-hook 分析工具**（#756）：safe-env（#748）需要常设攻击对手
   才能成为可回归的 E2E 指标。
2. **VMC4 升级档**（#762）：period 语义变义——待 mjdbg 对抗验证闭环后实施。

---

## 4. 不做的事

- 不做全 AOT；反射、动态代理、类加载、Instrumentation 等场景默认不承诺。
- 不把 StackC 宣称为强保护；它是正确性和覆盖率基线。
- 不在 mjdbg 还没有报告前自称保护强度已经提升。
- VMC4 性能优化（v2.3+，可用版已交付但性能非目标）。
- ldc Handle/MethodType 常量**已真实支持**（findXxx 描述符展开 + 栈形校验器
  单测 + exec E2E 守门）；其余不支持场景维持「显式拒绝（fail-closed 保字节码
  + 可观测日志）」承诺，不走静默降级。

---

## 5. 共享契约

| 契约 | 消费者 | 规则 |
|---|---|---|
| protected sample format | mjdbg、validation | 每个样本必须写 expected trace surface |
| runtime ABI | mj2x、mjdbg native trace | helper/ABI 改动需单测和版本说明 |
| trace expectation | mjdbg | 隐藏或随机化可观察信号前先协调 |
| libjbytecode usage | mbase | 公共 helper 需协调，直接使用底层 API 可以局部推进 |
| build options | build/global | 不静默引入 LLVM、Qt、额外 native toolchain 作为默认依赖 |

---

## 6. 验证

```powershell
xmake f --with_tests=y -y
xmake build mj2x_unittest
xmake run mj2x_unittest --gtest_brief=1
xmake build mj2x_integration_test
xmake run mj2x_integration_test --gtest_brief=1
```

native 相关 PR 必须记录 OS、JDK、C 编译器、是否运行生成产物、是否产出 mjdbg
observation/weakness report。
