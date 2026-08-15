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

- 配置系统 P0+P1 已完成（见归档索引）。剩余：GUI 配置工具（消费
  `--dump-config-schema` 渲染表单、输出配置文件，独立项目；协议即 schema）。

## 验证命令

```bash
xmake f --with_mj2x=y --with_mtest_set=y --with_tests=y -y   # 全量配置（勿用 -P 子项目，issue #782）
xmake build mj2x_unittest && xmake run mj2x_unittest --gtest_brief=1
xmake build mtest_set_unittest && xmake run mtest_set_unittest
```

> 性能数据必须先做覆盖率审计（见 `performance_evaluation_spec.md`）。

## 日志

- 2026-08-19：mjdbg N-7 VMC4 ISA 恢复工具 v1 全闭环（PR #845）——五形态识别 +
  静态全恢复（跨实例复用率 100%），VMC4 低风险档强度归零、S2 启动条件满足；
  红队验证发现并修复 computed_goto label 定义（`::` → `:`）。
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
| VMC4 红队对抗（N-7 / S2 前置） | `mjdbg/mjdbg-isa-recovery/`（PR #845）；`doc_ai/vmc4_red_team_contract.md` §6 |
