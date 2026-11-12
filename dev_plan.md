# dev_plan：mj2x 施工计划

> 本文件只保留**待做项**。已完成的工作不写日志,git log 即记录;重要的机制与契约写进设计文档(`doc/techniques/`)。

## 待做项

### A. VMC4 升级档（P2）

- period 语义变义（case 标签周期轮换）已实现（#844）,mjdbg 侧的检测配套已就绪（五形态识别、period 变义 label 表提取、case 常量反查、computed_goto 产物修复,见 mjdbg 仓 vmc4 攻击测试契约 §6）。剩余:在 `feat/mj2x-vmc4-mutation` 分支做升级档的闭环验证,并扩展攻击测试契约（含变义强度模型）。

### B. mj2x 自建测试集（#820）

- 步骤 6 待做:mjdbg 闭环验证（每用例期望 trace 输出 + weakness report,与 mjdbg 工具端联动）。
- libjbc 迁移自带的回归检查长期保留:round-trip 字节保真对照检查、maxs/SMT 与 javac 对照检查,后续测试清理不得顺手删除（原 plan_libjbc_migration 归档已删,来历见 git log）。

### C. 记录不阻塞（P3）

- VMC4 检查点指令级异常匹配（javac 字节码下块粒度==指令粒度,无实际缺口）、 agent 特征深度检测（javaagent MANIFEST 扫描 / JDWP 探测）、异常表多 region 匹配顺序测试扩展、runtime C++ 版与 crt C 版检测面统一。
- JVMRTDP 调研（issue #847）:纯 Java 面 JVMTI 工具,对 mj2x 保护产物（native 化/VMC4）**无实质威胁**。观察 a:agent 用 manual mapping 加载可绕过已加载模块枚举的检测（Windows 侧;capability 快照 + 启动参数扫描仍覆盖,记录不实施）;观察 b:RegC 无字符串加密变换（对标 jvmtp `obfuscation.strings`）,敏感字符串场景需加密模块（P3）。
- 枚举合成方法（values/valueOf）站点 intrinsic 登记只发生在枚举类自身转换时点:调用方类先于枚举类编译时登记未发生,调用点退通用 JNI 路径（fail-closed,仅损优化不损正确性;根治需编译序无关的登记时机）。

### D. 配置系统 GUI 工具（P2）

- 配置系统的前两阶段已完成（见归档索引）。设计已完成（GUI 配置工具设计:vite+vue 方案、schema 驱动渲染、联动/校验/导出/可选 sidecar 执行）。剩余:实现表单渲染、校验与导出。

### F. 整类下沉（P3,记录不阻塞）

- roadmap §2 表格行已列（算法密集类整体 native 化,边界保留 JNI）;GUI 设计已预留方法/类选择扩展点。条件:无反射、无动态类加载、intrinsic 覆盖充分;先做单类试点再铺开。

### H. indy 原生派发 backend 侧合流（RegC 面已完成）

- 管线层判定原型 + 设计文档已完成（indy 原生派发设计: §2 判定树 / §3 缓存 / §4 回退 / §6 接口提案,下列 P/F 编号见该提案）。
- P1（判定载荷单源化）、P2（字段/record 直派）、P3（TU 级直派缓存 + 惰性解析）已完成;P4（运行期破解）保持提案,随合流工作评估。
- 剩余:P5 stackc 分支、P7 vmc4 编码前改写式、P10 llvm 模块级缓存分派,三后端消费同一 MIR 载荷,发射面各自适配。
- 注:P6（stackc bootstrap 结果缓存）经核已有等价物（stackc_mir_invoke.cpp indyTargetCache,static jobject per call-site）,**无需实施**。
- 注:F1 直派 engagement 当前仅覆盖 indy 描述符即字段形态的站点（javac 产物不会出现,现实站点走 helper 零回归）,收益兑现需 D1 静态求值或 bootstrap 白名单扩展（发射面无需再动）,与 P4 一起评估。
- 注:Random 预检 registry 标志位化已单独完成,不再含入合流范围。

### H-2. 三后端共享层抽取:排期结论

**结论:不在与发射面改动同窗口实施;indy 解析载荷单源化（P1）已先行完成。**

- 依据:四个子项均需设计决策 + 全量回归,且抽取面与进行中的发射面改动高度重叠,同一窗口内叠加结构性抽取的语义风险最大;其余子项待合流完成后启动。
- 触发条件（重排期）:indy P5-P7/P10 三后端消费合流完成后,按映射表 → RegisterNatives → indy 解析收尾 → 多维数组 的子项顺序独立排期,每子项独立提交 + 全量回归。

### G. 全线审查遗留（待评估/待实施）

- rt_protect Linux rwx 检测曾疑似对宿主 JVM CodeCache（rwxp）误报（force 下自毁）。已在 WSL2 Ubuntu 24.04 + OpenJDK 17.0.20 实测排除:静息态与 JIT 满载态 `/proc/PID/maps` 均无 rwx 段,现代 HotSpot 不出现该形态,`mj2x_rt_maps_has_rwx` 在目标环境不会误触发。检测面保持现状;若未来遇受影响环境（旧 JDK/其他 JVM/异常内核）,再按 safe-env 模式细化 libjvm 自身映射排除。
- RegC 进程级缓存并发残留:jclass 与 methodID 两写间的短暂错配窗（需原子对或 seqlock 才能闭）、字符串字面量 32 槽缓存的瞬时覆盖竞态（越界安全,维持既有注记）;并发契约见 `mj2x_regc_runtime.c` 文件头。
- ~~CLI 外部进程调用（编译器/xmake/pack hook）无超时~~ **已完成（2026-10-15）**: `--native-timeout`（默认 900s,0=不限时）覆盖全部外呼点;Windows 上 `taskkill /T /F` 先终止整棵进程树再回收,避免孙进程持有管道写端导致的死锁。libca 0.0.15 已有超时基元,策略与文案单源在 `mj2x-cli/src/native_process_timeout.hpp`。
- 三后端共享层抽取（结构性最大项）:JVM 类型格点映射表（10+ 张平行）、 RegisterNatives 基建、indy bootstrap 运行时解析（3 份）、多维数组分配（3 份）;按 H-2 触发条件独立排期。
- ~~backend.hpp 的 Kind::BINARY 死枚举、llvmir `.ll` 挂 C_SOURCE 语义错位~~ **已完成（2026-10-24）**:`BackendResult::Kind` 删 BINARY 死枚举与无人消费的 `data` 字段,新增 `LLVM_IR`,backend.hpp 契约文档同步。剩余一项仍待办:finalize 无错误通道（三后端失败行为不一致）。~~接口默认方法能力边界~~ **已修（2026-10-26）**:JLS 9.4/JVMS 4.6 禁止接口方法带 ACC_NATIVE,MarkAll / annotation / prefix 三条标记路径统一整体跳过接口类（default/static/私有接口方法保持字节码）,InitClinitMover 同步跳过;契约测试 InterfaceNativeSkipTest 四用例。
- RegC intrinsic 判定热点:每条 Call 重复 match 6-10 次（含多次 std::string 构造与全表线性扫）,应函数级一次算好 decision;registry 视图不统一（analyze 走 builtins、emit 走用户 registry,覆盖判定有漂移风险）。
- 性能遗留:SSA 构建 O(n²)（fallthroughBlock 线性扫 / findBlockByLabel / reaching_def 无邻接表）;JAR 输出每条目全量拷贝;llvmir GlobalStringTable::intern 线性查。
- CLI 无下界整数参数负值回绕已修复（2026-10-06:Int/UInt64/Hex 及手写 vmc4-seed、native-lib-key 校验器恒拒负值并给合法范围提示,schema minimum 同源;豁免清单为空,现存整数参数均无负值语义）。
- 死代码清理:cli dispatcher 别名死分支核查（2026-10-06:唯一别名面 `--backend cbackend/c` 有 cli_test 用例 + doc/user-guide.rst + 双语翻译三处仍在使用,属文档化的旧脚本兼容契约,非死代码,跳过;MONITOR_EXIT 死枚举已删）。
- 待办（libs 层,跨仓）:`jbytecode::core::Type::getArgumentTypes("")` 对空描述符做 `byte_at(1)` 越界读,异常类型随越界字节内容漂移;mj2x 侧 indy_bootstrap_plan 已加空串守卫确定性抛 invalid_argument,libs 层其余调用方的暴露面待共享层统一修复。
- 约定基调:std::string vs Utf8String、int vs i32 在 CLI/codegen/后端三层各自为政,需在 AGENTS 层面定一次豁免边界。

### J. 待办池（后续开工从这里取）

**工程待办（小项,均可独立开工）**

1. llc / clang++ 的 SKIP 判定面缩小（2026-10-06 调研:cl/gcc/zig 非 clang 家族,不属 clangPathPrelude 注入面,原统一注入设想不成立）。仅 LLVM 标准安装未上 PATH / Homebrew llvm 未链接的环境受益;当前已建档环境增益为零,且需动 #1191 检查的双键基线,待此类环境入档再开工。
2. isGeneratedRuntimeClass 对用户手写 `Lambda$N` 的理论误判:名字识别法对用户自定义同名类存在理论冲突,低概率,记录在案。
3. ~~EnumSemCheck E2E 检查力~~ 已核实（2026-10-29）:#1193 内联缓存与类序解耦后,三处 Color.valueOf 调用点照常内联进 EnumSem.check;变异测试（临时禁用 ssa_inline_passes 保护继承登记）实证 regc 侧 E2E 能拦住该路径（挂起异常越过 try 边界时 regc 红、stackc/vmc4 不受影响、回滚复绿）。caller-first 排序变体不再需要。
4. #1194:模板 glob 并入 `src/generated/` 下任意 .c/.cpp,该目录为 CLI 专有,无现风险;若未来开放手工放置需加来源约束。
5. IndyBootstrapPlanTest.WalkEdgeShapes 非确定性失败（2026-10-06 评审发现）:`Type::getArgumentTypes("")` 对空描述符自 `byte_at(1)` 起越界读零长串,结果随栈残留字节浮动,同一二进制逐次运行可红可绿（实测 5 连跑 2 红）,全量检查对二进制布局变化引入噪声。消除判据:libs/libjbytecode 的 type.cpp `getArgumentCount`/`getArgumentTypes` 入口加 `length < 2` 边界检查显式抛 invalid_argument,测试改用合法最小描述符;动共享层需跨仓回归。
6. DEF_HEX 零注册死宏退役:param_registry 的 DEF_HEX 宏在 main 上无任何注册点,负值校验对其补丁属给死代码上锁,与清理主题相悖,删宏或注明口径;对应地,负值校验的声明口径为 DEF_INT/DEF_U64 两类宏 + vmc4-seed/native-lib-key 两支手写校验器。

**test_resource/java_corpus 语料集修复记录（2026-10-24 至 2026-10-29,全部完成,过程见 git log）**

7. lambda bootstrap 反查自身的 NoClassDefFoundError（合成类落点改为按外围类真实 access 位判定,exclusions 清零）。
8. regc 引用值丢 null 及 regfile 系列缺陷:ssa_builder 异常边入口快照、块参数活性区间、parallel-move 交换环破碎（emitParallelCopies 贪心排序 + scratch 槽）、异常区域拷贝槽位共享,全语料复绿。
9. regc/vmc4/llvm 三后端 LDC 字符串的 modified UTF-8 转换（共享 `codegen::utf8ToModifiedUtf8`,NUL 与增补平面代理对）。
10. stackc `instanceof` 对 null 返回 true;vmc4 `i2s/i2b/i2c` 窄化退化恒等（VmType 增 I16/I8/CHAR Convert 标签）。
11. stackc 异常分派 handler 顺序反排（按异常表序有序插入;ARRAY_LOAD/STORE 无保护区域也立即 ExceptionCheck 返回）。
12. computed jumps 大 switch 缺陷（key→目标映射表扫表循环,与 parallel-move 环破碎同提交修复,obf 档复接 `--regc-computed-jumps`,BigSwitch 形态已入 obf 档例行覆盖）。
13. regc INVOKESPECIAL 虚分派无限递归（owner 声明解析 + CallNonvirtual\*MethodA + JNI 参数序 obj 在前）。
14. regfile 与 monitor 密集代码的病态慢随上述修复消失;遗留的 helper ExceptionCheck 加固与最小复现单测降级为健壮性备注,遇回归再启用。

**产品/机器决策项（需决策）**

15. benchmark 驱动器默认输入路径:`-InputJar` 可覆盖,但默认是 Downloads 目录,改仓库样本路径还是提升为配置项,待定。
16. CI runner 安装 llvm/clang（#1122 方向 1）:unittest skip 下限 4→2（仅剩 zig 系）,integration LLVM 列 28 例在 CI 转实跑。
17. RegC CRT DLL 跨运行缓存（~12-20ms/次）:需解密 DLL 落盘持久化,与不落明文 DLL 的威胁模型冲突,接受现状或重评威胁模型,待定。
18. POSIX 600 文件权限:spool/临时文件 `Files.createFile` + posix attrs 收紧（Windows 无影响,POSIX 加固项）。

**编排层**

19. 锁协议 v2 记录:mkdir 抢锁 + owner.txt + 验 own 后释放、tests/gates 不持锁、`xmake build <target>` 单 target 单命令,写入归档页,供后续 sub agent 会话直接引用。
20. 测试分层策略文档重写:计划期表格（实施顺序列）大多已实现,删表留分层策略与覆盖率审计方法学。
21. 代码注释与 Doxygen 组名里的评审代号退役（如 `regc_r3_abi`）,文档侧已完成。

### K. jvmtp 对标差距（2026-10-14 调研）

> 对标对象 jvmtp **1.7.3**。结论:mj2x 在变换能力上不落后,在交付形态与抗分析面上明显落后。建议先做下列小改动项,再开发新功能。

**小改动项（无架构风险）**

1. ~~CFF 抗还原~~ **已完成（2026-10-15）**:state 由 `RegFunction::cffVolatileState` 发布、RegC 声明为 `volatile int32_t`（`RegTransformOptions::cffStateVolatile` 默认 true）。clang `-O2` 下 dispatcher 不再被完全还原,`switch i32` 存活,`-Wswitch` 溢出警告归零。测试:`RegCTest.CffStateRegisterIsEmittedVolatile` / `CffStateVolatileCanBeDisabled`。
   - 遗留:① LLVM 后端纯 SSA 无内存槽,volatile 方案不适用,`--backend llvm` + CFF 组合仍会被优化器还原,需 inline-asm 屏障（`asm volatile("" : "+r"(state))`）或模块级 volatile 槽;② clang -O2 后 dispatcher 存活尚未做成自动化 CI 断言,目前靠人工跑 `work/cff_probe/` 的 standalone 编译对照。
2. ~~生成工程优化级别~~ **已完成（2026-10-15）**:新增 `--native-opt-level <0|1|2|3|s>`（默认 `2`）,xmake 工程映射 `set_optimize` （none/faster/fastest/aggressive/smallest）,CMake 与 LLVM 侧分别注入 `/O*` 与 `-O*`。LTO 未做（对标 `compiler.lto`）,留待需要时开工。
3. ~~类级 glob include/exclude~~ **已完成（2026-10-15）**:新增 `--native-include` / `--native-exclude`（可重复 + 逗号分隔;`*` 不跨段、`**` 跨段、 `?` 单字符,`/` 与 `.` 等价,exclude 优先）,JSON 键 `native_include` / `native_exclude`。实现为 `NativeClassFilterPreprocessor`（置 `CompilerClass::filteredOut`）,方法级选择（annotation / prefix / mark-all）与 `<init>`/`<clinit>` 搬运入口按该标志跳过;被剔除类保留在输出 JAR 中不改写。空 glob 片段 fail-closed（`mj2x.cli.err.class_glob_empty`）。测试: `NativeClassFilterGlobTest.*` / `NativeClassFilterPreprocessorTest.*` + `CliTest.NativeClassFilter*` 四条 E2E。
4. ~~外部进程超时~~ **已完成（2026-10-15）**:新增 `--native-timeout <秒>` （默认 `900`,`0`=不限时;JSON 键 `native_timeout`;对标 jvmtp `compiler.timeout`）。每次外呼独立计时,覆盖编译器/link/vcvars 探针/xmake 两阶段/pack hook 共 10 余个站点。Windows 上 kill 单进程只终止直接子进程,孙进程继承管道写端使回收的 EOF 等待永不到达,`waitChildWithTimeout` 先 `taskkill /T /F` 终止整树再回收（Linux 依赖 libca 进程组 kill）。测试:`NativeProcessTimeoutTest` 7 例 + `CliTest.NativeTimeout*` 4 条 E2E。
5. ~~mba-deep 与 CFF 互斥~~ 与 ~~state 码溢出 int32~~:已修（2026-10-14）,分别由 `RegTransformPassTest.MbaDeepBarriersDoNotSuppressFlattening` / `CffStateCodesStayWithinPositiveInt32` 测试覆盖。

**中等改动项（补缺失的保护面）**

6. 原生混淆三件套:
   - ~~立即数隐藏（`obfuscation.constants` / `.constants.mba`）~~ **已完成（2026-10-16）**:新增 `--regc-hide-constants`（JSON 键 `regc_hide_constants`,默认关闭）。RegIR pass `applyConstantHiding` 把每个整型 `IntLiteral` 操作数 C 改写为 `E ^ Intrinsic("const.key:<site>")`:E = C ^ K 编译期物化,K 运行时由可分发运行时（regc CRT / LLVM runtime 的 `const_key`）按同一 site 派生,I32 追加 `Convert` 截断。两端 splitmix64 公式同源（`ca::random::SplitMix64`,#1027 逐位锁定）;不透明常量由 `static volatile` 承载以防 LTO 下 IPSCCP 折回原值。site 为函数内局部计数器、不掺 seed（site 必物化在产物里,掺 seed 只增大位宽不增加分析成本）。**不被 `--regc-obfuscate` 联动**:每立即数一次运行时调用,是家族里最重的一项。测试:`RegTransformPassTest.ConstantHiding*` 5 例（语义等价求值 / I64 不截断 / 浮点跳过 / seed 无关 / 与 CFF 叠加后 dispatcher key 仍字面量）+ `mj2x_rt_test.test_const_key_derivation` + LLVM E2E 变体。
   - ~~调用目标间接化 + per-signature dispatcher（`obfuscation.calls.*`）~~ **已完成（2026-10-18,PR #15;发射端接线同步完成）**:RegC 发射端通用兜底换发 `_mj2x_regc_dispatch_<后缀>`、LLVM 发射端 `call.dispatch:` 分支 + 模块级 declare、validation 白名单放行、CLI `--regc-indirect-calls`（默认关、不被 `--regc-obfuscate` 联动）。分篇见 [doc/techniques/call-indirection.rst](doc/techniques/call-indirection.rst)。可分发运行时双端（regc CRT / LLVM runtime）各加六返回形态 per-signature dispatcher,按（站点表,索引）解析 owner 类 + jmethodID 并缓存入条目,解析失败返零值并保留 pending exception;`RegInstruction::indirectCall` 标记 + `RegFunction::callSites`（按 desc 分组、组内独立索引）+ pass `applyCallIndirections`（排 CFF 之后、立即数隐藏之前;自递归静态调用跳过）。**关键设计**:pass 只打标记、不由 pass 改写指令,已转换目标必须保留发射端的直接 C 调用快路径（0 JNI）,间接化它只会退化成 JNI 调用、纯开销零收益;是否外部由发射端既有解析结果决定,pass 无需跨函数全局信息,保住可复现构建不变量。
   - ~~computed jumps（`obfuscation.jumps`）~~ **已完成（2026-10-23,PR #27）**: `--regc-computed-jumps`（默认关、不联动）。把 CFF dispatcher 的 key → 目标块映射从代码挪到模块级数据表（key 按站点密钥异或,splitmix64 派生）,分派走解析出的块索引。MSVC 不能用 computed goto,表驱动 + 索引 switch 是标准 C 上限。实测合计 +8.9%、switch 密集处 +27%。分篇见 [doc/techniques/computed-jumps.rst](doc/techniques/computed-jumps.rst)。
7. ~~混淆两级作用域 include/exclude~~ **已完成（2026-10-23,PR #28）**: `--obf-scope-include/exclude` 类级 glob,判定落到 `CompilerClass::inObfuscationScope`（pass 与 backend 只问类在不在作用域内）。两者都为空 = 零行为变化;显式给 include 隐含开启混淆。详见 [doc/techniques/protection-overview.rst](doc/techniques/protection-overview.rst)。
8. ~~VMC4:super-ops + 指令碎片化~~ **已完成（2026-10-23,PR #29）**: `--vmc4-super-ops` / `--vmc4-fragment`（均默认关、可组合）。分篇见 [doc/techniques/vm-super-ops.rst](doc/techniques/vm-super-ops.rst)。
9. ~~VMC4 寄存器文件定长化~~ **已完成（2026-10-24,issue #10）**: `--vmc4-regfile <n>`（默认 0 关、不联动）。线性扫描分配（块级活性不动点 → 连续包络区间 → Poletto）把计算寄存器压入定长池 `[numInputs, n)`,死值槽位复用,寄存器号不再镜像 SSA value id;装不下的活值分稳定溢出槽。实现相对原稿的偏差:原稿的 Spill/Reload 两个新 opcode 改为溢出槽直接寻址（统一 slots 数组高位索引,操作数直接读写）,零运行时搬运、零 opcode/解释器/trace 面改动;若未来运行时拆定长栈上数组 + 独立溢出区再补双 opcode。 fail-closed:limit < numInputs（溢出槽与参数区重叠）拒编;fragment 组合要求 limit ≥ numInputs + 3（碎片临时槽占池顶）;numRegs > 0xFFFF 编码入口拦截。分篇见 [doc/techniques/vm-regfile.rst](doc/techniques/vm-regfile.rst)。

**大改动项（需决策）**

10. ~~交叉编译 + 多目标产物（第一切片）~~ **已完成（2026-10-24,issue #11）**: `--targets <abi,...>` + `--target-cc <abi>=<path>`（显式交叉工具链, zig 为仓内默认交叉面、自动带 `-target` 三元组;宿主 ABI 回落自动探测）。逐 ABI 产物落 `<输出目录>/<abi>/<库名>`,manifest `<stem>.targets.json` 记录 built/skipped/failed 与原因（CI 集成挂点）;退出码两态（全部 built → 0）。v1 定界（定界外 fail-closed）:C99 后端（regc/vmc4）+ 直接驱动（file/pipe）;xmake/LLVM/StackC/jar 嵌入/native-dir 拒绝。 **剩余（后续切片,需决策）**:fat JAR + 多 ABI jar 嵌入 + NativeLoader 按平台挑选注册、bundled zig / toolchain 反射自动发现、darwin/mach ABI 档,对位 jvmtp 最有壁垒的部分仍未闭合。
11. Gradle 插件 + Java SDK SPI（可替换 packer/compiler 的显式 LifecycleEvent API）。
12. packer（代码段流加密 + 两阶段 loader + 免落盘）+ 签名流水线。

## 验证命令

```bash
# 全量配置（选项见 xmake/options.lua；--with_mj2x 总开关已随拆仓退场,勿再传；
# 勿用 -P 子项目,issue #782）。测试 target 需显式开 --with_tests=y。
xmake f --with_tests=y -y
xmake build mj2x_unittest && xmake run mj2x_unittest --gtest_brief=1
xmake build mj2x_integration_test && xmake run mj2x_integration_test
```

> 性能数据必须先做覆盖率审计。
> 注意:llvm sample E2E 外呼 `mj2x-cli.exe`,验证前须确认 CLI 已重建（`xmake build mj2x-cli`）,否则跑的是旧发射器（#894 教训）。
> unittest/integration 的 skip 数含环境性 skip（本机无 clang/zig 时 LLVM 列 exec 编译与性能检查整列跳过）,跨机对数时 skip 差异属环境差异,不构成回归判据,对数只看 passed/failed。
