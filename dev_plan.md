# dev_plan：mj2x 施工计划

> 本文件只保留**待做项**。已完成的工作不写日志——git log 即记录；重要的机制与契约写进 doc_ai 设计文档与 capabilities。

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
- 枚举合成方法（values/valueOf）站点 intrinsic 登记只发生在枚举类自身
  转换时点：调用方类先于枚举类编译时登记未发生，调用点退通用 JNI 路径
  （fail-closed，仅损优化不损正确性；根治需编译序无关的登记时机）。

### D. 配置系统 GUI 工具（P2）

- 配置系统 P0+P1 已完成（见归档索引）。设计已完成
  （`doc_ai/gui_config_tool_design.md`：vite+vue 方案、schema 驱动渲染、
  联动/校验/导出/可选 sidecar 执行）。剩余：P1 实现（表单渲染 + 校验 + 导出）。

### F. 整类下沉（P3，记录不阻塞）

- roadmap §2 表格行已列（算法密集类整体 native 化，边界保留 JNI）；GUI 设计已
  预留"方法/类选择"扩展点（`gui_config_tool_design.md` §6）。条件：无反射、无
  动态类加载、intrinsic 覆盖充分；先做单类试点再铺开。

### H. indy 原生派发 backend 侧合流（RegC 面已落地）

- 管线层判定原型 + 设计文档已落地（`doc_ai/indy_native_dispatch_design.md`
  §2 判定树 / §3 缓存 / §4 回退 / §6 接口提案）。
- P1（判定载荷单源化）、P2（字段/record 直派）、P3（TU 级直派缓存 + 惰性
  解析）已落地；P4（运行期破解）保持提案，随合流批评估。
- 剩余：P5 stackc F1/F2 分支、P7 vmc4 编码前改写式、P10 llvm 模块级缓存分派
  ——三后端消费同一 MIR 载荷，发射面各自适配。
- 注：P6（stackc bootstrap 结果缓存）经核已有等价物
  （stackc_mir_invoke.cpp indyTargetCache，static jobject per call-site），
  **无需实施**（设计文档写作时点误判，2026-09-18 审查订正）。
- 注：F1 直派 engagement 当前仅覆盖「indy 描述符即字段形态」站点（javac 产物
  不会出现，现实站点走 helper 零回归）——收益兑现需 D1 静态求值或 bootstrap
  白名单扩展（发射面无需再动），与 P4 同批评估。
- 注：Random 预检 registry 标志位化已单独落地，不再含入合流批。

### H-2. 三后端共享层抽取：排期结论

**结论：不在与发射面改动同窗口实施；indy 解析载荷单源化（P1）已作先导件落地。**

- 依据：四个子批均需设计决策 + 全量回归，且抽取面与进行中的发射面改动
  高度重叠，churn 窗口叠结构性抽取语义风险最大；indy 解析载荷单源化
  （P1）已作为先导件落地，其余待合流批完成后启动。
- 触发条件（重排期）：本 PR 合入 + indy P5-P7/P10 三后端消费合流完成后，
  按映射表 → RegisterNatives → indy 解析收尾 → 多维数组 的子批顺序独立
  排期，每子批独立提交 + 全量回归。

### G. 全线审查遗留（待评估/待实施）

- rt_protect Linux rwx 检测疑似对宿主 JVM CodeCache（rwxp）误报（force 下
  自毁）；需 Linux 真机验证后按 safe-env 模式排除 libjvm 自身映射——现有
  exec 级测试全在 Windows，掩盖此问题。
- RegC 进程级缓存并发残留：jclass 与 methodID 两写间的短暂错配窗（需原子对
  或 seqlock 才能闭）、字符串字面量 32 槽缓存的瞬时覆盖竞态（越界安全，
  维持既有注记）；并发契约见 `mj2x_regc_runtime.c` 文件头。
- CLI 外部进程调用（编译器/xmake/pack hook）无超时（需 libca 扩展或手写
  并发排空框架）。
- 三后端共享层抽取（结构性最大项）：JVM 类型格点映射表（10+ 张平行）、
  RegisterNatives 基建、indy bootstrap 运行时解析（3 份）、多维数组分配
  （3 份）；按 H-2 触发条件独立排期。
- backend.hpp 收拾：Kind::BINARY 死枚举、llvmir `.ll` 挂 C_SOURCE 语义错位、
  finalize 无错误通道（三后端失败行为不一致）、接口默认方法能力边界
  （MarkAll 下产出带 ACC_NATIVE 的接口方法 → ClassFormatError）。
- RegC intrinsic 判定热点：每条 Call 重复 match 6-10 次（含多次 std::string
  构造与全表线性扫），应函数级一次算好 decision；registry 视图不统一
  （analyze 走 builtins、emit 走用户 registry，覆盖判定有漂移风险）。
- 性能遗留：SSA 构建 O(n²)（fallthroughBlock 线性扫 / findBlockByLabel /
  reaching_def 无邻接表）；JAR 输出每条目全量拷贝；llvmir
  GlobalStringTable::intern 线性查。
- CLI 无下界整数参数负值回绕。
- 死代码清理批次：MONITOR_EXIT 死枚举、cli dispatcher 别名死分支。
- 约定基调：std::string vs Utf8String、int vs i32 在 CLI/codegen/后端三层
  各自为政，需在 capabilities/AGENTS 层面定一次豁免边界。

### J. 待办池（2026-10-02 冻结收官盘点；后续开卡从这里取）

**工程挂账（小项，均可独立成卡）**

1. llc / clang++ SKIP 门探测面收口（2026-10-06 调研收窄：原"cl/gcc/zig 列
   统一注入面"不成立——三者非 clang 家族，不属 clangPathPrelude 注入面）。
   仅 LLVM 标准安装未上 PATH / Homebrew llvm 未链接环境受益，已建档环境
   增益为零且需动 #1191 门禁双键基线——待此类环境入档再开卡。
2. isGeneratedRuntimeClass 用户手写 `Lambda$N` 理论误判：名字识别法对用户
   自定义同名类的理论冲突，低概率，记录在案。
3. EnumSemCheck E2E 门禁力：现类序使内联不发生，E2E 对 #1188 类内联缺口无
   门禁力（SSA 单测是真门）——caller-first 排序变体可解锁 E2E 防护。
4. N4/N6 dev_plan 历史日志数字微误：PR body 已勘正，本文件日志正文回填
   （低优先级，纯记录）。
5. #1194 P3-4：模板 glob 收编 `src/generated/` 下任意 .c/.cpp——目录为 CLI
   专有无现风险；若未来开放手工放置需加来源约束。
6. IndyBootstrapPlanTest.WalkEdgeShapes 非确定性翻车（2026-10-06 T3 评审
   发现）：`Type::getArgumentTypes("")` 对空描述符自 `byte_at(1)` 起越界读
   零长串，结果随栈残留字节浮动，同一二进制逐次运行可红可绿（实测 5 连跑
   2 红），全量门禁对任意二进制布局变化噪声化。消除判据：libs/libjbytecode
   的 type.cpp `getArgumentCount`/`getArgumentTypes` 入口加 `length < 2`
   边界检查显式抛 invalid_argument，测试改用合法最小描述符；动共享层需
   跨线回归。

**产品/机器决策项（需拍板）**

7. benchmark 驱动器默认输入路径：`-InputJar` 可覆盖，但默认落 Downloads——
   改仓库样本路径还是提升为配置项。
8. CI runner 安装 llvm/clang（#1122 方向 1）：unittest skip 地板 4→2
    （仅剩 zig 系），integration LLVM 列 28 例在 CI 转实跑。
9. RegC CRT DLL 跨运行缓存（~12-20ms/次）：需解密 DLL 落盘持久化，与
    「不落明文 DLL」威胁模型冲突——接受现状或重评威胁模型。
10. POSIX 600 文件权限：spool/临时文件 `Files.createFile` + posix attrs
    收紧（Windows 无影响，POSIX 加固项）。

**编排层**

11. 锁协议 v2 沉淀：mkdir 抢锁 + owner.txt + 验 own 后释放、tests/gates
    不持锁、`xmake build <target>` 单 target 单命令——落 doc_ai 归档页，
    供后续 sub agent 会话直接引用。
12. test_strategy.md 重写：计划期表格（落地波次列）大多已落地，删表留分层
    策略与覆盖率审计方法学。
13. 代码注释与 Doxygen 组名里的 R3/R6 轮次代号退役（`regc_r3_abi` 等），
    文档侧已先行（本 PR）。

## 验证命令

```bash
xmake f --with_mj2x=y --with_mtest_set=y --with_tests=y -y   # 全量配置（勿用 -P 子项目，issue #782）
xmake build mj2x_unittest && xmake run mj2x_unittest --gtest_brief=1
xmake build mtest_set_unittest && xmake run mtest_set_unittest
```

> 性能数据必须先做覆盖率审计（见 `performance_evaluation_spec.md`）。
> 注意：llvm sample E2E 外呼 `mj2x-cli.exe`，验证前须确认 CLI 已重建
> （`xmake build mj2x-cli`），否则跑的是旧发射器（#894 收尾时踩过）。
> unittest/integration 的 skip 数含环境性 skip（本机无 clang/zig 时 LLVM 列
> exec 编译与性能门控整列跳过）——跨机对数时 skip 差异属环境差异，不构成
> 回归判据；对数只看 passed/failed。

