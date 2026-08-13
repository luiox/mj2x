# RegC 测试分层与执行命令

> 对应 issue #782「RegC 开发迭代提速：benchmark 快速脚本 + 测试分层 + xmake 配置稳定性」。

RegC 单测按耗时分成三层。改动后按影响面选组，收尾/CI 跑全量。
所有命令在仓库根目录执行（xmake 的 `-P <repo>` 参数在本机 xmake 版本上有解析问题，
直接以仓库为工作目录运行即可；先 `xmake f -c -P <repo> -p windows -m release --with_mj2x=y --with_tests=y`
重置配置，再 `xmake build <target>`）。

## 三层分组

| 层 | 测试套件（gtest filter） | 耗时 | 何时必跑 |
|---|---|---|---|
| 快组（毫秒级） | `RegCTest.*`、`RegCIntrinsicTest.*`、`RegCIntrinsicRegistryTest.*`、`RegCCompareIntrinsicTest.*`、`RegCLdcClassTest.*` | 秒级 | 每次 RegC 改动必跑 |
| 中速组（IR/emitter，不编译生成代码） | `RegCBackendTest.*`、`Vmc4BackendTest.*`、`Vmc4ComputedGotoEmitTest.*` | 十秒级 | 涉及 IR/emitter/flow 时跑 |
| 慢速组（编译生成代码 + 启动 JVM） | `RegCBackendE2E.*`、`RegCBackendE2EHarnessTest.*`、`RegCSafeEnvE2E.*`、`CliTest.*` | 约 90 秒 | 收尾/提交前/CI |

## 执行命令

```powershell
# 重置配置（root 配置被 -P <子项目> 破坏后必做，见下）
xmake f -c -P D:\WorkSpace\ai\morpher -p windows -m release --with_mj2x=y --with_tests=y

# 构建
xmake build mj2x-cli
xmake build mj2x_unittest

# 快组
xmake run mj2x_unittest --gtest_filter="RegCTest.*:RegCIntrinsicTest.*:RegCIntrinsicRegistryTest.*:RegCCompareIntrinsicTest.*:RegCLdcClassTest.*"

# 中速组
xmake run mj2x_unittest --gtest_filter="RegCBackendTest.*:Vmc4BackendTest.*:Vmc4ComputedGotoEmitTest.*"

# 慢速组
xmake run mj2x_unittest --gtest_filter="RegCBackendE2E.*:RegCBackendE2EHarnessTest.*:RegCSafeEnvE2E.*:CliTest.*"

# 全量
xmake run mj2x_unittest
```

## 快速迭代主验证路径（13-probe 对比 os30）

```powershell
# 一条命令：RegC 全量转换 -> 编译 native -> 嵌入 -> 预热 1 轮 + 3 轮取样 -> 对比 os30，报告落盘
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHome C:\Users\Canrad\.jdks\corretto-21.0.4 -Runs 3 -SkipBuild
```

见 `mj2x/benchmark/README.md`。

## xmake 配置稳定性（issue #782 §3）

- 现象：`xmake f -P <子项目目录>`（如 native_project）会破坏 root 配置，之后
  `xmake build mj2x-cli` 报 `invalid argument: mj2x-cli`（target 列表失效）。
- 规避：脚本/文档统一用 `xmake f -c -P <repo> ...` 重置后再构建；
  benchmark 脚本内部已按此处理（`-P` 显式指定 + 每次 `-c` 重置）。
- 本机注意：`xmake -P <repo> build <target>` 形态偶发解析失败，直接以仓库为
  工作目录运行 `xmake build <target>` 更可靠。
