# dev_plan：mj2x 施工计划

> 本文件只保留**待做项**与**状态总览**；已完成阶段的详细落地记录已归档到
> 对应 PR / git log（见文末"已完成归档"索引）。依据 issue #787 §3（mj2x 部分）。

## 状态总览

| 主线 | 内容 | 状态 |
|---|---|---|
| 二（VMC4 可用版） | Phase 2.0-2.E：REF/调用/字段/数组/异常/intrinsic/收口（转化率 100%） | ✅ 完成（PR #796 前各 PR，详见 git log） |
| 一（RegC 性能） | 1.1 基准套件（13-probe + JNIC 对比）；1.2 位操作 intrinsic / 检查稀疏化 / resolved 双槽缓存 | ✅ 1.1 完成；1.2 部分完成 |
| 三（异常精确化） | 保护区判定真源改为指令序号区间 | ✅ 完成（feat/mj2x-exc-exact） |
| 四（反侦测体系） | antidebug / enforcement / rt_protect 注入 / agent 收集 | ✅ 完成（feat/mj2x-antidebug-agent） |
| 五（文档整理） | capabilities / roadmap / user_guide 等同步 | ✅ 完成（随各主线） |
| 六（剩余 issue） | 方法内联 / arraycopy 更宽 intrinsic / VMC4 升级档 | arraycopy 已完成（#819）；其余待做（见下） |
| 7（#820 测试集） | 语义样本 Wave3 + 三后端矩阵 + jar 整体 E2E | 步骤 1-5 完成；6 待做（mj2x 侧自验闭环已并入，mjdbg 对抗工具端未落地） |
| 八（#818 intrinsic） | 用户自定义 intrinsic 三后端机制 | ✅ 完成（PR #826，已合并） |
| 九（#827 VMC4 粒度） | program 粒度（全程序共享 VM） | ✅ 完成（见 PR） |
| 十（#829 混淆借鉴） | Cloakwork 借鉴项：RegC 可选混淆增强（CFF 死块/state 加密/深 MBA/opaque-零），全部默认关闭 | ✅ 完成（见 PR；默认产物零变化） |
| 十一（混淆默认关） | RegC 基础三项（MBA/opaque/CFF）默认关闭；`--regc-obfuscate` 显式开启；#829 开关联动基础项 | ✅ 完成（见 PR；RegC 默认产物语义直译零混淆） |
| 十二（配置系统） | 参数注册表单源 + 全量参数配置化 + JSON Schema 导出（GUI 配置工具路径） | ✅ 完成（P0+P1：注册表驱动 CLI/config/help/schema 四处；`--dump-config-schema`/`--dump-config-template`；ConfigRegistryTest 10 项） |

## 待做项

### A. RegC 性能（P1）

- **方法内联 pass（RegIR 层）**：1.1 基线数据已就绪；直调默认关闭后 JNI 开销成为
  热点，内联收益放大。建议独立分支 feat/mj2x-regc-inline；验收 = benchmark 加速比 +
  全量单测。**已完成**（#820 步骤 5 后：SSA 方法内联默认开启，`--regc-no-inline`
  关闭；同环境对照数据见 benchmark 记录）。

### B. VMC4 升级档（P2）

- ~~period 语义变义（#762 升级档）~~ **已完成**（case 标签周期轮换，见日志
  「主线四完成（#762）」；强度实测待 mjdbg 对抗侧）。
- **红队前置已就绪（2026-08-15）**：mjdbg N-7 VMC4 ISA 恢复工具 v1 交付
  （`mjdbg/mjdbg-isa-recovery/`，PR #845）——五 dispatcher 形态识别 +
  字节集合枚举（canonical/别名/drift unmap）+ handler 语义标注（真实产物
  20/20 对映）+ trace-surface 对账。实测：switch_table+drift2/16 与
  if_nest+drift4/32 不同 seed 样本均静态全恢复、跨实例复用率 100%
  （§4.1 复用率 = 100% 意味着低风险档的"跨实例可复用"归零基准已达成，
  升级档 S2 的启动条件满足）。
- **N-7 全闭环（2026-08-19）**：动态对拍（native/JNI trace 互证）与
  weakness report 通路接入已完成；period 语义变义（#844）对抗（label 表
  提取 + case 常量反查 + caseLabelPerms 对账）与 computed_goto 真实产物
  闭环（含生成侧 `::` label 修复）均完成——五形态真实产物 + 动态对拍
  全部就绪，可开 feat/mj2x-vmc4-mutation；红队契约扩展同批。

### C. mj2x 自建测试集（#820，步骤 5-6）

| 步骤 | 内容 | 状态 |
|---|---|---|
| 1 | 需求调研：三家测试集（huzpsb/bludnov/ttvar）用例清单对比 | ✅ 完成 |
| 2 | 设计：mtest_set 目录结构 + 断言器 + 矩阵配置 | ✅ 完成 |
| 3 | 首批用例：语义核心面（Wave1-3：G1-G15） | ✅ 完成 |
| 4 | 三后端矩阵：mtest_set 18 fixture × StackC/RegC + 用户 intrinsic 三后端 E2E + jar 整体 E2E（三后端） | ✅ 完成 |
| 5 | 性能回归自动化（benchmark 13-probe 扩展并入测试集） | ✅ 完成（脚本 `-Regression` 断言 + `mj2x_integration_test` 门控用例 `RegcPerfRegression`；基线 `benchmark/baseline/regc-perf-baseline.json`） |
| 6 | mjdbg 对抗闭环（每用例 expected trace surface + weakness report） | 待做 |

### D. 记录不阻塞（P3）

- VMC4 检查点指令级异常匹配（javac 字节码下块粒度==指令粒度，无实际缺口）
- agent 特征深度检测（javaagent MANIFEST 扫描 / JDWP 探测）
- 异常表多 region 覆盖同一块的匹配顺序测试扩展
- mj2x-runtime C++ 版与 crt C 版检测面统一

### E. 配置系统（P1，主线十二）

- **参数注册表 + 全量配置化 + schema 导出**：✅ **已完成**——`param_registry`
  （45+ 参数 ParamDef 单源，驱动 CLI 解析/config/help/schema 四处）；
  `--dump-config-schema`（draft-07，GUI/VS Code/ajv 消费）+ `--dump-config-template`；
  config_file 重写注册表驱动（旧 15 键兼容）；ConfigRegistryTest 10 项全绿。
  设计文档 `doc_ai/config_system_design.md` 已同步为实现态。
- GUI 配置工具（P2，独立）：消费 schema 渲染表单，输出配置文件。

## 验证命令

```bash
xmake f --with_mj2x=y --with_mtest_set=y --with_tests=y -y   # 全量配置（勿用 -P 子项目，issue #782）
xmake build mj2x_unittest && xmake run mj2x_unittest --gtest_brief=1
xmake build mtest_set_unittest && xmake run mtest_set_unittest
```

> 性能数据必须先做覆盖率审计（见 `performance_evaluation_spec.md`）。

## 日志

- 2026-08-15：**主线十二（配置系统）P0+P1 实现完成**——参数注册表
  `param_registry.{hpp,cpp}`（45+ 参数 ParamDef 单源：规范 ID/CLI flag/JSON 键/
  类型/枚举/范围/默认值/描述/分组/依赖）；main.cpp 手写解析链与 help 删除 →
  查表 + 分组 help；`--dump-config-schema`（JSON Schema draft-07，
  additionalProperties=false + x-requires）与 `--dump-config-template`（JSONC）；
  config_file 重写注册表驱动（旧 15 键兼容）；新增 ConfigRegistryTest 10 项
  （schema 键全覆盖/description 同源计数/模板全键/未知键·类型·枚举错误定位/
  config backend 驱动交叉校验/CLI 覆盖 config/多族参数一次配置）。验证：
  ConfigRegistryTest 10/10 + mj2x_unittest 全量 + mtest_set_unittest 25 passed；
  默认产物不变（既有 cli_test 一致性用例全绿）。
- 2026-08-15：**主线十二（配置系统）设计完成**——竞品调研（jvmtp properties
  域前缀平铺 + obfuscation 模块 opt-in/作用域；skidoni YAML 分组块/注释即关闭）
  结论：JSON + 参数注册表单源 + JSON Schema 导出（GUI 工具最短路径）。交付：
  `doc_ai/competitor_config_reference.md` + `doc_ai/config_system_design.md`
  （§4.2 全量 50+ 参数映射表、schema 设计、合并/校验语义、P1 实现方案与验收）。
  文档同步：`doc/config_file.md`（现状+缺口）、user_guide §4、doc_ai README。
- 2026-08-15：**主线四完成（#762 升级档：period 语义变义）**——case 标签周期
  轮换（`d*K + caseLabelPerm_[d][canonical]`，跨变体不相交 + case 共享 body）；
  全 dispatcher 覆盖（switch/funcptr/computed_goto/if_nest/state）；describe 新增
  caseLabelPerms 观测面（红队契约升级）；单测 10/10 + e2e 80/80（5 dispatcher ×
  16 用例 drift 全开语义保真）+ 无 drift 零回归。强度实测（变义强度模型）待
  mjdbg 对抗侧（vmc4_red_team_contract.md §7 自验闭环已纳入观测面）。
- 2026-08-15：**#820 步骤 6 mj2x 侧闭环（trace surface 自验）**——新增
  Vmc4TraceSurfaceConsistencyTest 3 项：describe↔产物逐字节核对（含 drift 变体
  unmap 全排列双射 + 轮转真实性）、跨实例语义命中率 2.50%（随机期望 ~5%，
  阈值 20%——§4.1 实例独立判据的自动化替代）；mjdbg 对抗工具就绪后作为
  §4 量化方法的前置基线（vmc4_red_team_contract.md §7）。
- 2026-08-15：**主线七步骤 5（#820 性能回归自动化）**——benchmark 13-probe 扩展
  `-Regression` 回归模式（13-probe 全过 + 覆盖率 ≥ 基线×0.9 + total ≤ 基线×2.0，
  失败 exit 非零；`-UpdateBaseline` 刷新基线）+ 基线文件
  `benchmark/baseline/regc-perf-baseline.json` + `mj2x_integration_test`
  `RegcPerfRegression` 门控用例（`MJ2X_PERF_REGRESSION=1`）；修复
  `Get-ProbeTimes` Status 取值的既有解析 bug。
- 2026-08-15：**sepacker 对接预留（主线第 6 项接口）**——`--native-lib-pack-hook`：
  嵌入前外部处理钩子（`{in}` 占位符 = 编译产物路径，就地改写，非零退出失败）；
  顺序 编译→hook→可选加密（`--native-lib-key`）→嵌入；加壳 DLL 走同一 `.enc`
  存储形态，运行时链路不变（E2E：hook 执行 + {in} 替换 + 产物照常运行 42）。
- 2026-08-15：**主线十（#829 Cloakwork 借鉴项）**——RegC 可选混淆增强：CFF 恒假死块
  注入（barrier 防 DCE，条件依赖运行时 state）+ 派生随机 state 码 + 状态异或、深 MBA
  （De Morgan/进位链 + barrier 防折叠 + per-site 变体）、opaque-零恒等变体
  （demorgan/shift-pair/or-dec）。全部默认关闭（新 CLI 开关：`--regc-cff-dead` /
  `--regc-cff-state-encrypt` / `--regc-mba-deep` / `--regc-opaque-zero` /
  `--regc-transform-seed`）；默认产物零变化，同 seed 同产物。
- 2026-08-15：**主线十一（混淆默认关）**——基础三项 MBA/opaque/CFF 默认关闭
  （RegC 语义直译、最快路径）；新增 `--regc-obfuscate` 显式开启基础三项；
  #829 增强开关联动对应基础项（mbaDeep→MBA、opaqueZero→opaque、cff-*→CFF）。
- 2026-08-15：**主线八完成（PR #826 已合并）**——用户 intrinsic 三后端（#818）+
  mtest_set Wave3（#820 首批）。
- 2026-08-15：**主线九（#827）**——VMC4 granularity=program（全程序共享 VM 实例），
  三粒度（class/method/program）测试锚定；jar 级整体 E2E（三后端 × 多类 jar 语义对比）。
- 2026-08-14：issue 梳理 + 文档系统梳理（readme/doc_ai/roadmap/architecture/user_guide/
  capabilities/mvp_release_scope/regc_backend_readme）；#816 直调默认关闭合入。
- 2026-08-13：VMC4 可用版主线 2.0-2.E 收口（转化率 25%→100%）；RegC 性能 1.1 完成 /
  1.2 部分；异常精确化（feat/mj2x-exc-exact，PR #796）；反侦测体系（feat/mj2x-antidebug-agent，
  PR #797）；回归 442/442 + 36/36。
- 2026-08-13：建立本计划（替换已完结的 RegC 模板引擎旧计划）。

## 已完成归档索引

| 主线 | 归档位置 |
|---|---|
| 二（VMC4 2.0-2.E） | git log `feat/mj2x-vmc4-*`；`mj2x/doc_ai/vmc4_multi_vm_design.md`；`mj2x/doc_ai/capabilities.md` §4 |
| 一（RegC 性能） | `mj2x/benchmark/results/2026-08-13-jvm-obf-tester-os30.md`；git log `feat/mj2x-regc-perf` |
| 三（异常精确化） | git log `feat/mj2x-exc-exact`（PR #796）；`doc_ai/` 异常精确化设计 |
| 四（反侦测） | git log `feat/mj2x-antidebug-agent`（PR #797）；`mj2x/doc/techniques/runtime_protection.md` |
| 五（文档） | `mj2x/doc/*.md`、`mj2x/doc_ai/capabilities.md` |
| 八（#818 intrinsic） | `mj2x/doc/user_intrinsic.md`；git log PR #826；`user_intrinsic_test.cpp` / `user_intrinsic_e2e_test.cpp` |
| 九（#827 program 粒度） | `vmc4_backend_test.cpp` 粒度矩阵测试；`vmc4_backend.cpp` 实例缓存 |
| 十（#829 混淆借鉴） | `reg_transform_passes.{hpp,cpp}`（Cloakwork 借鉴项 P0）；`register_ir_test.cpp` RegTransformPassTest 扩展 |
