# 技术文档索引

本目录收录 mj2x 的技术文档，分两组：

- **编译技术** — 把 JVM 字节码编译成原生代码的通用技术与本仓实现
- **保护技术** — 消除可还原锚点、对抗逆向的单项技术（[总览](protection_overview.md)）

## 保护技术

按作用层分族。单项技术回答四个问题：叫什么、对抗什么、机制是什么、代价是什么。

### A. 数据流混淆（RegIR pass）

| 文档 | 技术 | 开关 | 默认 |
|---|---|---|---|
| [mba.md](mba.md) | MBA（混合布尔算术） | `--regc-obfuscate` / `--regc-mba-deep` | 关 |
| [opaque_predicate.md](opaque_predicate.md) | 不透明谓词 | `--regc-obfuscate` / `--regc-opaque-zero` | 关 |
| [constant_hiding.md](constant_hiding.md) | 立即数隐藏（常量加密） | `--regc-hide-constants` | 关（不联动） |

### B. 控制流混淆（RegIR pass）

| 文档 | 技术 | 开关 | 默认 |
|---|---|---|---|
| [control_flow_flattening.md](control_flow_flattening.md) | CFF 控制流平坦化 | `--regc-obfuscate` / `--regc-cff-*` | 关 |
| [computed_jumps.md](computed_jumps.md) | computed jumps | 拟 `--regc-computed-jumps` | 📋 未实现（#7） |

### C. 调用图混淆

| 文档 | 技术 | 开关 | 默认 |
|---|---|---|---|
| [call_indirection.md](call_indirection.md) | 调用目标间接化 | `--regc-indirect-calls` | 关（不联动） |

### D. 结构降级与边界消除

| 文档 | 技术 | 开关 | 默认 |
|---|---|---|---|
| [native_direct_call.md](native_direct_call.md) | native→native 直接 C 调用 | `--regc-direct-calls` | 关（#814） |
| [pod_struct.md](pod_struct.md) | POD 类降级为 C struct | 拟 `--regc-pod-struct` | 📋 未实现 |
| [vm_virtualization.md](vm_virtualization.md) | VM 虚拟化（VMC4） | `--backend vmc4` + `--vmc4-*` | 关 |

### E. 运行时与产物保护

| 文档 | 技术 | 开关 | 默认 |
|---|---|---|---|
| [jni_table_protection.md](jni_table_protection.md) | safe-env（JNI 函数表保护） | `--regc-safe-env` | 关 |
| [runtime_anti_debug.md](runtime_anti_debug.md) | 运行时反侦测检查点 | `--runtime-protection` | `none` |
| [string_encryption.md](string_encryption.md) | 字符串 at-rest 加密 | `--strings-encrypt` | 关 |
| [native_lib_encryption.md](native_lib_encryption.md) | native 库 at-rest 加密 | `--native-lib-key` | 关 |

### 未成篇但已存在的保护面

| 技术 | 开关 | 说明 |
|---|---|---|
| RegisterNatives 动态绑定 | 总是开 | 函数名/可见性与 JNI 符号查找完全脱钩，绑定靠 finalize 发射的注册表 |
| 方法内联 | `--regc-no-inline` 反向控制 | SSA 内联默认开，消除 native 方法间 JNI 往返 |
| 用户 intrinsic | `--intrinsic-config` | 任意 Java 静态调用点降级为用户 native symbol 直调，见 [user_intrinsic.md](../user_intrinsic.md) |

## 编译技术

| 文档 | 主题 |
|---|---|
| [ssa_construction.md](ssa_construction.md) | SSA 构造：CFG → SSA IR、phi placement、支配树、重命名 |
| [bytecode_translation.md](bytecode_translation.md) | 字节码到 IR：操作数栈消除、异常表、switch、indy 桥接 |
| [codegen_strategies.md](codegen_strategies.md) | 原生代码生成：栈机 vs 寄存器、lowering、C codegen |
| [jni_bridge.md](jni_bridge.md) | JNI 桥接层：引用管理、字段/方法 ID、字符串、异常 |
| [intrinsic_mapping.md](intrinsic_mapping.md) | 编译器内联函数：JDK 方法识别、语义等价、原生替换 |
| [runtime_protection.md](runtime_protection.md) | 原生运行时保护：反调试、反 JVMTI、加密原语 |
