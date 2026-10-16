# jvm_obfuscator_test_benchmark.ps1 — RegC vs os30(JNIC) 13-probe 性能对比基准
#
# 用途：把 jvm-obf-tester（13 个探针 jar）用 mj2x-cli --backend regc 全量 native 化，
# 与竞品 os30（JNIC 混淆产物）在同一 JDK 下对比各探针耗时，产出 markdown + CSV 报告。
# 依据：issue #787 §3.1（RegC 性能基线）+ #778（热循环性能对比）+ #887（方法论升级）。
#
# 方法学（覆盖率审计先行 + 预热/计量分层，#887 修尺子）：
#   1. 每次对比前先做覆盖率审计（javap 验证 ACC_NATIVE + 无 Code 属性），
#      未附覆盖率的性能数据不予采信；
#   2. 预热 WarmupRuns 轮（默认 3，不计入统计）；
#   3. Runs 轮计量取样（默认 9），以中位数为主指标，
#      同时输出 min/max/p95/均值/标准差/方差/CV%；
#   4. 任一 CV% 超过 MaxCvPercent（默认 15）→ 报告显式标记"高方差，结果存疑"，
#      防止单次/低样本均值造成"假打平"结论（#887：72.7 vs 71ms 即被噪声误导）；
#   5. os30 基线缺失时 fail-closed：给出明确错误与放置路径说明，
#      不静默跳过、不拿旧数据对比；
#   6. 输出报告落盘到 benchmark/results/（markdown 表 + runs/stats 两个 CSV）。
#   7. 双 JDK 参数化（#887）：-JdkHomes 传多个 JDK 时逐 JDK 计量，
#      输出跨 JDK 对比表；同一进程会话内顺序完成，避免跨时段噪声。
#
# 用法：
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -Runs 9
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHome C:\Users\Canrad\.jdks\corretto-21.0.4
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -SkipBuild          # 复用已构建的 CLI
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHomes A,B       # 双 JDK 参数化（#887）
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -Attribution        # GC/类加载/JIT 日志采集
#
# 注意：脚本内 xmake 配置均用 -P 显式指定项目，避免 `xmake f -P` 破坏 root 配置（issue #782）。

param(
    [string]$Cli = "",                                    # mj2x-cli 路径；默认 <repo>/build/windows/x64/release/mj2x-cli.exe
    [string]$InputJar = "D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester.jar",
    [string]$Os30Jar = "D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar",
    [string]$JdkHome = "",                                # 单 JDK 模式；默认 $env:JAVA_HOME。与 -JdkHomes 互斥（-JdkHomes 优先）
    [string[]]$JdkHomes = @(),                            # 双 JDK 参数化（#887）：多个 JDK 时逐 JDK 计量并输出跨 JDK 对比表
    [int]$Runs = 9,                                       # 计量取样轮数（不含预热，≥9；中位数为主指标）
    [int]$WarmupRuns = 3,                                 # JVM 预热轮数（不计入统计）
    [double]$MaxCvPercent = 15.0,                         # CV% 阈值：超过即在报告标记"高方差，结果存疑"（#887）
    [switch]$SkipBuild,                                   # 跳过 CLI 构建（复用已有产物）
    [string]$BaselineFile = "",                           # 回归基线 JSON；默认 <repo>/mj2x/benchmark/baseline/regc-perf-baseline.json
    [double]$MaxTotalRatio = 2.0,                         # 回归：RegC total 允许相对基线的最差倍率（防性能退化）
    [double]$MinCoverageRatio = 0.9,                      # 回归：native 覆盖率允许相对基线的最低比例
    [switch]$Regression,                                  # 回归模式：#820 步骤 5。断言 13-probe 全过 + 覆盖率 + 总耗时阈值，失败退出非零；跳过 os30 取样
    [switch]$UpdateBaseline,                              # 把本次 RegC 结果写入基线文件（维护基线用；多 JDK 时取第一个 JDK 的结果）
    [switch]$RegcDirectCalls,                             # 转换时启用 --regc-direct-calls（#887 直调口径；产物间 native-to-native 直调）
    [switch]$Attribution                                  # 方差归因模式（#887）：每 JDK×产物额外跑 1 轮冷启动，采集 GC/类加载/JIT 日志
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")

# ---- JDK 解析：-JdkHomes 优先；为空时退回单 -JdkHome（兼容既有调用） ----
if ($JdkHomes.Count -gt 0) {
    # -File 调用时 shell 不会把 `a,b` 解析成数组——整串按逗号拆开（pwsh -Command 才有数组字面量）。
    $jdkHomeList = @($JdkHomes | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
} else {
    if ([string]::IsNullOrWhiteSpace($JdkHome)) {
        if (-not [string]::IsNullOrWhiteSpace($env:JAVA_HOME)) { $JdkHome = $env:JAVA_HOME }
        elseif (Test-Path "D:\sdk\java\jdk17") { $JdkHome = "D:\sdk\java\jdk17" }
        else { throw "JAVA_HOME 未设置且未找到 D:\sdk\java\jdk17，请用 -JdkHome/-JdkHomes 指定" }
    }
    $jdkHomeList = @($JdkHome)
}
$javaByHome = @{}
foreach ($jdkHome in $jdkHomeList) {
    if (-not (Test-Path (Join-Path $jdkHome "bin\java.exe"))) { throw "JDK 无效: $jdkHome" }
    $javaByHome[$jdkHome] = Join-Path $jdkHome "bin\java.exe"
}
$javap = Join-Path $jdkHomeList[0] "bin\javap.exe"
$jarTool = Join-Path $jdkHomeList[0] "bin\jar.exe"

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
$convertArgs = @("-i", $InputJar, "-o", $outJar, "--backend", "regc", "--generate-xmake", $projDir)
if ($RegcDirectCalls) { $convertArgs += "--regc-direct-calls" }
Run-ProcessChecked $Cli $convertArgs $outDir (Join-Path $outDir "regc-convert.log") | Out-Null
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
& $jarTool uf $outJar "META-INF/mj2x/native/jvm-obf-tester-regc.dll" 2>&1 | Out-Null
Pop-Location

# ---- 3. 覆盖率审计（javap 验证 ACC_NATIVE + 无 Code 属性） ----
# 口径：可执行方法（排除 abstract）中的 native 比例。
function Get-NativeCoverage([string]$JarPath, [string]$WorkDir, [string]$JdkHomePath) {
    $classesDir = Join-Path $WorkDir "audit-classes"
    Remove-Item -Recurse -Force $classesDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $classesDir | Out-Null
    Push-Location $classesDir
    & $jarTool xf $JarPath 2>&1 | Out-Null
    Pop-Location

    $total = 0; $executable = 0; $native = 0
    Get-ChildItem -Path $classesDir -Recurse -Filter "*.class" | ForEach-Object {
        $rel = $_.FullName.Substring($classesDir.Length + 1) -replace "\\", "/"
        $className = $rel -replace "\.class$", "" -replace "/", "."
        $javapOut = & (Join-Path $JdkHomePath "bin\javap.exe") -p -classpath $classesDir $className 2>&1
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
$audit = Get-NativeCoverage $outJar (Join-Path $outDir "audit-regc") $jdkHomeList[0]
if ($Regression) {
    $auditOs30 = $null
} else {
    $auditOs30 = Get-NativeCoverage $Os30Jar (Join-Path $outDir "audit-os30") $jdkHomeList[0]
}

# ---- 4. 跑 13-probe（预热 WarmupRuns 轮 + Runs 轮计量，#887 方法论） ----
function Run-Probes([string]$JavaPath, [string]$JarPath) {
    return & $JavaPath -jar $JarPath 2>&1
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
function Measure-Jar([string]$JavaPath, [string]$JarPath) {
    for ($i = 1; $i -le $WarmupRuns; $i++) { Run-Probes $JavaPath $JarPath | Out-Null }
    $totals = @(); $maps = @()
    for ($i = 1; $i -le $Runs; $i++) {
        $out = Run-Probes $JavaPath $JarPath
        $m = [regex]::Match(($out -join "`n"), "Total time:\s*(\d+)ms")
        $totals += [int]$m.Groups[1].Value
        $maps += Get-ProbeTimes $out
    }
    return [pscustomobject]@{ Totals = $totals; Maps = $maps }
}

# 运行统计（#887 修尺子）：中位数为主指标，附 min/max/p95/均值/标准差/方差/CV%。
# CV% = 标准差/均值，用于识别 RegC 产物 60~203ms 一类的高方差分布；
# p95（nearest-rank）对调用方判断长尾位置（如"9 轮里最差的 5% 有多差"）。
function Get-RunStats([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $n = $sorted.Count
    $median = if ($n -eq 0) { 0.0 }
              elseif ($n % 2 -eq 1) { [double]$sorted[[int](($n - 1) / 2)] }
              else { ([double]$sorted[[int]($n / 2 - 1)] + [double]$sorted[[int]($n / 2)]) / 2.0 }
    $mean = if ($n -gt 0) { [double]($sorted | Measure-Object -Average).Average } else { 0.0 }
    $min = if ($n -gt 0) { [double]$sorted[0] } else { 0.0 }
    $max = if ($n -gt 0) { [double]$sorted[$n - 1] } else { 0.0 }
    # nearest-rank p95：ceil(0.95*n) 位样本（n=9 → 第 9 位即 max；n=20 → 第 19 位）。
    $p95 = if ($n -gt 0) { [double]$sorted[[math]::Min($n - 1, [int][math]::Ceiling(0.95 * $n) - 1)] } else { 0.0 }
    $variance = 0.0
    if ($n -gt 1) {
        $sumSq = 0.0
        foreach ($v in $sorted) { $sumSq += ($v - $mean) * ($v - $mean) }
        $variance = $sumSq / ($n - 1)
    }
    $stddev = [math]::Sqrt($variance)
    $cv = if ($mean -gt 0) { 100.0 * $stddev / $mean } else { 0.0 }
    return [pscustomobject]@{ Median = $median; Mean = $mean; Min = $min; Max = $max; P95 = $p95; StdDev = $stddev; Variance = $variance; CvPercent = $cv; Count = $n }
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

# JDK 标签：从 `java -version` 首行取版本主号（如 21/25），跨 JDK 表格列名用。
function Get-JdkContext([string]$JdkHomePath) {
    $java = $javaByHome[$JdkHomePath]
    # java -version 走 stderr；PS5.1 会把 NativeCommandError 记录混入管道，经
    # cmd /c 合并后取纯文本（基线 meta.jdk 同口径，防 ErrorRecord 污染 JSON）。
    $versionLine = [string]((cmd /c "`"$java`" -version 2>&1" | Select-Object -First 1))
    $major = if ($versionLine -match 'version "(\d+)') { $Matches[1] } else { "unknown" }
    return [pscustomobject]@{ JdkHome = $JdkHomePath; Java = $java; Tag = "jdk$major"; Version = $versionLine }
}

Write-Host "==> 跑探针（$($jdkHomeList.Count) 个 JDK × 预热 $WarmupRuns 轮 + $Runs 轮计量，主指标=中位数） ..."
$faces = @()
foreach ($jdkHomePath in $jdkHomeList) {
    $ctx = Get-JdkContext $jdkHomePath
    Write-Host "--- [$($ctx.Tag)] RegC ..."
    $regcMeasurement = Measure-Jar $ctx.Java $outJar
    $os30Measurement = $null
    if (-not $Regression) {
        Write-Host "--- [$($ctx.Tag)] os30 ..."
        $os30Measurement = Measure-Jar $ctx.Java $Os30Jar
    }
    $faces += [pscustomobject]@{
        Ctx = $ctx
        Regc = (Get-StatsFace $regcMeasurement)
        RegcMeasurement = $regcMeasurement
        Os30 = $(if ($null -ne $os30Measurement) { Get-StatsFace $os30Measurement } else { $null })
        Os30Measurement = $os30Measurement
    }
}

# 高方差存疑清单（#887）：total 或任一 probe 的 CV% 超阈值即入列，
# 报告显式呈现，防止"假打平"再次误导结论。
$highVariance = New-Object System.Collections.Generic.List[string]
foreach ($face in $faces) {
    $tag = $face.Ctx.Tag
    if ($face.Regc.Total.CvPercent -gt $MaxCvPercent) {
        $highVariance.Add("$tag RegC total (CV=$([math]::Round($face.Regc.Total.CvPercent, 1))%)")
    }
    foreach ($k in ($face.Regc.Probes.Keys | Sort-Object)) {
        $s = $face.Regc.Probes[$k]
        if ($s.CvPercent -gt $MaxCvPercent) {
            $highVariance.Add("$tag RegC $k (CV=$([math]::Round($s.CvPercent, 1))%)")
        }
    }
    if ($null -ne $face.Os30) {
        if ($face.Os30.Total.CvPercent -gt $MaxCvPercent) {
            $highVariance.Add("$tag os30 total (CV=$([math]::Round($face.Os30.Total.CvPercent, 1))%)")
        }
        foreach ($k in ($face.Os30.Probes.Keys | Sort-Object)) {
            $s = $face.Os30.Probes[$k]
            if ($s.CvPercent -gt $MaxCvPercent) {
                $highVariance.Add("$tag os30 $k (CV=$([math]::Round($s.CvPercent, 1))%)")
            }
        }
    }
}

# ---- 5. 归因模式（#887）：每 JDK×产物额外 1 轮冷启动，采集 GC/类加载/JIT 日志 ----
# 相对路径落日志（-Xlog file= 相对 cwd）：Windows 盘符冒号会截断 -Xlog 的 file= 值。
function Invoke-AttributionRound([object]$Ctx, [string]$JarPath, [string]$ArtifactName) {
    $logDir = Join-Path $outDir "attribution"
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $tag = "$($Ctx.Tag)-$ArtifactName"
    # JIT 时间线用 -XX:+PrintCompilation（stdout 捕获进 $tag.stdout.log，含
    # "made not entrant/deoptimiz" 标记）：实测 -Xlog:jit+compilation 在本机
    # MS JDK 21.0.11 下静默零输出（JDK25 正常），PrintCompilation 全版本可靠。
    $args = @(
        "-Xlog:gc=info:file=$tag.gc.log:time,uptime",
        "-Xlog:class+load=info:file=$tag.classload.log:time,uptime",
        "-Xlog:deoptimization=info:file=$tag.deopt.log:time,uptime",
        "-XX:+PrintCompilation",
        "-jar", $JarPath
    )
    $stdout = Run-ProcessChecked $Ctx.Java $args $logDir (Join-Path $logDir "$tag.stdout.log")
    Write-Host "    归因日志 → $logDir\$tag.*.log"
    return $stdout
}

$attributionDir = $null
if ($Attribution) {
    $attributionDir = Join-Path $outDir "attribution"
    New-Item -ItemType Directory -Force $attributionDir | Out-Null
    Write-Host "==> 归因日志采集（GC/类加载/JIT，冷启动 1 轮） ..."
    # 默认 GC 识别：-Xlog:gc -version 首行打印 "Using G1"/"Using The Z Garbage Collector"。
    $gcLines = New-Object System.Collections.Generic.List[string]
    foreach ($face in $faces) {
        $gcDefault = (Run-ProcessChecked $face.Ctx.Java @("-Xlog:gc", "-version") $outDir $null | Select-String "Using").ToString()
        $gcLines.Add("$($face.Ctx.Tag) ($($face.Ctx.Version)): $gcDefault")
        Invoke-AttributionRound $face.Ctx $outJar "regc" | Out-Null
        if (-not $Regression) { Invoke-AttributionRound $face.Ctx $Os30Jar "os30" | Out-Null }
    }
    Set-Content -Path (Join-Path $attributionDir "default-gc.txt") -Value $gcLines -Encoding UTF8
    Write-Host ($gcLines | ForEach-Object { "    $_" })
}

# ---- 6. 报告（markdown + CSV） ----
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# jvm-obf-tester 13-probe 对比（$stamp）")
$lines.Add("")
$jdkDesc = ($faces | ForEach-Object { "$($_.Ctx.Tag) [$($_.Ctx.Version)]" }) -join "、"
$lines.Add("- 环境：JDK $jdkDesc（$($faces.Count) 个 JDK，同一会话顺序计量）")
$lines.Add("- 取样：预热 $WarmupRuns 轮（不计入统计）+ $Runs 轮计量；主指标=中位数，附 min~max/p95 与 CV%")
$lines.Add("- 高方差判定：CV% > $MaxCvPercent% 标记 ⚠ 并汇总为存疑项（#887 方法论）")
$dcNote = if ($RegcDirectCalls) { "开（--regc-direct-calls，native-to-native 直调口径）" } else { "关（默认 JNI 边界口径）" }
$lines.Add("- RegC 产物：$outJar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入；直调：$dcNote）")
if (-not $Regression) {
    # 记录 os30 产物的体积与时间戳：防止拿旧版本数据对比而不自知（#887）。
    $os30Item = Get-Item $Os30Jar
    $lines.Add("- os30 产物：$Os30Jar（JNIC 竞品，黑盒对比；$($os30Item.Length) 字节，$($os30Item.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))）")
}
if ($Regression) {
    $lines.Add("- 模式：性能回归（#820 步骤 5，基线 $BaselineFile，MaxTotalRatio=$MaxTotalRatio，MinCoverageRatio=$MinCoverageRatio；多 JDK 时取第一个 JDK 的结果）")
}
if ($Attribution) {
    $lines.Add("- 归因日志：$attributionDir（GC/类加载/JIT，各 JDK×产物冷启动 1 轮；default-gc.txt 记录各 JDK 默认收集器）")
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

# 跨 JDK 总耗时对比表（多 JDK 时）
if ($faces.Count -gt 1 -and -not $Regression) {
    $lines.Add("## 跨 JDK 总耗时对比（ms，主指标=中位数）")
    $lines.Add("")
    $header = "| 项 |"
    $sep = "|---|"
    foreach ($face in $faces) {
        $header += " $($face.Ctx.Tag) RegC 中位数 | $($face.Ctx.Tag) os30 中位数 | $($face.Ctx.Tag) ratio |"
        $sep += "---|---|---|"
    }
    $lines.Add($header)
    $lines.Add($sep)
    $row = "| Total |"
    foreach ($face in $faces) {
        $rt = $face.Regc.Total; $ot = $face.Os30.Total
        $row += " $(Round1 $rt.Median) | $(Round1 $ot.Median) | $(Round2 ($rt.Median / $ot.Median)) |"
    }
    $lines.Add($row)
    $lines.Add("")
}

# 跨 JDK 探针 ratio 对比表（多 JDK 时）：一表看清哪些探针在哪些 JDK 上落后/领先。
if ($faces.Count -gt 1 -and -not $Regression) {
    $lines.Add("## 跨 JDK 探针 ratio 对比（RegC 中位数 / os30 中位数）")
    $lines.Add("")
    $header = "| Probe |"
    $sep = "|---|"
    foreach ($face in $faces) { $header += " $($face.Ctx.Tag) ratio |"; $sep += "---|" }
    $lines.Add($header)
    $lines.Add($sep)
    $probeNames = $faces[0].Os30.Probes.Keys | Sort-Object
    foreach ($k in $probeNames) {
        $row = "| $k |"
        foreach ($face in $faces) {
            $o = $face.Os30.Probes[$k]
            $r = if ($face.Regc.Probes.ContainsKey($k)) { $face.Regc.Probes[$k].Median } else { 0.0 }
            $row += " $(if ($o.Median -gt 0) { Round2 ($r / $o.Median) } else { '-' }) |"
        }
        $lines.Add($row)
    }
    $lines.Add("")
}

# 每 JDK 明细节
foreach ($face in $faces) {
    $lines.Add("## $($face.Ctx.Tag)（$($face.Ctx.Version)）")
    $lines.Add("")
    $lines.Add("### 总耗时（ms，主指标=中位数）")
    $lines.Add("")
    $rt = $face.Regc.Total
    $regcTotalCvCell = "$(Round1 $rt.CvPercent)%$(if ($rt.CvPercent -gt $MaxCvPercent) { ' ⚠' } else { '' })"
    if ($Regression) {
        $lines.Add("| 项 | RegC 中位数 | RegC min~max | RegC p95 | RegC CV% |")
        $lines.Add("|---|---|---|---|---|")
        $lines.Add("| Total | $(Round1 $rt.Median) | $(Round1 $rt.Min)~$(Round1 $rt.Max) | $(Round1 $rt.P95) | $regcTotalCvCell |")
    } else {
        $ot = $face.Os30.Total
        $os30TotalCvCell = "$(Round1 $ot.CvPercent)%$(if ($ot.CvPercent -gt $MaxCvPercent) { ' ⚠' } else { '' })"
        $lines.Add("| 项 | RegC 中位数 | RegC min~max | RegC p95 | RegC CV% | os30 中位数 | os30 min~max | os30 p95 | os30 CV% | ratio (中位数) |")
        $lines.Add("|---|---|---|---|---|---|---|---|---|---|")
        $lines.Add("| Total | $(Round1 $rt.Median) | $(Round1 $rt.Min)~$(Round1 $rt.Max) | $(Round1 $rt.P95) | $regcTotalCvCell | $(Round1 $ot.Median) | $(Round1 $ot.Min)~$(Round1 $ot.Max) | $(Round1 $ot.P95) | $os30TotalCvCell | $(Round2 ($rt.Median / $ot.Median)) |")
    }
    $lines.Add("")
    $lines.Add("### 各探针耗时（ms，主指标=中位数）")
    $lines.Add("")
    if ($Regression) {
        $lines.Add("| Probe | RegC 中位数 | RegC CV% |")
        $lines.Add("|---|---|---|")
        foreach ($k in ($face.Regc.Probes.Keys | Sort-Object)) {
            $s = $face.Regc.Probes[$k]
            $cvCell = "$(Round1 $s.CvPercent)%$(if ($s.CvPercent -gt $MaxCvPercent) { ' ⚠' } else { '' })"
            $lines.Add("| $k | $(Round3 $s.Median) | $cvCell |")
        }
    } else {
        $lines.Add("| Probe | RegC 中位数 | RegC CV% | os30 中位数 | os30 CV% | ratio (中位数) |")
        $lines.Add("|---|---|---|---|---|---|")
        foreach ($k in ($face.Os30.Probes.Keys | Sort-Object)) {
            $o = $face.Os30.Probes[$k]
            $r = if ($face.Regc.Probes.ContainsKey($k)) { $face.Regc.Probes[$k] } else { $null }
            $rMedian = if ($r -ne $null) { $r.Median } else { 0.0 }
            $rCv = if ($r -ne $null) { $r.CvPercent } else { 0.0 }
            $rCell = "$(Round1 $rCv)%$(if ($rCv -gt $MaxCvPercent) { ' ⚠' } else { '' })"
            $oCell = "$(Round1 $o.CvPercent)%$(if ($o.CvPercent -gt $MaxCvPercent) { ' ⚠' } else { '' })"
            $rr = if ($o.Median -gt 0) { Round2 ($rMedian / $o.Median) } else { "-" }
            $lines.Add("| $k | $(Round3 $rMedian) | $rCell | $(Round3 $o.Median) | $oCell | $rr |")
        }
    }
    $lines.Add("")
}

$lines.Add("## 方法论说明（issue #887 修尺子）")
$lines.Add("")
$lines.Add("- 预热 $WarmupRuns 轮（独立 JVM，不计入统计）+ $Runs 轮计量（独立 JVM 冷启动），多 JDK 时同一会话顺序完成。")
$lines.Add("- 主指标为中位数（对 60~203ms 一类长尾抖动稳健）；均值仅供参照，不再用于结论。")
$lines.Add("- p95 为 nearest-rank 口径（$Runs 轮中第 [ceil(0.95×$Runs)] 位）；方差/标准差为无偏估计（n-1）。")
$lines.Add("- CV% = 标准差/均值；超过 $MaxCvPercent% 的行标记 ⚠，并在文首汇总为高方差存疑项。")
$lines.Add("- 原始样本与统计面同步落 CSV：results/ 下 *-runs.csv（逐轮明细）与 *-stats.csv（统计汇总）。")
$lines.Add("- 历史教训：#887 中单次 72.7 vs 71ms 的假打平，在预热+9 轮中位数口径下实为 ~1.6x 差距（53 vs 83ms，JDK25）。")
# 评审 N4（#887）：stamp 仅到日期，UpdateBaseline 与 Regression 同日产物互相覆盖，
# 基线中位数失去 CSV 对应留档——模式入名分离。
$outTag = if ($UpdateBaseline) { "updatebaseline" } else { "regression" }
$reportPath = Join-Path $reportDir "$stamp-jvm-obf-tester-os30-$outTag.md"
Set-Content -Path $reportPath -Value ($lines -join [Environment]::NewLine) -Encoding UTF8

# ---- CSV 落盘（#887：结果落 CSV/MD 表） ----
# runs CSV：逐轮原始样本（long 格式，一行 = JDK×产物×轮次×探针）。
$runsCsv = New-Object System.Collections.Generic.List[string]
$runsCsv.Add("jdk,artifact,run,total_ms,probe,time_ms,status")
foreach ($face in $faces) {
    $tag = $face.Ctx.Tag
    $regcM = $face.RegcMeasurement
    for ($i = 0; $i -lt $regcM.Totals.Count; $i++) {
        $map = $regcM.Maps[$i]
        foreach ($k in ($map.Keys | Sort-Object)) {
            $runsCsv.Add("$tag,regc,$($i + 1),$($regcM.Totals[$i]),$k,$($map[$k].Time),$($map[$k].Status)")
        }
    }
    if ($null -ne $face.Os30Measurement) {
        $os30M = $face.Os30Measurement
        for ($i = 0; $i -lt $os30M.Totals.Count; $i++) {
            $map = $os30M.Maps[$i]
            foreach ($k in ($map.Keys | Sort-Object)) {
                $runsCsv.Add("$tag,os30,$($i + 1),$($os30M.Totals[$i]),$k,$($map[$k].Time),$($map[$k].Status)")
            }
        }
    }
}
$runsCsvPath = Join-Path $reportDir "$stamp-jvm-obf-tester-os30-runs.csv"
Set-Content -Path $runsCsvPath -Value ($runsCsv -join [Environment]::NewLine) -Encoding UTF8

# stats CSV：统计汇总（一行 = JDK×产物×探针，total 记为 __total__）。
function Format-StatsRow([string]$Tag, [string]$Artifact, [string]$ProbeName, [object]$Stats) {
    return "$Tag,$Artifact,$ProbeName,$(Round3 $Stats.Median),$(Round3 $Stats.Min),$(Round3 $Stats.Max),$(Round3 $Stats.P95),$(Round3 $Stats.Mean),$(Round3 $Stats.StdDev),$(Round3 $Stats.Variance),$(Round2 $Stats.CvPercent),$($Stats.Count)"
}
$statsCsv = New-Object System.Collections.Generic.List[string]
$statsCsv.Add("jdk,artifact,probe,median_ms,min_ms,max_ms,p95_ms,mean_ms,stddev_ms,variance_ms2,cv_percent,samples")
foreach ($face in $faces) {
    $tag = $face.Ctx.Tag
    $statsCsv.Add((Format-StatsRow $tag "regc" "__total__" $face.Regc.Total))
    foreach ($k in ($face.Regc.Probes.Keys | Sort-Object)) {
        $statsCsv.Add((Format-StatsRow $tag "regc" $k $face.Regc.Probes[$k]))
    }
    if ($null -ne $face.Os30) {
        $statsCsv.Add((Format-StatsRow $tag "os30" "__total__" $face.Os30.Total))
        foreach ($k in ($face.Os30.Probes.Keys | Sort-Object)) {
            $statsCsv.Add((Format-StatsRow $tag "os30" $k $face.Os30.Probes[$k]))
        }
    }
}
$statsCsvPath = Join-Path $reportDir "$stamp-jvm-obf-tester-os30-stats.csv"
Set-Content -Path $statsCsvPath -Value ($statsCsv -join [Environment]::NewLine) -Encoding UTF8

# 控制台输出
foreach ($face in $faces) {
    $rt = $face.Regc.Total
    "=== [$($face.Ctx.Tag)] RegC total ($Runs runs, median): $(Round1 $rt.Median) ms  min~max=$(Round1 $rt.Min)~$(Round1 $rt.Max)  p95=$(Round1 $rt.P95)  mean=$(Round1 $rt.Mean)  cv=$(Round1 $rt.CvPercent)%"
    if (-not $Regression) {
        $ot = $face.Os30.Total
        "=== [$($face.Ctx.Tag)] os30 total ($Runs runs, median): $(Round1 $ot.Median) ms  min~max=$(Round1 $ot.Min)~$(Round1 $ot.Max)  p95=$(Round1 $ot.P95)  mean=$(Round1 $ot.Mean)  cv=$(Round1 $ot.CvPercent)%"
        "=== [$($face.Ctx.Tag)] ratio regc/os30 (median): $(Round2 ($rt.Median / $ot.Median))"
        "{0,-32} {1,12} {2,8} {3,12} {4,8} {5,8}" -f "Probe", "regc-med(ms)", "regc-cv", "os30-med(ms)", "os30-cv", "ratio"
        foreach ($k in ($face.Os30.Probes.Keys | Sort-Object)) {
            $o = $face.Os30.Probes[$k]
            $r = if ($face.Regc.Probes.ContainsKey($k)) { $face.Regc.Probes[$k] } else { $null }
            $rMedian = if ($r -ne $null) { $r.Median } else { 0.0 }
            $rCv = if ($r -ne $null) { $r.CvPercent } else { 0.0 }
            $rr = if ($o.Median -gt 0) { Round2 ($rMedian / $o.Median) } else { "-" }
            "{0,-32} {1,12} {2,8} {3,12} {4,8} {5,8}" -f $k, (Round3 $rMedian), "$(Round1 $rCv)%", (Round3 $o.Median), "$(Round1 $o.CvPercent)%", $rr
        }
    } else {
        "{0,-32} {1,12} {2,8}" -f "Probe", "regc-med(ms)", "regc-cv"
        foreach ($k in ($face.Regc.Probes.Keys | Sort-Object)) {
            $s = $face.Regc.Probes[$k]
            "{0,-32} {1,12} {2,8}" -f $k, (Round3 $s.Median), "$(Round1 $s.CvPercent)%"
        }
    }
}
if ($highVariance.Count -gt 0) {
    "=== [高方差警告] 以下样本 CV% > $MaxCvPercent%，结果存疑（详见报告）:"
    $highVariance | ForEach-Object { "  - $_" }
}
"`n报告已写入: $reportPath"
"CSV 已写入: $runsCsvPath"
"CSV 已写入: $statsCsvPath"

# ---- 7. 基线维护与回归断言（#820 步骤 5；#887 起主指标改中位数；多 JDK 取第一个） ----
if ($Regression -or $UpdateBaseline) {
    if (-not (Test-Path $BaselineFile)) { throw "基线文件未找到: $BaselineFile" }
    $baseline = Get-Content $BaselineFile -Raw | ConvertFrom-Json
    $firstFace = $faces[0]
    $firstAudit = $audit

    if ($UpdateBaseline) {
        # meta 字段统一走 Add-Member -Force：旧基线被 PS5.1 ErrorRecord 污染、
        # 修复后可能缺 warmupRuns/metric 等键——PSCustomObject 直接赋值新属性
        # 会抛 "property cannot be found"。Force=存在则覆写、缺失则新增。
        $baseline.meta | Add-Member -NotePropertyName generated -NotePropertyValue $stamp -Force
        # java -version 走 stderr，PS5.1 下 `& $java -version 2>&1` 会把
        # ErrorRecord 对象灌进 meta.jdk（旧基线已踩坑）——统一走 cmd /c 取纯文本。
        $baseline.meta | Add-Member -NotePropertyName jdk -NotePropertyValue $firstFace.Ctx.Version -Force
        $baseline.meta | Add-Member -NotePropertyName runs -NotePropertyValue $Runs -Force
        $baseline.meta | Add-Member -NotePropertyName warmupRuns -NotePropertyValue $WarmupRuns -Force
        $baseline.meta | Add-Member -NotePropertyName metric -NotePropertyValue "median" -Force
        $baseline.meta | Add-Member -NotePropertyName methodology -NotePropertyValue "预热 ${WarmupRuns} 轮（不计入统计）+ ${Runs} 轮计量取中位数；CV%>${MaxCvPercent}% 标记高方差存疑（#887）；覆盖率审计 javap 验证 ACC_NATIVE" -Force
        # 基线主指标：中位数（#887 前的历史基线为均值口径，更新基线时随之迁移）。
        # 同 meta：旧 schema 可能缺 totalCvPercent 等新键，统一 Add-Member -Force。
        $baseline.baseline | Add-Member -NotePropertyName totalMs -NotePropertyValue (Round1 $firstFace.Regc.Total.Median) -Force
        $baseline.baseline | Add-Member -NotePropertyName totalCvPercent -NotePropertyValue (Round1 $firstFace.Regc.Total.CvPercent) -Force
        if ($null -eq $baseline.baseline.coverage) {
            $baseline.baseline | Add-Member -NotePropertyName coverage -NotePropertyValue ([pscustomobject]@{}) -Force
        }
        $baseline.baseline.coverage | Add-Member -NotePropertyName native -NotePropertyValue $firstAudit.Native -Force
        $baseline.baseline.coverage | Add-Member -NotePropertyName executable -NotePropertyValue $firstAudit.Executable -Force
        $baseline.baseline.coverage | Add-Member -NotePropertyName percent -NotePropertyValue $firstAudit.Percent -Force
        # 评审 N5（#887）：schema 缺 probesMs 键时直赋值在 PS5.1 抛"找不到属性"，
        # 与其他键统一走 Add-Member -Force；已有键（含旧 JSON 的 PSCustomObject）则整体替换。
        if ($null -eq $baseline.baseline.probesMs) {
            $baseline.baseline | Add-Member -NotePropertyName probesMs -NotePropertyValue ([ordered]@{}) -Force
        }
        else {
            $baseline.baseline.probesMs = [ordered]@{}
        }
        foreach ($k in ($firstFace.Regc.Probes.Keys | Sort-Object)) {
            # OrderedDictionary 必须用索引器写键；Add-Member 只加 note property，
            # ConvertTo-Json 序列化时会整段丢失（曾致基线 probesMs 为空对象）。
            $baseline.baseline.probesMs[$k] = (Round3 $firstFace.Regc.Probes[$k].Median)
        }
        $baseline | ConvertTo-Json -Depth 6 | Set-Content $BaselineFile -Encoding UTF8
        Write-Host "`n基线已更新: $BaselineFile"
    }

    if ($Regression) {
        $errors = New-Object System.Collections.Generic.List[string]
        # 数据完整性门（fail-closed，#1183）：探针表为空或 total=0 说明产物根本没
        # 跑起来/输出未被解析（实例：生成 NativeLoader 字节码 VerifyError 致 main
        # clinit 崩，13-probe 表为空、total=0，功能门/性能门空转出"假绿"）——先于
        # 功能门判失败并给排查入口。
        if ($firstFace.Regc.Probes.Keys.Count -eq 0) {
            $errors.Add("探针表为空：产物运行输出未解析到任何 probe 行（检查产物可运行性与输出格式）")
        }
        $regcTotalRaw = $firstFace.Regc.Total.Median
        if ($regcTotalRaw -le 0) {
            $errors.Add("RegC total 中位数=${regcTotalRaw}ms：total 未解析到有效样本（Total time 行缺失）")
        }
        # 功能门：13-probe 全部 Passed
        foreach ($name in $firstFace.Regc.Probes.Keys) {
            $failedRun = $firstFace.RegcMeasurement.Maps | Where-Object { $_.$name.Status -ne "Passed" } | Select-Object -First 1
            if ($failedRun) { $errors.Add("probe '$name' 未通过（功能门）") }
        }
        # 覆盖率门：native 比例不低于基线 × MinCoverageRatio
        $covFloor = [math]::Round($baseline.baseline.coverage.percent * $MinCoverageRatio, 1)
        if ($firstAudit.Percent -lt $covFloor) {
            $errors.Add("覆盖率退化: $($firstAudit.Percent)% < 基线 $($baseline.baseline.coverage.percent)% × $MinCoverageRatio = $covFloor%")
        }
        # 总耗时门：RegC total 中位数不高于基线 × MaxTotalRatio
        $totalCeil = [math]::Round($baseline.baseline.totalMs * $MaxTotalRatio, 1)
        $regcTotalMedian = Round1 $firstFace.Regc.Total.Median
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
        Write-Host "`n[回归通过] RegC total 中位数=${regcTotalMedian}ms (≤ ${totalCeil}ms)，覆盖率 $($firstAudit.Percent)% (≥ $covFloor%)，13-probe 全过$varianceNote"
    }
}
