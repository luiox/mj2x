# mj2x benchmark

`java_obfuscator_test_benchmark.ps1` runs a Java benchmark fixture as a full
native RegC benchmark. By default it compiles the source-only fixture under
`mj2x/test_resource/benchmark/java_obfuscator_style/src` into `test/` before
running, so the repository does not need to commit benchmark JARs.

It measures:

- baseline JVM run of `TEST.jar`;
- full `mj2x-cli --backend regc --generate-xmake` conversion;
- xmake build time for the generated native project;
- JVM run of the generated jar after embedding the xmake-built native library.

Example:

```powershell
pwsh -ExecutionPolicy Bypass -File mj2x/benchmark/java_obfuscator_test_benchmark.ps1 -Runs 5
```

To run an external compatibility fixture, pass `-InputJar` explicitly:

```powershell
pwsh -ExecutionPolicy Bypass -File mj2x/benchmark/java_obfuscator_test_benchmark.ps1 `
  -InputJar E:\canrad\reverse\jvm\obf\JavaObfuscatorTest\bench\bin\TEST.jar `
  -Runs 5
```

Results are written under `test/java_obfuscator_test/benchmark-current/`:

- `perf-results.csv`: per-run `Calc: Nms` and process time.
- `summary.txt`: baseline/regc averages and ratio.
- `regc-full-xmake/`: generated xmake project and native build artifacts.

The benchmark requires a JDK. The script adds `JAVA_HOME/bin` to `PATH` for
`java`, `javac`, and `jar` discovery.

## RegC vs os30(JNIC) 13-probe 对比（`jvm_obfuscator_test_benchmark.ps1`）

RegC 性能对比主路径：把 jvm-obf-tester（13 个探针 jar）用
`mj2x-cli --backend regc` 全量 native 化，与竞品 os30（JNIC 混淆产物）在同一
JDK 下对比各探针耗时。方法学遵循 performance_evaluation_spec：覆盖率审计
（javap 验证 ACC_NATIVE）+ 预热 1 轮 + `-Runs` 次取样取平均。

```powershell
# JDK21（默认输入 D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester.jar 与 _os30.jar）
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHome C:\Users\Canrad\.jdks\corretto-21.0.4 -Runs 3 -SkipBuild

# JDK17
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHome D:\sdk\java\jdk17 -Runs 3 -SkipBuild
```

产物与报告：

- RegC 产物：`test/jvm-obf-tester/jvm-obf-tester-regc.jar`（含嵌入的 native DLL）。
- 报告：`results/<日期>-jvm-obf-tester-os30.md`（总耗时 + 13-probe 明细 + 覆盖率审计）。
- 脚本内部处理 xmake 配置稳定性（issue #782）：`-P` 显式指定 + `-c` 重置，
  避免子项目配置破坏 root。

### 性能回归模式（#820 步骤 5）

`-Regression` 把 13-probe 基准变成带断言的回归测试：13-probe 全 Passed（功能门）+
native 覆盖率 ≥ 基线 × `-MinCoverageRatio`（默认 0.9）+ RegC total ≤ 基线 ×
`-MaxTotalRatio`（默认 2.0），任一失败退出码非零。跳过 os30 取样（不依赖竞品
jar）。基线文件：`baseline/regc-perf-baseline.json`（`-UpdateBaseline` 用本次
结果刷新基线）。

```powershell
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -Runs 3 -Regression -SkipBuild
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -Runs 5 -UpdateBaseline -SkipBuild
```

回归已并入测试集：`mj2x_integration_test` 的 `RegcPerfRegression` 用例（门控
`MJ2X_PERF_REGRESSION=1`，耗时 2-3 分钟不进默认回归）：

```powershell
$env:MJ2X_PERF_REGRESSION=1
xmake run mj2x_integration_test --gtest_filter="RegcPerfRegression.*"
```

测试分层与各组执行命令见 `mj2x/doc/regc_test_strategy.md`。
