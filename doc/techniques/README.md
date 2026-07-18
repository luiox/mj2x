# JVM → 原生编译技术索引

将 JVM 字节码编译为原生代码的技术文档。

| 文档 | 主题 |
|---|---|
| [ssa_construction.md](ssa_construction.md) | SSA 构造：CFG → SSA IR、phi placement、支配树、重命名 |
| [bytecode_translation.md](bytecode_translation.md) | 字节码到 IR：操作数栈消除、异常表、switch、indy 桥接 |
| [codegen_strategies.md](codegen_strategies.md) | 原生代码生成：栈机 vs 寄存器、lowering、C codegen |
| [jni_bridge.md](jni_bridge.md) | JNI 桥接层：引用管理、字段/方法 ID、字符串、异常 |
| [intrinsic_mapping.md](intrinsic_mapping.md) | 编译器内联函数：JDK 方法识别、语义等价、原生替换 |
| [runtime_protection.md](runtime_protection.md) | 原生运行时保护：反调试、反 JVMTI、加密原语 |
