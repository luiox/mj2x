# jvm_obfuscator_test_benchmark.ps1 — RegC vs os30(JNIC) 13-probe 性能对比基准
#
# 用途：把 jvm-obf-tester（13 个探针 jar）用 mj2x-cli --backend regc 全量 native 化，
# 与竞品 os30（JNIC 混淆产物）在同一 JDK 下对比各探针耗时，产出 markdown 报告。
# 依据：issue #787 §3.1（RegC 性能基线）+ #778（热循环性能对比）+ #887（方法论升级）。
#
# 方法学（performance_evaluation_spec.md 强制项 + #887 修尺子）：
#   1. 每次对比前先做覆盖率审计（javap 验证 ACC_NATIVE + 无 Code 属性），
#      未附覆盖率的性能数据不予采信；
#   2. 预热 WarmupRuns 轮（默认 3，不计入统计）；
#   3. Runs 轮计量取样（默认 9），以中位数为主指标，
#      同时输出 min/max/均值/标准差/CV%；
#   4. 任一 CV% 超过 MaxCvPercent（默认 15）→ 报告显式标记"高方差，结果存疑"，
#      防止单次/低样本均值造成"假打平"结论（#887：72.7 vs 71ms 即被噪声误导）；
#   5. os30 基线缺失时 fail-closed：给出明确错误与放置路径说明，
#      不静默跳过、不拿旧数据对比；
#   6. 输出报告落盘到 benchmark/results/。
#
# 用法：
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -Runs 9
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHome C:\Users\Canrad\.jdks\corretto-21.0.4
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -SkipBuild          # 复用已构建的 CLI
#
# 注意：脚本内 xmake 配置均用 -P 显式指定项目，避免 `xmake f -P` 破坏 root 配置（issue #782）。

param(
    [string]$Cli = "",                                    # mj2x-cli 路径；默认 <repo>/build/windows/x64/release/mj2x-cli.exe
    [string]$InputJar = "D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester.jar",
    [string]$Os30Jar = "D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar",
    [string]$JdkHome = "",                                # 默认 $env:JAVA_HOME；JAVA_HOME 未设置则用 D:\sdk\java\jdk17
    [int]$Runs = 9,                                       # 计量取样轮数（不含预热，≥9；中位数为主指标）
    [int]$WarmupRuns = 3,                                 # JVM 预热轮数（不计入统计）
    [double]$MaxCvPercent = 15.0,                         # CV% 阈值：超过即在报告标记"高方差，结果存疑"（#887）
    [switch]$SkipBuild,                                   # 跳过 CLI 构建（复用已有产物）
    [string]$BaselineFile = "",                           # 回归基线 JSON；默认 <repo>/mj2x/benchmark/baseline/regc-perf-baseline.json
    [double]$MaxTotalRatio = 2.0,                         # 回归：RegC total 允许相对基线的最差倍率（防性能退化）
    [double]$MinCoverageRatio = 0.9,                      # 回归：native 覆盖率允许相对基线的最低比例
    [switch]$Regression,                                  # 回归模式：#820 步骤 5。断言 13-probe 全过 + 覆盖率 + 总耗时阈值，失败退出非零；跳过 os30 取样
    [switch]$UpdateBaseline                               # 把本次 RegC 结果写入基线文件（维护基线用）
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")

if ([string]::IsNullOrWhiteSpace($JdkHome)) {
    if (-not [string]::IsNullOrWhiteSpace($env:JAVA_HOME)) { $JdkHome = $env:JAVA_HOME }
    elseif (Test-Path "D:\sdk\java\jdk17") { $JdkHome = "D:\sdk\java\jdk17" }
    else { throw "JAVA_HOME 未设置且未找到 D:\sdk\java\jdk17，请用 -JdkHome 指定" }
}
if (-not (Test-Path (Join-Path $JdkHome "bin\java.exe"))) { throw "JDK 无效: $JdkHome" }
$java = Join-Path $JdkHome "bin\java.exe"
$javac = Join-Path $JdkHome "bin\javac.exe"
$jar = Join-Path $JdkHome "bin\jar.exe"
$javap = Join-Path $JdkHome "bin\javap.exe"

$stamp = Get-Date -Format "yyyy-MM-dd"
$outDir = Join-Path $repo "test\jvm-obf-tester"
$outJar = Join-Path $outDir "jvm-obf-tester-regc.jar"
$projDir = Join-Path $outDir "native_project"
$reportDir = Join-Path $PSScriptRoot "results"
New-Item -ItemType Directory -Force -Path $outDir, $reportDir | Out-Null

if ([string]::IsNullOrWhiteSpace($Cli)) {
    $Cli = Join-Path $repo "build\windows\x64\release\mj2x-cli.exe"
}
if (-not (Test-Path $Cli)) { throw "mj2x-cli 未找到: $Cli（先构建或传 -Cli）" }
if (-not (Test-Path $InputJar)) { throw "输入 jar 未找到: $InputJar" }
# os30 基线缺失时 fail-closed（#887）：对比模式绝不静默跳过竞品侧、更不拿旧
# 数据凑数——旧版/缺失基线正是此前"打平"误判的来源。Regression 模式跳过
# os30 是显式契约（单侧回归门），不在此列。
if (-not $Regression -and -not (Test-Path $Os30Jar)) {
    throw @"
os30（JNIC）对比基线缺失: $Os30Jar
对比模式 fail-closed：不做静默跳过，也不与旧版本数据对比（issue #887：旧版结论被高方差噪声误导）。
放置路径说明：
  1. 把 os30（JNIC）混淆产物放到上面的默认路径，或用 -Os30Jar 显式指定；
  2. 已知 final 版样例：C:\Users\l1502\Downloads\jvm-obf-tester_final.jar（2026-08-17，977338 字节），
     建议复制为 $Os30Jar 后重跑；
  3. 只需 RegC 单侧回归门时请显式使用 -Regression 模式。
"@
}
if ([string]::IsNullOrWhiteSpace($BaselineFile)) {
    $BaselineFile = Join-Path $PSScriptRoot "baseline\regc-perf-baseline.json"
}

function Run-ProcessChecked([string]$FilePath, [string[]]$Arguments, [string]$WorkingDirectory, [string]$LogPath) {
    Push-Location $WorkingDirectory
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $text = (& $FilePath @Arguments 2>&1 | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
        Pop-Location
    }
    if ($LogPath) { Set-Content -Path $LogPath -Value $text -Encoding UTF8 }
    if ($exitCode -ne 0) { throw "$FilePath exited with $exitCode. See $LogPath" }
    return $text
}

# ---- 0. 构建 CLI（默认） ----
if (-not $SkipBuild) {
    Write-Host "==> 构建 mj2x-cli ..."
    # 注意：必须 -c 重置配置 + 显式 -p windows -m release（裸 `xmake f -P` 会
    # 破坏 root 配置，见 issue #782 与交接备忘；缺 -c 时 build 报 invalid argument）。
    & xmake f -c -P $repo -p windows -m release --with_mj2x=y --with_tests=y 2>&1 | Out-Null
    $buildLog = Join-Path $outDir "cli-build.log"
    $out = & xmake build -P $repo mj2x-cli 2>&1 | ForEach-Object { $_.ToString() }
    Set-Content -Path $buildLog -Value ($out -join [Environment]::NewLine) -Encoding UTF8
    if (($out -join "`n") -match "error") { throw "mj2x-cli 构建失败，见 $buildLog" }
    if (-not (Test-Path $Cli)) { throw "构建后仍未找到 $Cli" }
}

# ---- 1. RegC 全量转换 ----
Write-Host "==> RegC 转换 + native 编译 ..."
Remove-Item -Recurse -Force $projDir -ErrorAction SilentlyContinue
Remove-Item -Force $outJar -ErrorAction SilentlyContinue
Run-ProcessChecked $Cli @("-i", $InputJar, "-o", $outJar, "--backend", "regc", "--generate-xmake", $projDir) `
    $outDir (Join-Path $outDir "regc-convert.log") | Out-Null
# -p windows -a x64 显式指定：shell 环境的 PATH/MSYSTEM 污染会让 xmake
# 平台自检误判为 mingw（Git 自带 mingw64），基准口径漂移 + toolchain not found。
Run-ProcessChecked "xmake" @("f", "-P", $projDir, "-p", "windows", "-a", "x64", "-m", "release") `
    $projDir (Join-Path $outDir "native-config.log") | Out-Null
$nativeLog = Join-Path $outDir "native-build.log"
# 必须 cwd=$projDir 调用（同 config 阶段）：从 benchmark 目录裸调用时 xmake
# 会向上找到 mj2x/xmake.lua 的缓存配置，平台漂移成 mingw（toolchain not found）。
Run-ProcessChecked "xmake" @("-P", ".", "mj2x_native") $projDir $nativeLog | Out-Null
if ((Get-Content $nativeLog -Raw) -match "error") { throw "native 编译失败，见 $nativeLog" }

# ---- 2. 嵌入 native DLL ----
$dll = Join-Path $projDir "build\windows\x64\release\mj2x_native.dll"
if (-not (Test-Path $dll)) { throw "native DLL 未生成: $dll" }
$embed = Join-Path $outDir "embed"
Remove-Item -Recurse -Force $embed -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $embed "META-INF\mj2x\native") | Out-Null
Copy-Item $dll (Join-Path $embed "META-INF\mj2x\native\jvm-obf-tester-regc.dll") -Force
Push-Location $embed
& $jar uf $outJar "META-INF/mj2x/native/jvm-obf-tester-regc.dll" 2>&1 | Out-Null
Pop-Location

# ---- 3. 覆盖率审计（javap 验证 ACC_NATIVE + 无 Code 属性） ----
# 口径：可执行方法（排除 abstract）中的 native 比例。
function Get-NativeCoverage([string]$JarPath, [string]$WorkDir) {
    $classesDir = Join-Path $WorkDir "audit-classes"
    Remove-Item -Recurse -Force $classesDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $classesDir | Out-Null
    Push-Location $classesDir
    & $jar xf $JarPath 2>&1 | Out-Null
    Pop-Location

    $total = 0; $executable = 0; $native = 0
    Get-ChildItem -Path $classesDir -Recurse -Filter "*.class" | ForEach-Object {
        $rel = $_.FullName.Substring($classesDir.Length + 1) -replace "\\", "/"
        $className = $rel -replace "\.class$", "" -replace "/", "."
        $javapOut = & $javap -p -classpath $classesDir $className 2>&1
        foreach ($line in $javapOut) {
            # 方法声明行（javap -p 格式）：`  public static native int foo(int, int);`
            # 排除：字段行（含 = 或 ; 后无括号）、`static {}` 初始化块（无括号参数）。
            if ($line -match '^\s+[^=]*\([^;]*\);$') {
                $total++
                if ($line -match '\babstract\b') { continue }   # abstract 不可转 native，不计入可执行
                $executable++
                if ($line -match '\bnative\b') { $native++ }
            }
        }
    }
    $pct = if ($executable -gt 0) { [math]::Round(100.0 * $native / $executable, 1) } else { 0 }
    return [pscustomobject]@{ Total = $total; Executable = $executable; Native = $native; Percent = $pct }
}

Write-Host "==> 覆盖率审计（javap） ..."
$audit = Get-NativeCoverage $outJar (Join-Path $outDir "audit-regc")
if ($Regression) {
    $auditOs30 = $null
} else {
    $auditOs30 = Get-NativeCoverage $Os30Jar (Join-Path $outDir "audit-os30")
}

# ---- 4. 跑 13-probe（预热 WarmupRuns 轮 + Runs 轮计量，#887 方法论） ----
function Run-Probes([string]$JarPath) {
    return & $java -jar $JarPath 2>&1
}

function Get-ProbeTimes([string[]]$Lines) {
    $map = @{}
    foreach ($line in $Lines) {
        # 先取 Status，再取 Time（$Matches 会被下一次 -match 覆盖）。
        if ($line -match '^\| (.+) \| (Passed|Failed) \|') {
            $name   = $Matches[1].Trim()
            $status = $Matches[2]
            $time   = if ($line -match '(\d+\.?\d*)ms') { [double]$Matches[1] } else { 0 }
            $map[$name] = [pscustomobject]@{ Time = $time; Status = $status }
        }
    }
    return $map
}

# 单产物完整计量：WarmupRuns 轮 JVM 预热（每轮独立 JVM，不计入统计）+
# Runs 轮计量取样（每轮独立 JVM 冷启动，与既有口径一致）。
function Measure-Jar([string]$JarPath) {
    for ($i = 1; $i -le $WarmupRuns; $i++) { Run-Probes $JarPath | Out-Null }
    $totals = @(); $maps = @()
    for ($i = 1; $i -le $Runs; $i++) {
        $out = Run-Probes $JarPath
        $m = [regex]::Match(($out -join "`n"), "Total time:\s*(\d+)ms")
        $totals += [int]$m.Groups[1].Value
        $maps += Get-ProbeTimes $out
    }
    return [pscustomobject]@{ Totals = $totals; Maps = $maps }
}

# 运行统计（#887 修尺子）：中位数为主指标，附 min/max/均值/标准差/CV%。
# CV% = 标准差/均值，用于识别 RegC 产物 60~203ms 一类的高方差分布。
function Get-RunStats([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $n = $sorted.Count
    $median = if ($n -eq 0) { 0.0 }
              elseif ($n % 2 -eq 1) { [double]$sorted[[int](($n - 1) / 2)] }
              else { ([double]$sorted[[int]($n / 2 - 1)] + [double]$sorted[[int]($n / 2)]) / 2.0 }
    $mean = if ($n -gt 0) { [double]($sorted | Measure-Object -Average).Average } else { 0.0 }
    $min = if ($n -gt 0) { [double]$sorted[0] } else { 0.0 }
    $max = if ($n -gt 0) { [double]$sorted[$n - 1] } else { 0.0 }
    $stddev = 0.0
    if ($n -gt 1) {
        $sumSq = 0.0
        foreach ($v in $sorted) { $sumSq += ($v - $mean) * ($v - $mean) }
        $stddev = [math]::Sqrt($sumSq / ($n - 1))
    }
    $cv = if ($mean -gt 0) { 100.0 * $stddev / $mean } else { 0.0 }
    return [pscustomobject]@{ Median = $median; Mean = $mean; Min = $min; Max = $max; StdDev = $stddev; CvPercent = $cv; Count = $n }
}

# 报告数字格式化统一出口（1/2/3 位小数），避免行内 [math]::Round 噪声。
function Round1([double]$V) { [math]::Round($V, 1) }
function Round2([double]$V) { [math]::Round($V, 2) }
function Round3([double]$V) { [math]::Round($V, 3) }

# 单产物统计面：total 统计 + 逐 probe 统计（{probe 名 → Get-RunStats 结果}）。
function Get-StatsFace([object]$Measurement) {
    $probeRuns = @{}
    foreach ($map in $Measurement.Maps) {
        foreach ($name in $map.Keys) {
            if (-not $probeRuns.ContainsKey($name)) {
                $probeRuns[$name] = New-Object System.Collections.Generic.List[double]
            }
            $probeRuns[$name].Add([double]$map[$name].Time)
        }
    }
    $probeStats = @{}
    foreach ($name in $probeRuns.Keys) {
        $probeStats[$name] = Get-RunStats $probeRuns[$name].ToArray()
    }
    return [pscustomobject]@{ Total = (Get-RunStats $Measurement.Totals); Probes = $probeStats }
}

Write-Host "==> 跑探针（预热 $WarmupRuns 轮 + $Runs 轮计量，主指标=中位数） ..."
$regcMeasurement = Measure-Jar $outJar
$regcFace = Get-StatsFace $regcMeasurement
$os30Measurement = $null
$os30Face = $null
if (-not $Regression) {
    $os30Measurement = Measure-Jar $Os30Jar
    $os30Face = Get-StatsFace $os30Measurement
}

# 高方差存疑清单（#887）：total 或任一 probe 的 CV% 超阈值即入列，
# 报告显式呈现，防止"假打平"再次误导结论。
$highVariance = New-Object System.Collections.Generic.List[string]
if ($regcFace.Total.CvPercent -gt $MaxCvPercent) {
    $highVariance.Add("RegC total (CV=$([math]::Round($regcFace.Total.CvPercent, 1))%)")
}
foreach ($k in ($regcFace.Probes.Keys | Sort-Object)) {
    $s = $regcFace.Probes[$k]
    if ($s.CvPercent -gt $MaxCvPercent) {
        $highVariance.Add("RegC $k (CV=$([math]::Round($s.CvPercent, 1))%)")
    }
}
if ($os30Face -ne $null) {
    if ($os30Face.Total.CvPercent -gt $MaxCvPercent) {
        $highVariance.Add("os30 total (CV=$([math]::Round($os30Face.Total.CvPercent, 1))%)")
    }
    foreach ($k in ($os30Face.Probes.Keys | Sort-Object)) {
        $s = $os30Face.Probes[$k]
        if ($s.CvPercent -gt $MaxCvPercent) {
            $highVariance.Add("os30 $k (CV=$([math]::Round($s.CvPercent, 1))%)")
        }
    }
}

# ---- 5. 报告 ----
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# jvm-obf-tester 13-probe 对比（$stamp）")
$lines.Add("")
# java -version 走 stderr；PS5.1 会把 NativeCommandError 记录混入管道，经
# cmd /c 合并后取纯文本（基线 meta.jdk 同口径，防 ErrorRecord 污染 JSON）。
$jdkVersion = [string]((cmd /c "`"$java`" -version 2>&1" | Select-Object -First 1))
$lines.Add("- 环境：JDK $jdkVersion")
$lines.Add("- 取样：预热 $WarmupRuns 轮（不计入统计）+ $Runs 轮计量；主指标=中位数，附 min~max 与 CV%")
$lines.Add("- 高方差判定：CV% > $MaxCvPercent% 标记 ⚠ 并汇总为存疑项（#887 方法论）")
$lines.Add("- RegC 产物：$outJar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入）")
if ($Regression) {
    $lines.Add("- 模式：性能回归（#820 步骤 5，基线 $BaselineFile，MaxTotalRatio=$MaxTotalRatio，MinCoverageRatio=$MinCoverageRatio）")
} else {
    # 记录 os30 产物的体积与时间戳：防止拿旧版本数据对比而不自知（#887）。
    $os30Item = Get-Item $Os30Jar
    $lines.Add("- os30 产物：$Os30Jar（JNIC 竞品，黑盒对比；$($os30Item.Length) 字节，$($os30Item.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))）")
}
$lines.Add("")
# 高方差警告置顶：结论性阅读前先看这里（#887：假打平的教训）。
if ($highVariance.Count -gt 0) {
    $lines.Add("> **⚠ 高方差警告：结果存疑** — 以下样本 CV% 超过 $MaxCvPercent% 阈值，")
    $lines.Add("> 不应作为打平/领先结论依据，建议提高 -Runs 或排查环境负载后重测：")
    foreach ($item in $highVariance) { $lines.Add("> - $item") }
    $lines.Add("")
}
$lines.Add("## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）")
$lines.Add("")
$lines.Add("| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |")
$lines.Add("|---|---|---|---|---|")
$lines.Add("| RegC | $($audit.Total) | $($audit.Executable) | $($audit.Native) | $($audit.Percent)% |")
if (-not $Regression) {
    $lines.Add("| os30 | $($auditOs30.Total) | $($auditOs30.Executable) | $($auditOs30.Native) | $($auditOs30.Percent)% |")
}
$lines.Add("")
$lines.Add("## 总耗时（ms，主指标=中位数）")
$lines.Add("")
if ($Regression) {
    $lines.Add("| 项 | RegC 中位数 | RegC min~max | RegC CV% |")
    $lines.Add("|---|---|---|---|")
    $t = $regcFace.Total
    $regcTotalCvCell = "$(Round1 $t.CvPercent)%$(if ($t.CvPercent -gt $MaxCvPercent) { ' ⚠' } else { '' })"
    $lines.Add("| Total | $(Round1 $t.Median) | $(Round1 $t.Min)~$(Round1 $t.Max) | $regcTotalCvCell |")
} else {
    $rt = $regcFace.Total
    $ot = $os30Face.Total
    $regcTotalCvCell = "$(Round1 $rt.CvPercent)%$(if ($rt.CvPercent -gt $MaxCvPercent) { ' ⚠' } else { '' })"
    $os30TotalCvCell = "$(Round1 $ot.CvPercent)%$(if ($ot.CvPercent -gt $MaxCvPercent) { ' ⚠' } else { '' })"
    $lines.Add("| 项 | RegC 中位数 | RegC min~max | RegC CV% | os30 中位数 | os30 min~max | os30 CV% | ratio (中位数) |")
    $lines.Add("|---|---|---|---|---|---|---|---|")
    $lines.Add("| Total | $(Round1 $rt.Median) | $(Round1 $rt.Min)~$(Round1 $rt.Max) | $regcTotalCvCell | $(Round1 $ot.Median) | $(Round1 $ot.Min)~$(Round1 $ot.Max) | $os30TotalCvCell | $(Round2 ($rt.Median / $ot.Median)) |")
}
$lines.Add("")
$lines.Add("## 各探针耗时（ms，主指标=中位数）")
$lines.Add("")
if ($Regression) {
    $lines.Add("| Probe | RegC 中位数 | RegC CV% |")
    $lines.Add("|---|---|---|")
    foreach ($k in ($regcFace.Probes.Keys | Sort-Object)) {
        $s = $regcFace.Probes[$k]
        $cvCell = "$(Round1 $s.CvPercent)%$(if ($s.CvPercent -gt $MaxCvPercent) { ' ⚠' } else { '' })"
        $lines.Add("| $k | $(Round3 $s.Median) | $cvCell |")
    }
} else {
    $lines.Add("| Probe | RegC 中位数 | RegC CV% | os30 中位数 | os30 CV% | ratio (中位数) |")
    $lines.Add("|---|---|---|---|---|---|")
    foreach ($k in ($os30Face.Probes.Keys | Sort-Object)) {
        $o = $os30Face.Probes[$k]
        $r = if ($regcFace.Probes.ContainsKey($k)) { $regcFace.Probes[$k] } else { $null }
        $rMedian = if ($r -ne $null) { $r.Median } else { 0.0 }
        $rCv = if ($r -ne $null) { $r.CvPercent } else { 0.0 }
        $rCell = "$(Round1 $rCv)%$(if ($rCv -gt $MaxCvPercent) { ' ⚠' } else { '' })"
        $oCell = "$(Round1 $o.CvPercent)%$(if ($o.CvPercent -gt $MaxCvPercent) { ' ⚠' } else { '' })"
        $rr = if ($o.Median -gt 0) { Round2 ($rMedian / $o.Median) } else { "-" }
        $lines.Add("| $k | $(Round3 $rMedian) | $rCell | $(Round3 $o.Median) | $oCell | $rr |")
    }
}
$lines.Add("")
$lines.Add("## 方法论说明（issue #887 修尺子）")
$lines.Add("")
$lines.Add("- 预热 $WarmupRuns 轮（独立 JVM，不计入统计）+ $Runs 轮计量（独立 JVM 冷启动）。")
$lines.Add("- 主指标为中位数（对 60~203ms 一类长尾抖动稳健）；均值仅供参照，不再用于结论。")
$lines.Add("- CV% = 标准差/均值；超过 $MaxCvPercent% 的行标记 ⚠，并在文首汇总为高方差存疑项。")
$lines.Add("- JDK 版本：$jdkVersion。")
$lines.Add("- 历史教训：#887 中单次 72.7 vs 71ms 的假打平，在预热+9 轮中位数口径下实为 ~1.6x 差距（53 vs 83ms，JDK25）。")
$reportPath = Join-Path $reportDir "$stamp-jvm-obf-tester-os30.md"
Set-Content -Path $reportPath -Value ($lines -join [Environment]::NewLine) -Encoding UTF8

# 控制台输出
$rt = $regcFace.Total
"`n=== RegC total ($Runs runs, median): $(Round1 $rt.Median) ms  min~max=$(Round1 $rt.Min)~$(Round1 $rt.Max)  mean=$(Round1 $rt.Mean)  cv=$(Round1 $rt.CvPercent)%"
if (-not $Regression) {
    $ot = $os30Face.Total
    "=== os30 total ($Runs runs, median): $(Round1 $ot.Median) ms  min~max=$(Round1 $ot.Min)~$(Round1 $ot.Max)  mean=$(Round1 $ot.Mean)  cv=$(Round1 $ot.CvPercent)%"
    "=== ratio regc/os30 (median): $(Round2 ($rt.Median / $ot.Median))"
    "=== 覆盖率审计: RegC $($audit.Native)/$($audit.Executable) ($($audit.Percent)%)  os30 $($auditOs30.Native)/$($auditOs30.Executable) ($($auditOs30.Percent)%)"
    "{0,-32} {1,12} {2,8} {3,12} {4,8} {5,8}" -f "Probe", "regc-med(ms)", "regc-cv", "os30-med(ms)", "os30-cv", "ratio"
    foreach ($k in ($os30Face.Probes.Keys | Sort-Object)) {
        $o = $os30Face.Probes[$k]
        $r = if ($regcFace.Probes.ContainsKey($k)) { $regcFace.Probes[$k] } else { $null }
        $rMedian = if ($r -ne $null) { $r.Median } else { 0.0 }
        $rCv = if ($r -ne $null) { $r.CvPercent } else { 0.0 }
        $rr = if ($o.Median -gt 0) { Round2 ($rMedian / $o.Median) } else { "-" }
        "{0,-32} {1,12} {2,8} {3,12} {4,8} {5,8}" -f $k, (Round3 $rMedian), "$(Round1 $rCv)%", (Round3 $o.Median), "$(Round1 $o.CvPercent)%", $rr
    }
} else {
    "=== 覆盖率审计: RegC $($audit.Native)/$($audit.Executable) ($($audit.Percent)%)"
    "{0,-32} {1,12} {2,8}" -f "Probe", "regc-med(ms)", "regc-cv"
    foreach ($k in ($regcFace.Probes.Keys | Sort-Object)) {
        $s = $regcFace.Probes[$k]
        "{0,-32} {1,12} {2,8}" -f $k, (Round3 $s.Median), "$(Round1 $s.CvPercent)%"
    }
}
if ($highVariance.Count -gt 0) {
    "=== [高方差警告] 以下样本 CV% > $MaxCvPercent%，结果存疑（详见报告）:"
    $highVariance | ForEach-Object { "  - $_" }
}
"`n报告已写入: $reportPath"

# ---- 6. 基线维护与回归断言（#820 步骤 5；#887 起主指标改中位数） ----
if ($Regression -or $UpdateBaseline) {
    if (-not (Test-Path $BaselineFile)) { throw "基线文件未找到: $BaselineFile" }
    $baseline = Get-Content $BaselineFile -Raw | ConvertFrom-Json

    if ($UpdateBaseline) {
        $baseline.meta.generated = $stamp
        # java -version 走 stderr，PS5.1 下 `& $java -version 2>&1` 会把
        # ErrorRecord 对象灌进 meta.jdk（旧基线已踩坑）——统一走 cmd /c 取纯文本。
        $baseline.meta.jdk = $jdkVersion
        $baseline.meta.runs = $Runs
        $baseline.meta.warmupRuns = $WarmupRuns
        $baseline.meta.metric = "median"
        $baseline.meta.methodology = "预热 ${WarmupRuns} 轮（不计入统计）+ ${Runs} 轮计量取中位数；CV%>${MaxCvPercent}% 标记高方差存疑（#887）；覆盖率审计 javap 验证 ACC_NATIVE"
        # 基线主指标：中位数（#887 前的历史基线为均值口径，更新基线时随之迁移）。
        $baseline.baseline.totalMs = Round1 $regcFace.Total.Median
        $baseline.baseline.totalCvPercent = Round1 $regcFace.Total.CvPercent
        $baseline.baseline.coverage.native = $audit.Native
        $baseline.baseline.coverage.executable = $audit.Executable
        $baseline.baseline.coverage.percent = $audit.Percent
        $baseline.baseline.probesMs = [ordered]@{}
        foreach ($k in ($regcFace.Probes.Keys | Sort-Object)) {
            $baseline.baseline.probesMs | Add-Member -NotePropertyName $k -NotePropertyValue (Round3 $regcFace.Probes[$k].Median)
        }
        $baseline | ConvertTo-Json -Depth 6 | Set-Content $BaselineFile -Encoding UTF8
        Write-Host "`n基线已更新: $BaselineFile"
    }

    if ($Regression) {
        $errors = New-Object System.Collections.Generic.List[string]
        # 功能门：13-probe 全部 Passed
        foreach ($name in $regcFace.Probes.Keys) {
            $failedRun = $regcMeasurement.Maps | Where-Object { $_.$name.Status -ne "Passed" } | Select-Object -First 1
            if ($failedRun) { $errors.Add("probe '$name' 未通过（功能门）") }
        }
        # 覆盖率门：native 比例不低于基线 × MinCoverageRatio
        $covFloor = [math]::Round($baseline.baseline.coverage.percent * $MinCoverageRatio, 1)
        if ($audit.Percent -lt $covFloor) {
            $errors.Add("覆盖率退化: $($audit.Percent)% < 基线 $($baseline.baseline.coverage.percent)% × $MinCoverageRatio = $covFloor%")
        }
        # 总耗时门：RegC total 中位数不高于基线 × MaxTotalRatio
        $totalCeil = [math]::Round($baseline.baseline.totalMs * $MaxTotalRatio, 1)
        $regcTotalMedian = Round1 $regcFace.Total.Median
        if ($regcTotalMedian -gt $totalCeil) {
            $errors.Add("性能退化: RegC total 中位数 ${regcTotalMedian}ms > 基线 $($baseline.baseline.totalMs)ms × $MaxTotalRatio = ${totalCeil}ms")
        }
        if ($errors.Count -gt 0) {
            Write-Host "`n[回归失败]"
            $errors | ForEach-Object { Write-Host "  - $_" }
            Write-Host "  基线: $BaselineFile (total=$($baseline.baseline.totalMs)ms, coverage=$($baseline.baseline.coverage.percent)%)"
            exit 1
        }
        $varianceNote = ""
        if ($highVariance.Count -gt 0) {
            $varianceNote = "（注意：本次 CV% 超阈值项 $($highVariance.Count) 个，结果存疑，建议复测确认）"
        }
        Write-Host "`n[回归通过] RegC total 中位数=${regcTotalMedian}ms (≤ ${totalCeil}ms)，覆盖率 $($audit.Percent)% (≥ $covFloor%)，13-probe 全过$varianceNote"
    }
}
