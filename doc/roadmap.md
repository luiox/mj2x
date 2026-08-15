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
| RegC 后端 | 寄存器式 C/C++ 输出，无 `cstack` / `stack[]` / `sp` anchor | 保护主线；native 互调默认走 JNI 保栈帧语义（#814/#816），`--regc-direct-calls` 可选直调 |
| Codegen 工具层 | CSourceStream、IncludeSet、mangle、literal helper、CodeTemplate 五层模型 | 已落地（PR #781/#784） |
| Runtime ABI | div/rem/barrier、helper、异常/错误桥接 | 保持 ABI 可测试、可版本化 |
| Runtime core | crypto、kernel、stringpool、antidebug、antijvmti | 反侦测体系已落地（PR #797：Linux antidebug + enforcement + 检查点注入 + agent 检测） |
| JDK intrinsic | 包装类、Math/Random、String/StringBuilder、位操作、System.arraycopy | 位操作 intrinsic 补全（PR #793）；arraycopy 等更宽 intrinsic 待办 |
| String/UTF-16 | Java String 语义、StringBuilder/StringBuffer | 依赖 libca UTF-16 或 mj2x 最小垫片 |
| L4 变换 pass | DCE、常量传播、MBA、CFF、opaque predicate | 只有 RegC/R3 live 验证稳定后再扩 |
| 整类下沉 | 算法密集类内部方法全 native，边界保留 JNI | 先做无反射、无动态类加载、intrinsic 覆盖充分的类 |
| Protected samples | StackC/RegC 样本、expected trace surface、hardening history | 必须给 mjdbg 攻击 |
| safe-env（JNI 表保护） | 快照 + 入口恢复，`--regc-safe-env`（#748） | **已实现**（PR #754/#768）；mjdbg 侧 JNI-hook 分析工具（#756）作常态化 E2E 对手，进行中 |
| VMC4 可用版 | 完整方法语义的 VM 后端 | **已完成**（PR #789/#790/#792/#796/#798/#802：对象/字段/数组/调用/异常/intrinsic，转化率 100%）；执行期 dispatch drift 低风险档已落地（PR #766，#762 升级档待 mjdbg 对抗验证） |
| VMC4 实例粒度 | class / method / program 三档（program = 全程序共享一个 VM，opcode 空间全局统一） | **program 粒度已落地**（#827）；三粒度测试锚定（class/method/program 实例共享/独立语义） |
| 用户 intrinsic | 任意 Java 静态调用点降级为用户 native symbol 直调 | **已完成**（#818，PR #826，三后端统一 ABI + CLI JSON 配置；文档 `doc/user_intrinsic.md`） |
| 自建测试集 | mtest_set 语义样本 + 三后端矩阵 + jar 整体 E2E | **第一批已完成**（#820：Wave1-3 G1-G15 样本；用户 intrinsic 三后端 E2E；多类 jar 整体程序三后端 E2E）；**性能回归已并入测试集**（#820 步骤 5：benchmark `-Regression` 断言 + `mj2x_integration_test` 门控用例）；mjdbg 对抗闭环待做 |
| RegC 性能 | 追平/反超 JNIC（os30）基线 | **已完成第一轮**（PR #793/#803：JDK17/21 双矩阵，0.88x-1.16x）；方法内联已完成（SSA 内联默认开启，消除 native 方法间 JNI 往返）；余项：arraycopy 等更宽 intrinsic |
| 配置系统 | 参数注册表单源 + 全量参数配置化 + JSON Schema 导出 | 📋 **设计完成**（doc_ai/config_system_design.md v1：单源 ParamDef 表驱动 CLI/config/help/schema；GUI 工具路径 = 消费 schema 渲染表单）；P1 实现待做 |
| Release packaging | CLI、runtime、注解包、样本和验证脚本 | v2.1 已纳入发布矩阵；v2.2 持续 |

---

## 3. v2.2 优先级（issue #787）

1. **RegC 性能**（P1，贯穿）：基线已建（benchmark/results 2026-08-13/14），
   第一轮优化完成（intrinsic 补全 / 异常检查稀疏化 / resolved dispatch 双槽缓存 /
   发射器模板化）；余项：方法内联 pass、System.arraycopy 等更宽 intrinsic。
2. **VMC4 可用版**（P2）：✅ 已完成（转化率 100%，见上表）。
3. **反侦测体系**：✅ 已完成（PR #797）。
4. **异常语义精确化**：✅ 已完成（PR #796，指令序号区间判定）。
5. **mjdbg 侧 JNI-hook 分析工具**（#756）：进行中——safe-env（#748）需要常设
   攻击对手才能成为可回归的 E2E 指标。
6. **VMC4 升级档**（#762）：period 语义变义——待 mjdbg 对抗验证闭环后实施。

---

## 4. 不做的事

- 不做全 AOT；反射、动态代理、类加载、Instrumentation 等场景默认不承诺。
- 不把 StackC 宣称为强保护；它是正确性和覆盖率基线。
- 不在 mjdbg 还没有报告前自称保护强度已经提升。
- VMC4 性能优化（v2.3+，可用版已交付但性能非目标）。
- LLVM 后端基础款已转正（v2.2 起默认开启，静态标量纯计算/控制流；intrinsic、对象/JNI/异常等 fail-closed 面的扩展 v2.3+ 视需求）。

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
xmake f --with_mj2x=y --with_tests=y
xmake build mj2x-core
xmake build mj2x-cli
xmake build mj2x_unittest
xmake run mj2x_unittest
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild -AllowMissingSamples
```

native 相关 PR 必须记录 OS、JDK、C 编译器、是否运行生成产物、是否产出 mjdbg
observation/weakness report。
