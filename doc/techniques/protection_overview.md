# 保护技术总览

`doc/techniques/` 下与保护相关的条目按**作用层**分四族。本文是索引与组合纪律，
单项技术见各分篇。

## 一、威胁模型

mj2x 的对手拿到的是受保护后的 JAR + native 库。他能做的四类事情，决定了每项
技术到底在挡什么：

| 攻击面 | 攻击者能拿到什么 | 挡它的技术 |
|---|---|---|
| 静态反编译字节码 | 剩余字节码的算法结构 | 字节码侧的变换（本仓主线不在这：mj2x 是把语义迁走） |
| 静态读 native 产物 | C 源码 / `.ll` / 反汇编里的控制流与常量 | MBA、opaque 谓词、CFF、立即数隐藏、VM 虚拟化、字符串加密 |
| 静态建调用图 | "这个方法调用了谁" | 调用目标间接化、computed jumps |
| 动态观察 | 调试器、JVMTI agent、JNI 表 hook、内存 dump | 运行时保护检查点、safe-env、native 库加密、懒解密 |

**关键前提**：mj2x 的主线不是"把 Java 全量 AOT"，而是把选定方法/类的语义迁到
native，减少字节码和 JNI 可还原锚点。所以"保护强度"的第一来源是**迁移本身**，
变换是叠加在上面的第二层。没有迁移、只在字节码上做变换，不是 mj2x 的路线。

## 二、技术族与一览

### A. 数据流混淆（RegIR pass）

| 技术 | 分篇 | 开关 | 默认 |
|---|---|---|---|
| MBA（混合布尔算术） | [mba.md](mba.md) | `--regc-obfuscate` / `--regc-mba-deep` | 关 |
| 不透明谓词 | [opaque_predicate.md](opaque_predicate.md) | `--regc-obfuscate` / `--regc-opaque-zero` | 关 |
| 立即数隐藏 | [constant_hiding.md](constant_hiding.md) | `--regc-hide-constants` | 关（不联动） |

### B. 控制流混淆（RegIR pass）

| 技术 | 分篇 | 开关 | 默认 |
|---|---|---|---|
| CFF 控制流平坦化 | [control_flow_flattening.md](control_flow_flattening.md) | `--regc-obfuscate` / `--regc-cff-*` | 关 |
| computed jumps | [computed_jumps.md](computed_jumps.md) | —（issue #7） | 未实现 |

### C. 调用图混淆（RegIR pass → 发射端 → 运行时）

| 技术 | 分篇 | 开关 | 默认 |
|---|---|---|---|
| 调用目标间接化 | [call_indirection.md](call_indirection.md) | `--regc-indirect-calls` | 关（不联动） |

### D. 结构降级与边界消除

| 技术 | 分篇 | 开关 | 默认 |
|---|---|---|---|
| native→native 直调 | [native_direct_call.md](native_direct_call.md) | `--regc-direct-calls` | 关（#814） |
| POD 类降级为 C struct | [pod_struct.md](pod_struct.md) | —（待开 issue） | 未实现 |
| VM 虚拟化（VMC4） | [vm_virtualization.md](vm_virtualization.md) | `--backend vmc4` + `--vmc4-*` | 关 |

### E. 运行时与产物保护

| 技术 | 分篇 | 开关 | 默认 |
|---|---|---|---|
| safe-env（JNI 表保护） | [jni_table_protection.md](jni_table_protection.md) | `--regc-safe-env` | 关 |
| 运行时反侦测检查点 | [runtime_anti_debug.md](runtime_anti_debug.md) | `--runtime-protection` | `none` |
| 字符串 at-rest 加密 | [string_encryption.md](string_encryption.md) | `--strings-encrypt` | 关 |
| native 库 at-rest 加密 | [native_lib_encryption.md](native_lib_encryption.md) | `--native-lib-key` | 关 |

## 三、开关联动规则

三个层次，不要凭名字猜：

1. **基础三项**（MBA / opaque / CFF）由 `--regc-obfuscate` 一次性开启。
2. **增强项**各自联动对应基础项：`--regc-mba-deep` → MBA，`--regc-opaque-zero`
   → opaque，`--regc-cff-dead` / `--regc-cff-state-encrypt` → CFF。
3. **重型项不联动**：`--regc-hide-constants` 与 `--regc-indirect-calls` 必须显式
   给。理由是代价——前者每个整型立即数多一次运行时调用，后者每次调用多一层
   分派，都是"要保护才付"的档位。

## 四、组合纪律（顺序敏感）

RegIR pass 的执行顺序是硬约束，改动前先看这里：

```text
MBA → opaque 谓词 → CFF → 调用间接化 → 立即数隐藏
```

两条已踩过的坑：

- **立即数隐藏必须排 CFF 之后**。`isCffPureInstruction` 对 `Intrinsic` 返回
  false，若先插入合成 Intrinsic 会把 CFF 的纯性判定整体打掉，平坦化直接失效。
  反过来 CFF 的 case key 保持字面量（分发表完整性），不会被隐藏。
- **调用间接化排 CFF 之后、立即数隐藏之前**。它只置标记、不改指令序列，与
  前后两者零交互。

CFF 之前的 pass 生成的字面量（opaque 谓词的 `1`、state 码）会随立即数隐藏
一并被隐藏，语义不变。

## 五、后端覆盖面

不是每项技术在四个后端都有：

| 技术 | StackC | RegC | LLVM | VMC4 |
|---|---|---|---|---|
| MBA / opaque / CFF | ❌ | ✅ | ✅（超子集则整方法回退未变换） | ❌（显式不走 transform passes） |
| 立即数隐藏 | ❌ | ✅ | ✅ | ❌ |
| 调用目标间接化 | ❌ | ✅ | ✅ | ❌ |
| native→native 直调 | ❌ | ✅（`--regc-direct-calls`） | ✅（仅 INVOKESTATIC，**无门控**） | ❌ |
| 运行时保护 | ❌ | ✅ | ✅ | ✅ |
| VM 虚拟化 | — | — | — | ✅ |

VMC4 不走 RegIR transform passes 是显式决定：VM 层已有自己的混淆面（opcode
随机化 / drift / 块乱序），再叠 RegIR 变换是重复投入。

## 六、代价与取舍的通用原则

每项技术分篇里都有"代价"一节，通用取舍原则四条：

1. **默认全关**。RegC 定位高性能 native 生成器，默认产物语义直译零混淆。
2. **语义零变化**是硬底线。任何变换都要有 exec 级 E2E 比对 JVM 基线。
3. **可复现**。同 seed + 同输入 → 同产物（除不掺 seed 的项，如立即数隐藏的
   site，理由见分篇）。
4. **不宣称强度**。没有 mjdbg 红队报告前，只说"消除了哪个可还原锚点"，不说
   "提升了保护强度"。
