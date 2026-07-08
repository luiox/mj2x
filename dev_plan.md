# mj2x — dev_plan

> 本文档**只放未完成项**。已完成功能见 [doc_ai/capabilities.md](doc_ai/capabilities.md);路线图见 [doc/roadmap.md](doc/roadmap.md)。
>
> 治理规则(`doc_ai/documentation_governance.md` §7):任务完成 → 从本文件删除 → 加到 CAPABILITIES。本文件不允许 `[x]` 长期存在。

---

## 当前阻塞

无。

## 待办

- [ ] **CBackend native-obfuscator gap 对齐(本分支验收清单)**:以根目录
  `cbackend_vs_native_obfuscator_gap_report.md` 为任务源,按功能块拆分提交。
  - [x] P0/P2/P4(旧报告):导出符号消除、`JNI_OnLoad` + `RegisterNatives`、int/long
    div/rem `ArithmeticException` trap。提交:`2b2e84a2 [mj2x] align cbackend native registration`。
  - [ ] P6:boolean[]/byte[] `BALOAD`/`BASTORE` 运行时歧义消除。
  - [ ] P2:ClassLoader-aware 类解析,减少自定义 class loader 下 `FindClass` 失败。
  - [ ] P3:JNI 局部引用追踪与批量释放,降低复杂方法 local-ref table 溢出风险。
  - [ ] P4:method/field ID 懒缓存,降低重复 `GetMethodID`/`GetFieldID` 开销。
  - [ ] P5:String pool,减少重复 `NewStringUTF` 并收敛字符串常量表面。
  - [ ] P1:`<clinit>` 分散注册入口,从集中 `JNI_OnLoad` 迁到 per-class 触发。
  - [ ] P1:hidden `<clinit>` proxy / HiddenMethodsPool,把类初始化本体移出 Java 层。
  - [ ] P7:JAR 内 native loader,减少用户手动 `System.loadLibrary` 配置。
  - [ ] P8:CMake / 独立工程构建输出,对齐 native-obfuscator 可移植构建体验。
  - [ ] P0:invokedynamic lowering 策略,支持 lambda/string concat/method handle 的可执行路径。
- [ ] **R4:L4 变换 pass 系统**(MBA / DCE / CFF / opaque predicate)。前置:RegC/R3 live 验证。
- [ ] **R5:mjdbg 闭环验证**。用 mjdbg / c2j 红队验证 RegC 保护强度,产出量化结论。前置:R3 RegC ABI live 验证 + mjdbg 样本反馈。
- [ ] **RegC/R3 live 验证(剩余)**:intrinsic(Math/parseInt)已 live 验证(E2E 通过);剩余把 inline RegC helper 逐步迁到 R3 runtime ABI、补 SSA/RegC 常见 javac 输入形状。
- [ ] **JDK intrinsic 覆盖扩展**:把已交付的 runtime intrinsic(包装类 valueOf/intValue、Random nextInt/nextLong/...)接入 RegC emitter。当前阻塞点:包装类对象与 Random 对象在 native 侧需有逃逸表示(mj2x_int struct ↔ mj2x_ref jobject 边界转换),Math/parseInt 因无对象逃逸已通。
- [ ] **运行时反侦测体系(issue #411)**:四类威胁——反 jvmtp/IDE 调试、反 java agent(arthas 类,不能光靠启动参数)、反 jvmti dump agent(mjt-dump 类)、反 native 调试器(x64dbg 类)。现有 antidebug/antijvmti 是初版切片(IsDebuggerPresent + 参数扫描 + JNI table 篡改),需纵向加深到运行时 transformer 枚举、JVMTI 能力查询、PEB/NtQuery/断点扫描。演进设计见 `doc/translation_evolution.md` §运行时反侦测体系。
- [ ] **xmake 项目生成器重构**(`feat/mj2x/xmake-project-generator` 分支):当前 emit 后字符串改写,迁到 codegen 阶段直出。前置:建立 golden test 固化当前输出。

## 下一步验证命令

```bash
xmake build mj2x-core mj2x-cli mj2x_unittest mj2x_runtime_test
xmake run mj2x_unittest
xmake run mj2x_unittest --gtest_filter=CliTest.*
xmake run mj2x_runtime_test
```

`CliTest.*` 覆盖 CLI 注解筛选、`--compile` 错误提示和可用工具链 smoke；本机缺少 `JAVA_HOME` 或没有可用 C 编译器时，native library 成功编译 smoke 会按环境跳过。
