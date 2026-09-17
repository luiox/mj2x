# dev_plan：mj2x 施工计划

> 本文件只保留**待做项**与**状态总览**；已完成阶段的详细落地记录已归档到
> 对应 PR / git log（见文末"已完成归档"索引）。依据 issue #787 §3（mj2x 部分）。

## 状态总览

| 主线 | 内容 | 状态 |
|---|---|---|
| 一~五、八~十一 | VMC4 可用版 / RegC 性能（含方法内联）/ 异常精确化 / 反侦测体系 / 文档 / 用户 intrinsic（#818）/ program 粒度（#827）/ 混淆借鉴与默认关（#829） | ✅ 完成（见归档索引） |
| 六（剩余 issue） | VMC4 升级档：period 语义变义（#762/#844）已实现，N-7 对抗全闭环 | 🟡 S2 对抗验证 + 红队契约扩展待做（见待做 A） |
| 七（#820 测试集） | 步骤 1-5 完成（调研/设计/Wave3/三后端矩阵/性能回归自动化） | 🟡 步骤 6 对抗闭环待做（见待做 B） |
| 十二（配置系统） | P0+P1 完成：参数注册表 + 全量配置化 + `--dump-config-schema`/`--dump-config-template` | 🟡 GUI 配置工具（P2）待做（见待做 D） |
| 十三（LLVM IR 后端） | 基础款转正默认开启（v2.2）+ 二次扩面 11 项（对象/数组/实例调用/异常/monitor/ldc/窄类型等，PR #864） | ✅ 完成（见归档索引） |
| 十四（LLVM 第二轮扩面） | invokedynamic 运行时解析、intrinsic 扩面对齐 RegC、保护特性接入、mtest_set 矩阵 + protected_samples llvm 样本 | 🟢 完成（Math/包装类等 RegC-only intrinsic 下沉除外，见待做 E） |
| 十五（mj2x-llvmrt 运行时下沉） | #894：llvm_ir_helpers.cpp 固定 IR helper 逐批下沉 mj2x-rt/src/llvmir（declare 化）；源内 define internal 41→1（仅剩 bsm 站点模板）；benchmark 中性（`benchmark/results/2026-08-23-llvmrt-sink.md`） | ✅ 完成（见归档索引；顺带修复 Compare-REF Eq 裸拷贝发射缺陷 c1ce74d79） |
| 十六（sprint 2026-09-17/18） | #760 切片 2 lazy 分块解密+密钥擦除（`--vmc4-lazy-decrypt`）；#887 探针级优化三连（class literal 缓存/trampoline 条件发射/装箱缓存，基准 ratio 1.36→1.12，`benchmark/results/2026-09-17-regc-probe-optimization.md`）；indy 原生派发判定原型+设计文档；#879-X3 stackc indy jclass 缓存；passReceiver intrinsic 扩面 | 🟢 波次一+部分波次二完成；indy backend 合流见待做 H |

## 待做项

### A. VMC4 升级档 S2（P2）

- period 语义变义（case 标签周期轮换）已实现（#844）且 mjdbg N-7 对抗闭环就绪
  （五形态识别 + period 变义 label 表提取/case 常量反查 + computed_goto 产物修复，
  见 `mj2x/doc_ai/vmc4_red_team_contract.md` §6）。剩余：开 `feat/mj2x-vmc4-mutation`
  的 S2 对抗验证 + 红队契约扩展（变义强度模型同批）。

### B. mj2x 自建测试集（#820）

- 步骤 6 待做：mjdbg 对抗闭环（每用例 expected trace surface + weakness report，
  与 mjdbg 工具端联动）。

### C. 记录不阻塞（P3）

- VMC4 检查点指令级异常匹配（javac 字节码下块粒度==指令粒度，无实际缺口）、
  agent 特征深度检测（javaagent MANIFEST 扫描 / JDWP 探测）、异常表多 region
  匹配顺序测试扩展、runtime C++ 版与 crt C 版检测面统一。
- JVMRTDP 调研（issue #847）：纯 Java 面 JVMTI 工具，对 mj2x 保护产物（native
  化/VMC4）**无实质威胁**。观察 a：agent 用 manual mapping 加载可绕过「已加载
  模块枚举」（Windows 侧；capability 快照 + 启动参数扫描仍覆盖，记录不实施）；
  观察 b：RegC 无字符串加密变换（对标 jvmtp `obfuscation.strings`），敏感字符串
  场景需加密模块（P3）。

### D. 配置系统 GUI 工具（P2）

- 配置系统 P0+P1 已完成（见归档索引）。设计已完成
  （`doc_ai/gui_config_tool_design.md`：vite+vue 方案、schema 驱动渲染、
  联动/校验/导出/可选 sidecar 执行）。剩余：P1 实现（表单渲染 + 校验 + 导出）。

### E. LLVM 第二轮扩面（已完成，PR #870）

- **invokedynamic 运行时解析** ✅：残留 `indy:<idx>` 站点（自定义 bootstrap）不再
  fail-closed——LLVM 发射模块级全局 cache（`NewLocalRef` 缓存 + `NewGlobalRef`
  稳定化）+ 运行时 bootstrap 解析（`MethodHandles.lookup` / `MethodType` /
  `privateLookupIn` / bsm 调用 / `CallSite.getTarget` / `invokeWithArguments`，
  参数装箱、返回拆箱）。与 RegC 对齐（regc_backend 的 emitIndyBootstrapFunction
  同功能——原文"超出 RegC"失实，2026-08-16 审查订正）。
  验证：`CliTest.LlvmIndyCustomBootstrapE2E`（plain / runtime-protection fake /
  regc-obfuscate 三变体，输出 7-42-11）+ `LlvmIrBackendTest.EmitsIndyRuntimeResolution`。
- **intrinsic 扩面对齐 RegC** 🟡（Math 纯函数 + 包装类 + Random + StringBuilder
  完成）：`java.lang.Math` 全纯函数面直调 mj2x-crt runtime 符号（abs/min/max
  I/J/F/D、sin/cos/tan/sqrt/floor/ceil/pow、round、random；0 JNI、无
  ExceptionCheck，declare 模块级输出，链接侧并入 mj2x-crt 源）；**包装类
  valueOf/xxxValue 非逃逸切片**（Integer/Long/Short/Byte/Float/Double/Boolean/
  Character——shadow 结构 `%mj2x_int` 等 + 窄装箱 trunc / 窄拆箱 sext/zext，
  逃逸根经 copy 联通分析回退 JNI，对齐 RegC regc_intrinsics.cpp）；**Random
  状态机切片**（非逃逸 new Random(seed) + nextXxx 直调——RegC 同 face 为 inert；
  无参构造保持 JNI）；**StringBuilder/StringBuffer 状态机切片**（非逃逸
  new Builder + init()V/(I)V/(String)V/(CharSequence)V + append 全 13 面 +
  toString/length/capacity/charAt/ensureCapacity/setLength/setCharAt/
  appendCodePoint——函数级 `%mj2x_regc_context` 前导（init_context +
  throw_exception 回调）+ 每次调用 mj2x_regc_status 检查（非 0 → 异常分发/
  默认返回），状态为 crt 堆分配 `mj2x_string_builder*`）。String 类面
  （length/charAt/substring/equals 等 RegC-only）保持 JNI 分派，留待整类
  下沉 F。
- **保护特性接入** ✅：`--runtime-protection` 支持 llvm——每方法入口注入
  `%mj2x_rt_checkpoint`（模块级全局 + `mj2x_rt_checkpoint_run` 调用，crt
  `mj2x_rt_protect.c` 在 link/工程层追加；FAKE 动作走默认返回路径）。
  `--regc-obfuscate` 及 MBA/opaque/CFF 变换选项支持 llvm（RegIR 级同源变换；
  mbaDeep 的 Barrier 指令 LLVM 子集不支持时回退未变换形态并告警）。
  `--regc-safe-env` 明确不适用（JNI 表快照恢复是 RegC 运行时面）。
- **测试矩阵接入** ✅：`mtest_set_native_matrix_test.cpp` 加 LLVM 列（22 fixtures
  允许清单，fail-closed 自动排除未列入者；clang 探测缺失时整列跳过）；
  `protected_samples/` 增加 llvm 样本目录（Tiny/IndySurface/ExceptionMonitor 三样本，
  `LlvmSamples.GenerateMjdbgFixtures` 生成 native.ll + clang 编译 + Driver 真跑）。

### F. 整类下沉（P3，记录不阻塞）

- roadmap §2 表格行已列（算法密集类整体 native 化，边界保留 JNI）；GUI 设计已
  预留"方法/类选择"扩展点（`gui_config_tool_design.md` §6）。条件：无反射、无
  动态类加载、intrinsic 覆盖充分；先做单类试点再铺开。

### H. indy 原生派发 backend 侧合流（波次二后续，P1-M）

- 管线层静态判定原型 + 设计文档已落地（`doc_ai/indy_native_dispatch_design.md`
  §2 判定树 / §3 缓存 / §4 回退 / §6 接口提案 P1-P10）。剩余：提案逐后端落地
  （P1 MIR 站点载荷、P2-P4 regc 直派发射与 finalize 缓存与 D2 破解、P5 stackc
  F1/F2 分支、P7 vmc4 编码前改写式、P10 llvm 模块级缓存分派）。
- 注：P6（stackc bootstrap 结果缓存）经核已有等价物
  （stackc_mir_invoke.cpp indyTargetCache，static jobject per call-site），
  **无需实施**（设计文档写作时点误判，2026-09-18 审查订正）。
- #879-Y4（Random 预检 registry 标志位化，替换两后端符号名/签名硬编码耦合）
  建议与本项同批，对齐 cross_backend_semantics.md §3.1 单源方向。
- 三后端共享层抽取（调研候选 4）继续等本批后端施工全部合流后再评估。

### G. 全线审查 round-3（2026-08-17，分支 `review/mj2x-audit-fixes`）

四方向并行审查（前端/core、RegC、StackC/VMC4/LLVM、CLI/CRT/构建/文档），
高危项已逐行复核确认。分两批处理：

**G-1 本批修复**（简单、独立可回退，逐项独立 commit，✅ 2026-08-17 已完成，
见日志与分支 `review/mj2x-audit-fixes`）：

- [正确性-高] mbaDeep De Morgan 变体恒等式错误：`reg_transform_passes.cpp`
  t6 移位的是 `~(a&b)` 应为 `(a&b)`（`a=0,b=0` 得 -2）；零测试覆盖，需连补
  位运算穷举样本。
- [正确性-高] LLVM 整数 div/rem 用 udiv/urem，负操作数结果错（JVM `-7/2=-3`
  vs udiv 2147483644）；改 sdiv/srem + INT_MIN/-1 特判，对齐 StackC/VMC4。
- [正确性-高] RegC `getClass().getName().hashCode()` 融合跳过窗口无条件
  `continue`，吞掉窗口内 StoreField/ArrayStore 副作用与中间 dest（后续读
  0/NULL 静默错码）；改为窗口内仅跳过 getName/hashCode 两条，其余原位发射。
- [正确性-高] VMC4 `scanComputeRegisters` 漏 Switch 终结者边 copies（常量槽
  可能分配到边拷贝 dest 上被覆写 / numRegs 偏小致 regs[] 越界）；
  `encodeAllocate` 缺维度 >32 fail-closed（解释器 argBuf[32]）。
- [正确性-高] `OutputWriter::writeBytes` 静默吞写失败（打不开/写错误只记
  日志），磁盘满时 pipeline 返回成功。
- [正确性-中] indy preprocessor 改写后不置 `cls.modified`（JAR 模式写回原始
  字节，展开静默作废）；3 处 `InsnList::remove` 节点泄漏（remove 只摘链）。
- [正确性-中] CFF 随机 state code 无去重，同码 dispatcher 静默路由错误。
- [杂项] RegC：privateThis 分支缺 convertedFunctions 判空、INT64_MIN 字面量
  发射（`(-9223372036854775807LL - 1)`）、varargs >32 参数 fail-closed、
  MultiArray 空维防御、splitRewriteStatic 词边界、no-stack-anchor 门禁
  `Cannot invoke` 自然语言锚点误伤。LLVM：include 挤行、jni_slot 裸字面量
  25/21、finalize 不清 maxStack/maxLocals（对齐另两后端）。VMC4：
  vmOpcodeName 缺 SuperInc/SuperDec。StackC：死循环遍历删除。CLI：
  readFileBytes 短读/tellg 检查、main.cpp 重复 include、工程模板 `%.*s`
  fmt bug、pipeline 中段异常无 catch。SSA：GUARD_LIMIT 截断加告警。
- [runtime] rt_str_obf.cpp 无效 memcpy 往返删除；intrinsic_math 取负有符号
  溢出 UB（改无符号取负）、Math.round 上界 DBL_MAX/2 → 2^63、Math.random
  共享 LCG 原子化；box/unbox FindClass local ref 释放；dbg_depth
  thread_local；sha256 aesExpandKey 块级重复调度外提。
- [文档] `--vmc4-dispatcher` 两处文档记载的 `func_ptr`/`state` 等非法值；
  `c_compiler` 文档默认值 `auto` 不可显式书写（改为合法别名）；user_guide
  参数表补 `--emit-native-dir`/`--compile-mode`/`--stackc-exception-check`；
  mj2x-runtime readme 失效 `RUNTIME_*.md` 引用。

**G-2 待评估**（大改 / 需实测 / 需设计决策，本批不动）：

- rt_protect Linux rwx 检测大概率对宿主 JVM CodeCache（rwxp）误报，force=1 +
  terminate/fake/delay 下自毁；需 Linux 真机验证后按 safe_env 模式排除
  libjvm 自身映射。现有 exec 级测试全在 Windows，掩盖此问题。
- RegC 进程级缓存并发安全：**2026-08-25 已定契约并落地**（`mj2x_regc_runtime.c`
  文件头契约注释）——jclass 全局槽 CAS 发布（输家自删复用赢家，消除双建
  泄漏）；lastReceiverClass 只增不删（CAS 换占，旧 ref 不 Delete，消除
  Delete→New 窗口下 IsSameObject use-after-free；泄漏上界 = 每调用点 distinct
  receiver 类数）；jmethodID/jfieldID 槽解析幂等保留 check-then-write。残留：
  class→method 两写间的短暂错配窗（与旧代码同窗，需原子对/seqlock 才能闭）；
  字符串字面量 32 槽缓存的瞬时覆盖竞态（越界安全，维持既有注记）。
- CLI：外部进程调用（编译器/xmake/pack hook）无超时（libca `wait_for` 未
  用；`wait_with_output` 无超时参数，需 libca 扩展或手写并发排空框架，
  保留待办）；pipe 模式先写 stdin 后排空输出的死锁窗口 **2026-08-25 已修**
  （stdin 写入挪独立线程 `spawnStdinWriter`，三处 pipe 站点收敛，C 管道 /
  clang 直连 / llc 链）。
- 三后端共享层抽取（结构性最大项）：RegisterNatives 基础设施、JVM 类型格点
  映射表（10+ 张平行）、indy bootstrap 运行时解析（3 份）、多维数组分配
  （3 份）。LLVM udiv bug 即三份手工同步"修一漏二"的实例；jvm_semantics.hpp
  已有共享先例。
- backend.hpp 收拾：Kind::BINARY 死枚举、llvmir `.ll` 挂 C_SOURCE 语义错位、
  finalize 无错误通道（三后端失败行为不一致）、接口默认方法能力边界
  （VMC4/LLVM 无 StackC 的代理机制，MarkAll 下产出带 ACC_NATIVE 的接口方法
  → ClassFormatError）。
- RegC intrinsic 判定热点：每条 Call 重复 match 6-10 次（每次 3 个
  std::string + ~150 条表线性扫），应函数级一次算好 decision；registry 视图
  不统一（analyze 走 builtins、emit 走用户 registry，覆盖场景判定漂移）。
- 用户 intrinsic 实例方法：**2026-08-25 已修**——`matchUserCall` 补 isStatic
  门控，instance 调用点按契约（doc/user_intrinsic.md「只匹配静态调用」）直接
  回退通用 JNI 派发；修复前同 owner/name/desc 的 instance 调用走 argCount
  错位（含 receiver）的 Unsupported 路径，整方法 fail-closed。回归锁
  `RegCUserIntrinsicTest.InstanceCallSiteNeverEntersUserIntrinsicPath`。
- indy 装箱/拆箱缺 Boolean/Character 通道：**已修**（预处理器/emitter/运行时
  三层均已补 boolean/char 分支，2026-08 复核确认）。
- 性能：SSA 构建 O(n²)（fallthroughBlock 线性扫 / findBlockByLabel /
  reaching_def 无邻接表）；JAR 输出每条目全量拷贝；llvmir
  GlobalStringTable::intern 线性查。
- CLI：无下界整数参数负值回绕。
- 死代码清理批次：MethodStack 整类、FieldID、emitNativeMethod 失败分支、
  shouldKeepBytecodeForCompatibility 恒 false、_mj2x_intrinsic_string_* 无引
  用 helper、MONITOR_EXIT 死枚举、cli dispatcher 别名死分支。
- 约定基调：std::string vs Utf8String、int vs i32 在 CLI/codegen/后端三层
  各自为政，需在 capabilities/AGENTS 层面定一次豁免边界。

## 验证命令

```bash
xmake f --with_mj2x=y --with_mtest_set=y --with_tests=y -y   # 全量配置（勿用 -P 子项目，issue #782）
xmake build mj2x_unittest && xmake run mj2x_unittest --gtest_brief=1
xmake build mtest_set_unittest && xmake run mtest_set_unittest
```

> 性能数据必须先做覆盖率审计（见 `performance_evaluation_spec.md`）。
> 注意：llvm sample E2E 外呼 `mj2x-cli.exe`，验证前须确认 CLI 已重建
> （`xmake build mj2x-cli`），否则跑的是旧发射器（#894 收尾时踩过）。

## 日志

- 2026-08-23（mj2x-cli 解析迁移 ca::opt v2，分支 `feat/mj2x-cli-opt-v2`）：
  libca PR #198（opt v2：类型化 Arg/十类错误/互斥组/ValueSource 来源查询/
  OptionalString）评审后合入，third_party/libca 同步至 `9ea6b4d`。main.cpp
  手写 argv 双循环迁移为 Parser::parse + 注册表校验环：`buildOptCommand()`
  按注册表 ParamType→OptKind 映射登记 67 参数（值语义校验统一留 parseCli
  单源），错误按 ParseErrorCategory 映射既有 i18n 键，HelpRequested 接自研
  printUsage；校验环仅处理 CommandLine 来源——配置文件值已由
  loadConfigFile 直写 options，注入式三级优先级对本项目冗余且会破坏 emit
  输出组"首个 CLI 输出清空其他"替换语义（回归锁定）。行为变更共三处
  （均 fail-closed，各由 E2E 锁定）：① --dump-config-schema/template 文件
  目标改内联 `=` 形态；② --lang 悬空无值由静默容忍改为 MissingValue 报错；
  ③ --lang 空格形态会消费后继 '-' 开头 token（迁移前静默忽略 lang），
  合法用法为内联 --lang=<lang>。--quiet/--verbose 维持 quiet 赢。前瞻
  耦合（ParseError 不携带互斥组标识）已由 libca PR #201 落地解决：
  MutexGroup::label → ParseError.group，mj2x 按 "dump_once" 组标识分派
  文案并补双 dump 互斥 E2E（BothDumpFlagsRejectedViaGroupLabel）；
  StringList 参数合法项不得含逗号（opt 拆分前提，已注记
  buildOptCommand）。验证：全量 mj2x_unittest 632 过/3 skip +
  mtest_set 25/25；CLI 面冒烟逐项对齐（help/未知选项/缺值/dump 形态/
  config 三级优先级）。

- 2026-08-23（#894 llvmrt 运行时下沉收尾，分支 `refactor/mj2x-llvm-runtime-sink`）：
  阶段三b 字段族 ×9（`b0b21af75`）、三c 数组元素族 ×8（`6ca864156`，byte 特判
  boolean[]）、三d 调用族 ×10（`8c45b207a`）、四a class_const/throw_new
  （`7bdab6f5f`）、四b monitor + 死常量清理（`412677393`）、四c indy 直调面 +
  box/unbox ×8（`49f85f160`）、四d indy 固定 helper 组 ×8（`67dd8b6cc`）。
  源内 define internal **41→1**（仅剩 `mj2x_llvm_indy_bsm_<tag>` 站点模板，
  按运行时/模板准则留 emitter）；发射端固定常量仅保留 bsm 引用的
  `java_lang_Object`。每批全量 mj2x_unittest 绿。收尾 benchmark：CLI 编译耗时
  中性（±5% 噪声带内）、.ll 体积 -56%~-80%、dll +4~5%、产物运行 48 样本交错
  中位数无回归证据——报告 `benchmark/results/2026-08-23-llvmrt-sink.md`
  （含 javap 覆盖率审计表，两侧 native 状态完全对称）。顺带修复 benchmark
  暴露的既有缺陷：Compare-REF Eq/ifnull 路径发射 `%vN = %vM` 裸拷贝伪指令，
  clang 拒绝任何含 null 检查的方法（main 同样命中；`c1ce74d79` +
  EmitsRefEqWithoutBareCopyPseudo 回归）。过程教训入验证命令节：sample E2E
  外呼的 mj2x-cli.exe 必须先重建，否则验证的是旧发射器。

- 2026-08-17（round-3 全线审查修复，分支 `review/mj2x-audit-fixes`）：四方向
  并行审查（前端/core、RegC、StackC/VMC4/LLVM、CLI/CRT/文档，发现记录见
  待做 G）。**G-1 本批修复全部落地**（11 commits）：高危正确性 5 项——
  mbaDeep De Morgan 恒等式（(0,0) 得 -2，改 (a&b)+(a|b) 形态 + 直线求值器
  语义测试）、LLVM 整数 div/rem 负数结果错（udiv→sdiv/srem + INT_MIN/-1
  防护分支 + E2E 四站点）、RegC getClass 融合窗口吞中间指令（双指针仅跳
  getName/hashCode，其余原位发射 + E2E 三变体回归）、VMC4 寄存器预扫描漏
  Switch 边 copies（常量槽撞车窗口，手构 RegIR 回归）+ allocate 维数上限、
  OutputWriter 写失败静默（上抛 + pipeline 中段兜异常）；中危——indy/ldc
  预处理器 modified 标志 + InsnList::remove 泄漏（连带 native_loader 注入
  条件解耦为 modified∧ACC_NATIVE）、CFF state 码查重；杂项批——regc
  （privateThis 判空/INT64_MIN/varargs>32 fail-loud/MultiArray 空维/词边界/
  门禁自然语言锚点误伤/dbg_depth thread_local/box-unbox local ref）、
  llvm（include 挤行/maxStack 清零/jni_slot 裸 25/21）、stackc/vmc4/ssa
  小修、cli（fmt bug/c_compiler 显式 auto）、io（JAR 全量拷贝消除）、
  runtime（取负 UB/Math.round 2^63 边界/Math.random 原子化/AES 密钥外提/
  删死代码 rt_str_obf）、文档订正（dispatcher 枚举值/参数表缺项/失效引用）。
  回归守门测试均反向验证（旧代码失败、修复后通过）。验证：mj2x_unittest
  全量 **602 过 / 17 skip / 0 fail**（修复中段一次全量 603 过，无失败）；
  mtest_set_unittest 25/25；
  mj2x_integration_test 49 过 / 25 skip（本机无 clang 的 LLVM E2E 列与
  性能门控 skip）；mj2x_runtime_test 63/63。构建配置恢复 with_mj2x_llvm
  默认开。G-2 待评估项（rt_protect Linux、RegC 并发、三后端共享层等）不动。
- 2026-08-17（regc 13-probe 性能第三轮：JNI 局部帧按需发射）：**Total
  ratio 0.95→0.90**（同轮对比；本轮机器整体降频，绝对值偏高但 ratio
  稳定）。无环 CFG 且产 local ref 指令 ≤6 的函数豁免
  PushLocalFrame/PopLocalFrame（每次调用省 2 JNI，稳态零 ref 的叶子
  函数被热循环直调时收益最大；全量帧数 95→45）。受益：Inheritance
  1.55→1.0、Exception 1.37→0.87、Reflection 1.26→0.90、String
  1.12→0.95、Crypto 1.04→0.97。基准脚本再修：native 编译必须在
  cwd=projDir + `xmake -P .` 形式调用（从 benchmark 目录裸调用时
  xmake 向上加载 root/mj2x 缓存配置，平台漂移 mingw）。
- 2026-08-17（regc 13-probe 性能第二轮：整体反超 os30）：**Total ratio
  1.07→0.95（116ms vs os30 122.3ms，MSVC -p windows 口径），首次整体反超**。
  两项优化：`getClass().getName().hashCode()` 链融合 intrinsic（emitter 前瞻
  匹配 + CRT 双槽 receiver 身份缓存 + 自旋锁，3 JNI→1，Inheritance
  1.72→1.55、Reflection 1.38→1.26）；`substring(I|II)` intrinsic 回退
  （CRT 版本 4 JNI + 全串拷贝劣于 1-JNI JDK 调用，String 1.54→1.12）。
  基准脚本修复：UTF-8 BOM（PS5.1 ANSI 解析炸中文）、`-P` 移到 `build`
  子命令后（本机 xmake 全局选项前置报 invalid argument）、`java -version`
  经 cmd /c 合并 stderr（PS5.1 NativeCommandError）。验证：mj2x_unittest
  613 过/3 skip、mtest_set 25/25、E2E 套件绿、覆盖审计 RegC 74.7% vs
  os30 61.6%。遗留：Inheritance 1.55（bias 字段/checkcast/接口分派）、
  Exception 1.37、ControlFlow 1.34、Crypto 1.04（JDK 侧主导）。
- 2026-08-17（regc 13-probe 性能追平 os30）：**Total ratio 1.14→0.98（118.8ms
  vs os30 121.6ms），7/13 探针反超**（Enums 0.53x / Visitor 0.77x / Reflection
  0.91x / Resource 0.15x / ClassLoading 0.67x / Collections 0.98x / Inner 1.00x）。
  分支 `perf/regc-probe-optimization`，三项优化（详见
  `benchmark/results/2026-08-17-jvm-obf-tester-os30.md`）：枚举 intrinsic
  （子类 name/ordinal 直连 + values/valueOf javac 合成形态内联，后端预扫描
  在 injectIntoClass 清空指令前验证方法体）；StringMethod 亏损 intrinsic 回退
  （hashCode/charAt/equals 的 3-6 JNI 数据管道不如 1-JNI 缓存调用 + HotSpot
  VM intrinsic；反直觉教训：GetStringUTFChars ASCII 快路径实测也比 3 个 UTF16
  调用贵，已试并回退）；确定性虚调用免检直调（final/无覆写时跳过 IsInstanceOf，
  含匿名类 ACC_FINAL 缺失修正与 convertedFunctions 声明类回查）。验证：
  mj2x_unittest 全量绿（CliTest 3 例为 shell 环境 MSYSTEM/PATH 污染 xmake
  平台检测所致，`env -u MSYSTEM` 后全过）；E2E 13/13。遗留：Inheritance
  1.41x（checkcast 复用/类名 hash 缓存）、Crypto 1.21x（JDK 侧主导）。
  另修复 main 上 native_compiler.cpp environmentKeyEquals 链接错误。
- 2026-08-22（mj2x-runtime 整树退役,R3 整合收尾,同支追加）:删除
  `mj2x/mj2x-runtime/`(intrinsic C++ 四件套由 `mj2x_regc_intrinsic.c` 接管,
  余 4 符号核实零消费弃用;保护模块探测已移植 rt_protect;jni_lib/runtime_entry
  为 aspirational 死代码);`mj2x/xmake.lua` 移除 includes;regc e2e 清死代码
  lambda,vmc4 parity 改链 crt intrinsic(与生成工程同源);capabilities/readme/
  architecture/test_strategy/regc_intrinsic/样例 README 收敛。
- 2026-08-22（mj2x-runtime 处置定案 + R3 保护探测移植实施,分支
  `feat/mj2x-rt-protect-probes`）:intrinsic C++ 四件套 49 符号中 45 个与
  `mj2x_regc_intrinsic.c` 重复;crypto/kernel/antidebug/antijvmti/detection/
  stringpool 六件套零消费但非弃件(R3 预研)。按
  [doc_ai/r3_protect_integration.md](doc_ai/r3_protect_integration.md) 将 C++ 独有
  探测 C99 化并入 `mj2x_rt_protect.c`:**PEB BeingDebugged/NtGlobalFlag**、
  **NtQueryInformationProcess 三元组**(动态加载,容忍 STATUS_PORT_NOT_SET)、
  **硬件断点 Dr0-3/Dr7**(地址+使能双条件防误报)、**Linux agent 库映射扫描**
  (maps 含 "agent" 的 .so,补 attach 面);软断点自检有意缓议(0xCC 误判代价
  与 TERMINATE 不对称)。接口零变化;纯解码器导出供单测
  (`mj2x_rt_protect_probe_test.c` 挂 runner 惯例)。验证:mj2x_rt_test 全过;
  CliTest 保护链路 14 项 e2e(真实 JVM 注入检查点)全过零误报。
- 2026-08-22（#894 LLVM 片段盘点收口 + mj2x-llvmrt 设计定稿,同分支追加）:
  四后端内嵌代码盘点至此全部完成并定性——**LLVM**(`llvm_ir_helpers.cpp` 1640 行)
  向每模块发射 ~90 个 `define internal` IR 函数(throw/new/field_load 9kind/
  array 8kind/box/unbox/monitor/exception/string_const/class_const)。定性:固定体
  =运行时(IR 手搓 JNI 槽位分发 `getelementptr i64 <slot>`,转 C 反而简化为
  `(*env)->FindClass` 一行;多数与 vmc4/stackc 运行时语义相同可复用);真模板留
  发射器:emitIndyBootstrapFunction(per-site)、GlobalStringTable(per-program)、
  内部互调 @mj2x_llvm_Java_*。**转换设计**:新建 mj2x-rt/src/llvmir/
  mj2x_llvmir_runtime.{h,c},保持现有符号名与 ABI(i8=jboolean/i16=jchar/jshort),
  .ll 从 define internal 改 declare-only;addCrtSources 递归收集自动接线。
  **分阶段原因**:前两轮为零语义风险原样搬运,本轮是 IR→C 重写且 internal→extern
  失去模块内联(LLVM 直调粒度比解释器分发更敏感),按仓库规约性能变化需先做
  benchmark+覆盖率审计——物理下沉独立成支,设计已定稿可直接开工。
- 2026-08-22（#894 运行时/模板分类下沉 round-2，分支 `refactor/mj2x-embed-runtime-split`）：
  按「运行时=固定代码→mj2x-rt 链接消费;模板=生成期动态拼接→发射器」口径完成剩余两类——
  **VMC4 prelude**（slot/call_fn 类型 + div/rem/f2i/throw/exception_matches,原
  `emitRuntimePrelude` 逐 TU 内联）→ `mj2x-rt/src/vmc4/mj2x_vmc4_runtime.{h,c}`；
  **StackC prelude**（126 行零占位符"模板"实为运行时:cache 宏/inline 热路径留头内,
  find_class/component_class/new_multi_array/mid_slow/fid_slow/class_slow 六个 slow
  path → `mj2x-rt/src/stackc/mj2x_stackc_runtime.{h,c}`,删除 stackc_runtime_prelude
  三件套）。rt_protect 判定已正确（调用片段=模板,实现在 rt）不动。接线：rt.resources/
  kRtSourceMounts 增 stackc/vmc4 挂载；`--compile` 四后端统一要求源包；e2e 编译清单
  补 runtime 源与 /utf-8（header 中文注释 cp936 必错）；vmc4 e2e 结构断言改 include 标记。
  验证：`mj2x_unittest` 全量 **630 过 / 0 fail**。
- 2026-08-22（#894 模板资源化 + B 类真下沉收尾，分支 `refactor/mj2x-embedded-code`）：
  四批落地——**第一批** stackc runtime prelude 模板资源化 + `c_render` 文本模板渲染器
  （bfaf462b7）；**第二批** regc `emitRuntimeHelpers` 迁移
  `resources/codegen/regc_runtime_helpers.c.in`（933a333fb）；**第三批** field/method
  helper 并入运行时模板、`emitRuntimeHelpers(out, cacheDeclarations, fwd)` 签名收敛
  （60cad5f0e）；**B 类真下沉** RegC 运行时主体物理下沉 `mj2x-rt/src/regc/
  mj2x_regc_runtime.{h,c}`——helper/角色单例缓存/TLS env-ctx-owner_class/trace 状态/
  intrinsic 静态全部去 static 化，头文件 extern 声明，残量模板仅剩
  `@FORWARD_DECLARATIONS@`/`@CACHE_DECLARATIONS@`，backend 停发 11 个运行时符号、
  prologue 经 `mj2x_regc_set_trace_call_log` 注入 trace 开关（74f040ebf）。
  连锁修复：header 变量声明补 `extern`（C++ TU 误定义致 LNK2005）、regc/vmc4 两处
  e2e 的 cl 链接清单补 `mj2x_regc_runtime.c`（LNK2019）、samples fixture requiredText
  改为实际调用的 helper。验证：`mj2x_unittest` 全量 **630 过 / 0 fail**。
- 2026-08-21（round-2 收尾·toString 修正）：**`StringBuilder.toString()` 映射修正为
  to_jstring**——实时隔离定位（非猜测）：独立 hex Driver 反调 `concat()`，返回
  `java.lang.StringBuilder` 对象（`CLASS=java.lang.StringBuilder`）而非 String；
  `concat("k",7)` 单独调用即 len=16、'k'=7 + 11 个 NUL。根因：LLVM intrinsic
  registry 把 `StringBuilder.toString` 误映射到 `mj2x_string_builder_materialize`
  （转义桥，返回真 Java StringBuilder 对象，头注释明示），而 Java 语义要求
  toString 返回真 String。修复：toString → `mj2x_string_builder_to_jstring`
  （真 jstring），StringBuffer 同步 `mj2x_string_buffer_to_jstring`，单测断言跟随。
  证据链：修复前 `.ll` 与正确样本 hash 相同（emitter 无错、缺陷在 crt 调用选择）；
  修复后 `LlvmIndySurfaceSample/native.ll` 已 `call i32 @mj2x_string_builder_to_jstring`；
  `LlvmSamples.GenerateMjdbgFixtures` PASSED（`[k]=7 slice=5 types=true`）。提交 1b6f4f721。
- 2026-08-21（round-2 收尾·全量验证与文档）：`mj2x_unittest` 全量 **600 过 / 3 skip /
  0 fail**（58 suites、707s；3 skip = EmitsExceptionDispatchWithRegions 由 CliExc E2E
  覆盖 + RealZig* 本机无 zig）；`mtest_set_unittest` **25/25 过**；`mj2x_integration_test`
  **66 过 / 1 skip**（67 例：MatrixBaseline 语义矩阵含 LLVM 列全绿；skip = 性能回归门控
  `MJ2X_PERF_REGRESSION` 未设）。分组回归全绿：LLVM 32 过 1 skip、RegC 108、StackC 83、
  VMC4 145。`CliTest.GenerateXmakeProjectUsesEmbeddedCrtOutsideRepo` 本机为环境性偶发
  （spawn 的真实 xmake 退出 0xC0000374 堆损坏；该测试早于本 PR，手工独立工程 xmake
  f+build 均 exit 0，全量跑中亦通过），判定非回归。文档同步：roadmap §2 Intrinsic 行 +
  §4 LLVM 状态行（PR #870 完成态）、user_guide §5.4（invokedynamic 运行时解析 / intrinsic
  扩面对齐 RegC / 保护特性 / to_jstring，fail-closed 列表剔除 invokedynamic）；capabilities
  无回退。
- 2026-08-21（round-2 收尾·补 intrinsic 4）：**StringBuilder/StringBuffer 状态机
  切片**——非逃逸 builder 全链直调 crt（init 四面 + append 十三面 +
  toString/length/capacity/charAt/ensureCapacity/setLength/setCharAt/
  appendCodePoint；函数级 `%mj2x_regc_context` 前导 + 每次调用 mj2x_regc_status
  检查；append 结果与 receiver 联通；修复 init 的 out_state 参数顺序
  （crt 签名 (context, args..., out_state**)，曾把槽放参数前导致写穿 jstring））。
  验证：E2E 与 JVM 逐位一致 `b=7,...,d=3.2` + `len=7`。
- 2026-08-21（round-2 收尾·补 intrinsic 3）：**Random 状态机切片**——非逃逸
  `new Random(seed)` + nextInt/nextInt(bound)/nextLong/nextBoolean/nextFloat/
  nextDouble 直调 mj2x-crt（`%mj2x_random = { i64 }` 指针状态；标准 Java LCG，
  E2E 与 JVM 逐位一致 `sum=1008396158678580558 b=36`；无参构造保持 JNI；
  RegC 同 face 为 inert——Allocate+<init> 融合 NewObjectA 绕过 registry）。
  修复：fallback 扫描漏 randomRoots（ARETURN 逃逸未回退）；Random 结构定义
  未推入模块级声明（clang Cannot allocate unsized type）。
- 2026-08-21（round-2 收尾·补 intrinsic 2）：**包装类 shadow 切片**——Integer/
  Long/Short/Byte/Float/Double/Boolean/Character 的 valueOf/xxxValue 非逃逸用法
  直调 mj2x-crt 值结构（`%mj2x_int` 等单字段 struct；窄装箱 trunc、窄拆箱
  sext/zext；valueOf 结果寄存器经 Copy 联通逃逸分析，任何非匹配拆箱使用整根
  回退 JNI；Short/Byte 的 valueOf 用 JVM 实际窄描述符 (S)/(B)）。验证：单测
  EmitsWrapperBoxUnboxDirectCalls + E2E `sum=23 esc=7`（六种 wrapper 直调 +
  逃逸回退）。
- 2026-08-21（round-2 收尾·补 intrinsic）：**Math 纯函数切片**直调 mj2x-crt
  runtime（abs/min/max I/J/F/D、sin/cos/tan/sqrt/floor/ceil/pow、round、random；
  0 JNI、无 ExceptionCheck，`.ll` 模块级 declare）；LLVM `--compile`（pipe/文件）
  并入 mj2x-crt 源（rt_protect 专用路径单一提供，修 LNK2005 重复符号）；
  `--generate-cmake/--generate-xmake` 工程整树分发 crt。验证：mj2x_unittest
  594 过 / 3 skip；矩阵 66 例全绿（g5.abs 走 intrinsic）；Math E2E
  `abs=10 sqrt=5.0`（pipe/文件双模式）。
- 2026-08-21（round-2 收尾）：四主线落地——**indy 运行时解析**（自定义 bootstrap
  `indy:<idx>` → 全局 cache + bsm resolver，与 RegC 对齐——原文"超出 RegC"失实，订正）、**intrinsic**（cmp.nan_*）、
  **保护特性**（runtime-protection 检查点 + MBA/opaque/CFF 变换接入 llvm）、
  **测试矩阵**（LLVM 列 22 fixtures + protected_samples/llvm 三样本）。修复：
  ANEWARRAY 数组描述符悬垂（SSA arena 生命周期）、I2S 误按 i8 截断、check_cast/
  instance_of 参数缺 ptr 类型、mangle `$`→`_00024`。验证：mj2x_unittest 全绿 +
  mtest_set 25 过 + 矩阵 66 例全绿 + 样本 E2E（见 PR #870）。
- 2026-08-21（round-2 开工）：同步 roadmap 两处 stale（arraycopy intrinsic 已完成
  #819、方法内联矛盾）+ §4 LLVM 状态行（#864 扩面后）；GUI 配置工具设计完成
  （vite+vue，`doc_ai/gui_config_tool_design.md`）；记录整类下沉为 P3 待办（待做 F）。
- 2026-08-21：LLVM IR 后端二次扩面 11 项全部落地（PR #864）——**对象**（REF/字段
  读写/分配/类型检查）、**数组**（全 XALOAD/XASTORE/arraylength/多维）、**调用**
  （实例/外部 JNI CallXxxMethodA 分派）、**异常真实现**（放行异常区域 + 函数级
  dispatch label + 除零分发；修复此前异常方法静默回退字节码的假阳性）、**monitor**、
  **ldc String/Class**、**intrinsic 面对齐**（array.length/instanceof/monitor，
  invokedynamic 除外）、**窄类型 Z/B/C/S**、**浮点特殊字面量**、**工具链**
  （pipe/cmake/xmake）。NewStringUTF 槽位按 jni.h 成员数核对 163→167。单文件后端
  拆分五模块（llvm_ir_internal.hpp/validation/emitter/helpers/backend）。验证：
  mj2x_unittest 585 过 3 skip；mtest_set_unittest 25 过；真 native E2E
  `234-7-9-42-hi` 与 `9-20-5-18-15-wrapped`（多 catch/显式 throw/finally/除零重抛）。
- 2026-08-19：LLVM IR 后端基础款二次扩面——**整数除/取余**（udiv/urem 位模式
  等价规避 INT_MIN/-1 溢出 poison；除零经共享 throw-arithmetic helper 抛
  ArithmeticException：JNIEnv 表槽 FindClass=6/ThrowNew=14，helper 签名统一
  加 JNIEnv* 首参透传）、**递归/互递归**（预检 visiting 命中按描述接受，符号
  确定性生成不悬空）、**跨类静态调用**（目标须非 library + markedForCompile +
  可转换；符号用目标 owner）、**浮点→整数**（llvm.fptosi.sat，NaN→0/溢出饱和）。
  验证：LlvmIrBackendTest 11 例 + cli_test 3 例全绿；E2E 输出
  `17-83-14-2-3628800-2147483647-0-ae`（除法/余数/递归阶乘/f2i 饱和/NaN/除零
  异常捕获，clang/llc 双路径）。
- 2026-08-19：LLVM IR 后端基础款转正并扩面（v2.2 默认开启，`with_mj2x_llvm` 默认
  on）——受限标量子集：静态 int/long/float/double/void 纯计算（含移位，Java 掩码
  语义）+ 比较/分支/循环/switch + **同类内静态互调**（三态 resolve：已编译成功
  按描述解析 / 编译失败整链 fail-closed / 未编译递归预检含环检测）；对象/JNI/异常/
  monitor/内存/整数 div-rem/跨类·实例·递归调用保持 fail-closed。验证：
  LlvmIrBackendTest 7 例 + cli_test 3 例（clang/llc 双路径编译 → JVM 运行 E2E，
  互调+移位输出 17-83）全绿。
- 2026-08-19：mjdbg N-7 VMC4 ISA 恢复工具 v1 全闭环（PR #845）——五形态识别 +
  静态全恢复（跨实例复用率 100%），VMC4 低风险档强度归零、S2 启动条件满足；
  红队验证发现并修复 computed_goto label 定义（`::` → `:`）。
- 2026-08-16（语义审计修复 X1-X4,分支 `mj2x/semantics-hardening`）：
  - **X1（最严重）**：`LdcPreprocessor` 展开栈序缺陷——压栈 `Class,
    String(desc), MethodType, String(name)` 与 `findVirtual/findStatic` 的
    `(Class, String, MethodType)` 不符（多一参且 name/MethodType 颠倒）；
    field tag 沿用 3 参 MethodType 形态 desc、`findSpecial` 4 参只压 3 参、
    `findConstructor` 2 参多压 String——默认注册 + 遍历所有非库类，任何含
    ldc Handle 的类经 CLI 处理即 VerifyError。修复：按 tag 分派重写展开
    （含 field 类型的 Class 发射,primitive 走 `GETSTATIC <Box>.TYPE`）、
    MethodType 构造改用调用者 ClassLoader（保护态 jar 语义）、code_converter
    Handle ldc 由静默 null 改显式 unsupported marker。守门：迷你栈校验器单测
    （11 例,直接抓住压栈顺序/数量错误）+ 手构字节码 exec E2E（javac 不产
    ldc Handle,保护器产物才是真实来源）。
  - **X2**：RegC/LLVM 移位缺 JVM `&31/&63` 掩码（C UB / LLVM poison）,且
    `llvm_ir_validation.cpp` 注释错误声称等价；新增暴露 LLVM long 移位
    i32 计数未 zext 的非法定 IR。修复：掩码决策收敛到
    `codegen/jvm_semantics.hpp` 单源,RegC/LLVM 消费；新增四后端语义回归集
    （`g13.shift_out_of_range` 计数走参数防常量折叠 + `g12.string_compare`）,
    矩阵/LLVM 白名单/VMC4 E2E 全接入。
  - **X3（复核为误报）**：indy 单对象快路径 `stack[sp+1]`/`++sp` 净效应恰好
    正确（通用路径先 `sp -= argCount` 补偿）——但快路径白建 varargs 数组
    （热路径每调用多 2 次 JNI）且依赖 stale 槽。清理为参数 pop 前原槽捕获 +
    结果原位覆写,emit-shape 单测锁形态。
  - **X4（结构性）**：`staticizeStackText` 加字符串字面量/`//` 注释感知
    （indy bsm 元数据字符串内联进方法体,含 `sp -= N` 等模式会被静默改写并
    错位后续槽）；跨后端语义决策笔记沉淀 `doc_ai/cross_backend_semantics.md`
    （含 RegIR 语义显式化的后续路径）。
  - 次要：vmc4 过时"MVP 纯计算"注释与恒真死分支（误导日志）清理、新增
    `Vmc4JniSurfaceSample` protected sample（JNI helper 攻防面 + CLI round-trip
    测试）、删空 `platform.h`。
  - 验证：mj2x_unittest 全量 / mj2x_integration_test 全量（矩阵含新 fixture
    三列）/ mtest_set_unittest + mtest_set_integration_test 全绿。
- 2026-08-16（LLVM round-2 补充审查 Y1-Y4,同分支追加）：
  - **Y1**：indy 结果 REF dest 被 `unsupported type` 整方法拒绝（与注释
    "REF 直接透传"/validation 层矛盾,unboxReturnType REF 分支不可达）→ REF
    直接透传存槽;E2E 补 REF 返回站点（此前全是 (II)I 原语,零覆盖）。
  - **Y2**：bsm 失败（返回 null + pending）被送进 mj2x_llvm_call_object 的
    ThrowNew NPE 掩盖（StackC 对应路径是异常分发,移植降级）→ null 分支走
    emitExceptionDispatch;invokeWithArguments 之后统一 ExceptionCheck 再
    拆箱/透传。E2E 补恒抛 ISE 的 bsmFail 站点（BootstrapMethodError 解包
    归一化后基线/native 一致）。
  - **Y3**：indy 缓存失效路径 `br %cached_ok; ret null` → `br %resolve`
    重走解析（≈OOM 触发,意图写错）。
  - **Y4**：`Random.nextInt(bound)` 切片唯一 fail-open 角落——crt 对
    bound<=0 返回 INT32_MIN（无异常通道）而 JVM 应抛 IAE → RegC/LLVM 发射端
    预检（RegC 走 context throw + mayThrow 分发;LLVM 新增 throw_new prelude
    helper + 异常分发）,保留切片优化;RegC E2E 新用例 + LLVM 样本/E2E 覆盖。
  - 文档订正：capabilities/user_guide/dev_plan 三处"indy 运行时解析超出
    RegC"失实（regc_backend.cpp:1034 早有同功能 emitIndyBootstrapFunction）;
    native_compiler.hpp 过时"不需要 crt source pack"注释;param_registry 五个
    regc|llvm 共享参数 help 分组 RegC→RegC/LLVM;`$`→`_00024` JNI 符号
    mangling 变更与 alloca 已知限制记录入 capabilities;匹配表/逃逸分析/
    indy 解析三个共享层抽取候选补入 cross_backend_semantics.md §3.1。
- 2026-08-24（mj2x-rt 按后端分发 + `MJ2X_RT_BACKEND_*` 宏，#891 收尾，
  分支 `feat/mj2x-rt-backend-dispatch`）：生成工程从"整树全量分发"改为按
  后端分发挂载子集，分发面单一事实源 `embedded_rt.hpp rtFoldersForBackend`
  （StackC=stackc；RegC=common+regc；VMC4=common+vmc4；LLVM=common+regc+
  llvmir）。`rtBackendDefine` 唯一后端宏全链路注入（xmake 模板
  `@RT_BACKEND_DEFINES@` token / CMake `target_compile_definitions` / LLVM
  内联脚本 `-D` / `--compile` 直驱命令 MSVC `/D` 其余 `-D`）；
  `addCrtSources` 收集按后端过滤；C 后端 CMake 布局统一 `src/mj2x-rt/`。
  顺带修复三缺陷：llvmir 挂载未注册（生成 LLVM 工程 `mj2x_llvm_*` 未解析）、
  StackC/VMC4 `--generate-cmake` 不分发 rt（#894 后必然缺符号）、RegC/VMC4
  保护产物 `rt_protect/` 旧 include 路径（改名 common 后必坏路径，实编验证
  修复）。测试：四态分发+宏断言用例、1147 嵌入树外按后端字节比对、1037/1070
  CMake 断言更新、golden 同步。
- 2026-08-15：配置系统 P0+P1 完成（参数注册表 + schema/模板导出 + ConfigRegistryTest
  10 项，PR #846）。
- 更早记录已归档到 git log（见"已完成归档"索引）。

## 已完成归档索引

| 主线 | 归档位置 |
|---|---|
| 一~五（VMC4 可用版 / RegC 性能 / 异常精确化 / 反侦测 / 文档） | git log `feat/mj2x-vmc4-*`、`feat/mj2x-regc-perf`、`feat/mj2x-exc-exact`（PR #796）、`feat/mj2x-antidebug-agent`（PR #797）；`doc_ai/vmc4_multi_vm_design.md`、`doc/techniques/runtime_protection.md`、`benchmark/results/` |
| 六/七（#819 arraycopy、#820 步骤 1-5、#762/#844 变义、#843 trace surface 自验） | git log `feat/mj2x-arraycopy-intrinsic`、`feat/mj2x-vmc4-label-mutation`（PR #844）；`mj2x_integration_test` + `benchmark/baseline/regc-perf-baseline.json`；`vmc4_trace_surface_consistency_test.cpp` |
| 八~十一（#818 intrinsic、#827 program 粒度、#829 混淆） | git log PR #826；`vmc4_backend.cpp` 实例缓存 + 粒度矩阵测试；`reg_transform_passes.{hpp,cpp}` + `register_ir_test.cpp` |
| 十二（配置系统） | `mj2x-cli/src/param_registry.{hpp,cpp}`；`doc/config_file.md`；`doc_ai/config_system_design.md`（PR #846） |
| 十三（LLVM IR 后端） | `mj2x-core/src/mj2x/backend/llvmir/`（ed9a13881 实现，v2.2 转正 + 移位/互调/除零异常/递归/跨类/fptosi.sat 扩面）；`unittest/src/backend/llvm_ir_backend_test.cpp`（11 例）+ cli_test LLVM 3 例（含 clang/llc 编译 → JVM 运行 E2E）；`doc/user_guide.md` §5.4 |
| 十五（mj2x-llvmrt 运行时下沉，#894） | 分支 `refactor/mj2x-llvm-runtime-sink`（99f79487b…67dd8b6cc 十一批 + refeq 修复 c1ce74d79）；`mj2x-rt/src/llvmir/mj2x_llvmir_runtime.{h,c}`；`benchmark/results/2026-08-23-llvmrt-sink.md`；守卫测试 llvm_ir_backend_test.cpp（declare 形态锁） |
| VMC4 红队对抗（N-7 / S2 前置） | `mjdbg/mjdbg-isa-recovery/`（PR #845）；`doc_ai/vmc4_red_team_contract.md` §6 |
