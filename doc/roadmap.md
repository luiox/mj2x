# mj2x 路线图

> mj2x 是 morpher 的 bytecode -> native 保护工具线。它的主线不是“把 Java 全量
> AOT”，而是把选定方法/类的语义迁到 native，减少字节码和 JNI 可还原锚点。

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

StackC 和 RegC 并行存在：

- StackC：广覆盖、正确性优先，适合作为“能跑”的通用后端和回归基线。
- RegC：保护导向，目标是消除 stack-style anchor，减少 c2j 可还原面。

---

## 2. 能力路线图

| 能力 | 目标 | 当前方向 |
|---|---|---|
| 输入/输出 | `.class` / `.jar` 输入，改写 class，输出 C/native project | 保持 class/JAR 顺序、资源、manifest 语义稳定 |
| 方法选择 | 全量标记、注解筛选、未来配置筛选 | 用户注解和 CLI 筛选并行 |
| L1 MIR | 从 bytecode 提升到栈式中间表示 | 支撑 StackC 和后续 SSA/RegIR |
| StackC 后端 | 广覆盖 JNI C 后端，正确性优先 | 覆盖 javac 常见形状、异常、monitor、invokedynamic 桥 |
| SSA / RegIR | 消除 stack slot，建立保护导向 IR | 服务 RegC、DCE、MBA、CFF 和整类下沉 |
| RegC 后端 | 寄存器式 C/C++ 输出，无 `cstack` / `stack[]` / `sp` anchor | 保护主线，unsupported 必须 fail closed |
| Codegen 工具层 | CSourceStream、IncludeSet、mangle、literal helper | 减少裸字符串拼接和后端重复 helper |
| Runtime ABI | div/rem/barrier、helper、异常/错误桥接 | 保持 ABI 可测试、可版本化 |
| Runtime core | crypto、kernel、stringpool、antidebug、antijvmti | 从基础探测走向真实反调试/反 agent/反 dump |
| JDK intrinsic | 包装类、Math/Random、String/StringBuilder、集合、工具类 | 从非逃逸小切片扩大，逃逸必须 fallback/fail closed |
| String/UTF-16 | Java String 语义、StringBuilder/StringBuffer | 依赖 libca UTF-16 或 mj2x 最小垫片 |
| L4 变换 pass | DCE、常量传播、MBA、CFF、opaque predicate | 只有 RegC/R3 live 验证稳定后再扩 |
| 整类下沉 | 算法密集类内部方法全 native，边界保留 JNI | 先做无反射、无动态类加载、intrinsic 覆盖充分的类 |
| Protected samples | StackC/RegC 样本、expected trace surface、hardening history | 必须给 mjdbg 攻击 |
| Release packaging | CLI、runtime、注解包、样本和验证脚本 | v2.1 需要把 mj2x 纳入发布矩阵 |
| VMC4 | 高价值方法可选 VM 虚拟化支线 | 暂不作为主线，按 mjdbg 反馈决定 |

---

## 3. v2.1 优先级

1. mj2x 纳入 release artifact 和 release validation，而不是只停留在源码/单测层。
2. protected sample corpus：每个样本都写 expected trace surface，供 mjdbg 攻击。
3. RegC/R3 live 验证：现有 intrinsic、runtime ABI、fallback/unsupported reason 稳定。
4. runtime 反侦测体系：从参数扫描/简单 debugger 检测，扩展到 agent/JVMTI/native debugger 场景。
5. String/StringBuilder intrinsic 先做设计和最小垫片；是否进入 v2.1 实现取决于 UTF-16 基础和样本需求。
6. L4 变换 pass 不急着堆，先保证输出能跑、能被 mjdbg 攻击、能量化变强。

---

## 4. 不做的事

- 不做全 AOT；反射、动态代理、类加载、Instrumentation 等场景默认不承诺。
- 不把 StackC 宣称为强保护；它是正确性和覆盖率基线。
- 不在 mjdbg 还没有报告前自称保护强度已经提升。
- 不把 VMC4 提前拉回主线，除非 RegC/runtime 闭环证明需要。

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
xmake build mj2x_runtime_test
xmake run mj2x_unittest
xmake run mj2x_runtime_test
tools/product_sample_smoke.ps1 -Mode fast -SkipBuild -AllowMissingSamples
```

native 相关 PR 必须记录 OS、JDK、C 编译器、是否运行生成产物、是否产出 mjdbg
observation/weakness report。
