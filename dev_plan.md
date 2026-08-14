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
| 七（#820 测试集） | 语义样本 Wave3 + 三后端矩阵 + jar 整体 E2E | 步骤 1-4 完成；5-6 待做 |
| 八（#818 intrinsic） | 用户自定义 intrinsic 三后端机制 | ✅ 完成（PR #826，已合并） |
| 九（#827 VMC4 粒度） | program 粒度（全程序共享 VM） | ✅ 完成（见 PR） |
| 十（#829 混淆借鉴） | Cloakwork 借鉴项：RegC 可选混淆增强（CFF 死块/state 加密/深 MBA/opaque-零），全部默认关闭 | ✅ 完成（见 PR；默认产物零变化） |
| 十一（混淆默认关） | RegC 基础三项（MBA/opaque/CFF）默认关闭；`--regc-obfuscate` 显式开启；#829 开关联动基础项 | ✅ 完成（见 PR；RegC 默认产物语义直译零混淆） |

## 待做项

### A. RegC 性能（P1）

- **方法内联 pass（RegIR 层）**：1.1 基线数据已就绪；直调默认关闭后 JNI 开销成为
  热点，内联收益放大。建议独立分支 feat/mj2x-regc-inline；验收 = benchmark 加速比 +
  全量单测。

### B. VMC4 升级档（P2）

- **period 语义变义**（#762 升级档）：低风险档已达成；mjdbg 对抗验证闭环就绪后开
  feat/mj2x-vmc4-mutation；红队契约扩展同批。

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

## 验证命令

```bash
xmake f --with_mj2x=y --with_mtest_set=y --with_tests=y -y   # 全量配置（勿用 -P 子项目，issue #782）
xmake build mj2x_unittest && xmake run mj2x_unittest --gtest_brief=1
xmake build mtest_set_unittest && xmake run mtest_set_unittest
```

> 性能数据必须先做覆盖率审计（见 `performance_evaluation_spec.md`）。

## 日志

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
