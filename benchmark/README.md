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
JDK 下对比各探针耗时。方法学如下（#887 修尺子）：

- 覆盖率审计先行（javap 验证 ACC_NATIVE），未附覆盖率的性能数据不予采信；
- **预热 `-WarmupRuns` 轮（默认 3，不计入统计）**；
- **计量 `-Runs` 轮（默认 9）**，以**中位数为主指标**，同时输出
  min/max/均值/标准差/CV%；
- **任一样本 CV% 超过 `-MaxCvPercent`（默认 15）→ 报告置顶"高方差警告：结果
  存疑"**，对应行 CV 列标 ⚠——#887 中单次 72.7 vs 71ms 的"假打平"即被高方差
  噪声误导，中位数口径下 RegC 实际落后 os30 final ~1.6x；
- **os30 基线缺失时 fail-closed**：对比模式直接报错并给出放置路径说明
  （默认 `D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar`，已知
  final 版样例 `C:\Users\l1502\Downloads\jvm-obf-tester_final.jar`），不静默
  跳过、不拿旧数据对比；报告同时记录 os30 产物的体积与时间戳，防陈旧基线
  混入结论。

```powershell
# JDK21（默认输入 D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester.jar 与 _os30.jar）
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHome C:\Users\Canrad\.jdks\corretto-21.0.4 -SkipBuild

# 显式指定轮数/预热
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -Runs 9 -WarmupRuns 3 -SkipBuild

# 双 JDK 参数化（#887）：逐 JDK 计量，输出跨 JDK 对比表；同一会话顺序完成
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHomes "C:\jdk-21","C:\jdk-25" -SkipBuild

# 方差归因模式（#887）：每 JDK×产物额外 1 轮冷启动，采集 GC/类加载/JIT 日志到
# test/jvm-obf-tester/attribution/，并记录各 JDK 默认收集器（default-gc.txt）
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHomes "C:\jdk-21","C:\jdk-25" -Attribution -SkipBuild

# RegC 直调口径（#887 基线）：转换时加 --regc-direct-calls
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -RegcDirectCalls -SkipBuild
```

产物与报告：

- RegC 产物：`test/jvm-obf-tester/jvm-obf-tester-regc.jar`（含嵌入的 native DLL）。
- 报告：`results/<日期>-jvm-obf-tester-os30.md`（环境/方法论 + 高方差警告块 +
  覆盖率审计 + 跨 JDK 对比表（多 JDK 时）+ 每 JDK 总耗时中位数对比表（含
  min~max/p95/CV%）+ 13-probe 中位数明细 + 方法论说明），与既有 results/*.md
  的章节结构兼容，列口径从"平均"升级为"中位数"。
- CSV：`results/<日期>-jvm-obf-tester-os30-runs.csv`（逐轮原始样本，long 格式）
  与 `*-stats.csv`（统计汇总：中位数/min/max/p95/均值/标准差/方差/CV%）。
- 脚本内部处理 xmake 配置稳定性（issue #782）：`-P` 显式指定 + `-c` 重置，
  避免子项目配置破坏 root。

### 性能回归模式（#820 步骤 5）

`-Regression` 把 13-probe 基准变成带断言的回归测试：13-probe 全 Passed（功能门）+
native 覆盖率 ≥ 基线 × `-MinCoverageRatio`（默认 0.9）+ **RegC total 中位数** ≤
基线 × `-MaxTotalRatio`（默认 2.0），任一失败退出码非零。CV% 超阈值时回归仍按
门值判定，但控制台与报告都会显式提示"结果存疑，建议复测"。跳过 os30 取样（不
依赖竞品 jar）。基线文件：`baseline/regc-perf-baseline.json`（`-UpdateBaseline`
用本次结果刷新基线；#887 起基线主指标迁移为中位数，meta 记录
`metric="median"`/`warmupRuns`/`runs`，`meta.jdk` 修复为纯文本版本串）。

```powershell
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -Runs 9 -Regression -SkipBuild
pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -Runs 9 -UpdateBaseline -SkipBuild
```

回归已并入测试集：`mj2x_integration_test` 的 `RegcPerfRegression` 用例（门控
`MJ2X_PERF_REGRESSION=1`，耗时 2-3 分钟不进默认回归）：

```powershell
$env:MJ2X_PERF_REGRESSION=1
xmake run mj2x_integration_test --gtest_filter="RegcPerfRegression.*"
```

测试分层与各组执行命令见 `mj2x/doc/regc_test_strategy.md`。
