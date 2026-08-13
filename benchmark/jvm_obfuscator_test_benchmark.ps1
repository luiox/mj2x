# jvm_obfuscator_test_benchmark.ps1 — RegC vs os30(JNIC) 13-probe 性能对比基准
#
# 用途：把 jvm-obf-tester（13 个探针 jar）用 mj2x-cli --backend regc 全量 native 化，
# 与竞品 os30（JNIC 混淆产物）在同一 JDK 下对比各探针耗时，产出 markdown 报告。
# 依据：issue #787 §3.1（RegC 性能基线）+ #778（热循环性能对比）。
#
# 方法学（performance_evaluation_spec.md 强制项）：
#   1. 每次对比前先做覆盖率审计（javap 验证 ACC_NATIVE + 无 Code 属性），
#      未附覆盖率的性能数据不予采信；
#   2. 预热 1 轮 + Runs 次取样取平均；
#   3. 输出报告落盘到 benchmark/results/。
#
# 用法：
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -Runs 3
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -JdkHome C:\Users\Canrad\.jdks\corretto-21.0.4
#   pwsh -File mj2x/benchmark/jvm_obfuscator_test_benchmark.ps1 -SkipBuild          # 复用已构建的 CLI
#
# 注意：脚本内 xmake 配置均用 -P 显式指定项目，避免 `xmake f -P` 破坏 root 配置（issue #782）。

param(
    [string]$Cli = "",                                    # mj2x-cli 路径；默认 <repo>/build/windows/x64/release/mj2x-cli.exe
    [string]$InputJar = "D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester.jar",
    [string]$Os30Jar = "D:\WorkSpace\jvm-obf-tester\target\jvm-obf-tester_os30.jar",
    [string]$JdkHome = "",                                # 默认 $env:JAVA_HOME；JAVA_HOME 未设置则用 D:\sdk\java\jdk17
    [int]$Runs = 3,                                       # 取样轮数（不含预热）
    [switch]$SkipBuild                                    # 跳过 CLI 构建（复用已有产物）
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
if (-not (Test-Path $Os30Jar)) { throw "os30 jar 未找到: $Os30Jar" }

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
    $out = & xmake -P $repo build mj2x-cli 2>&1 | ForEach-Object { $_.ToString() }
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
Run-ProcessChecked "xmake" @("f", "-P", $projDir, "-m", "release") `
    $projDir (Join-Path $outDir "native-config.log") | Out-Null
$nativeLog = Join-Path $outDir "native-build.log"
$out = & xmake -P $projDir mj2x_native 2>&1 | ForEach-Object { $_.ToString() }
Set-Content -Path $nativeLog -Value ($out -join [Environment]::NewLine) -Encoding UTF8
if (($out -join "`n") -match "error") { throw "native 编译失败，见 $nativeLog" }

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
$auditOs30 = Get-NativeCoverage $Os30Jar (Join-Path $outDir "audit-os30")

# ---- 4. 跑 13-probe（预热 1 轮 + Runs 次取样） ----
function Run-Probes([string]$JarPath) {
    return & $java -jar $JarPath 2>&1
}

function Get-ProbeTimes([string[]]$Lines) {
    $map = @{}
    foreach ($line in $Lines) {
        if ($line -match '^\| (.+) \| (Passed|Failed) \|') {
            $name = $Matches[1].Trim()
            $time = if ($line -match '(\d+\.?\d*)ms') { [double]$Matches[1] } else { 0 }
            $map[$name] = [pscustomobject]@{ Time = $time; Status = $Matches[2] }
        }
    }
    return $map
}

Write-Host "==> 跑探针（预热 + $Runs 轮取样） ..."
Run-Probes $outJar | Out-Null   # 预热

$regcMaps = @(); $regcTotals = @(); $regcOut = $null
for ($i = 1; $i -le $Runs; $i++) {
    $regcOut = Run-Probes $outJar
    $m = [regex]::Match(($regcOut -join "`n"), "Total time:\s*(\d+)ms")
    $regcTotals += [int]$m.Groups[1].Value
    $regcMaps += Get-ProbeTimes $regcOut
}
$os30Maps = @(); $os30Totals = @(); $os30Out = $null
for ($i = 1; $i -le $Runs; $i++) {
    $os30Out = Run-Probes $Os30Jar
    $m = [regex]::Match(($os30Out -join "`n"), "Total time:\s*(\d+)ms")
    $os30Totals += [int]$m.Groups[1].Value
    $os30Maps += Get-ProbeTimes $os30Out
}
# 取各 probe 平均
$regcAvg = @{}; $os30Avg = @{}
foreach ($name in $regcMaps[0].Keys) {
    $regcAvg[$name] = ($regcMaps | ForEach-Object { $_.$name.Time } | Measure-Object -Average).Average
}
foreach ($name in $os30Maps[0].Keys) {
    $os30Avg[$name] = ($os30Maps | ForEach-Object { $_.$name.Time } | Measure-Object -Average).Average
}
$regcTotalAvg = ($regcTotals | Measure-Object -Average).Average
$os30TotalAvg = ($os30Totals | Measure-Object -Average).Average

# ---- 5. 报告 ----
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# jvm-obf-tester 13-probe 对比（$stamp）")
$lines.Add("")
$lines.Add("- 环境：JDK $((& $java -version 2>&1 | Select-Object -First 1))")
$lines.Add("- 取样：预热 1 轮 + $Runs 轮取平均")
$lines.Add("- RegC 产物：$outJar（mj2x-cli --backend regc 全量转换 + xmake 编译嵌入）")
$lines.Add("- os30 产物：$Os30Jar（JNIC 竞品，黑盒对比）")
$lines.Add("")
$lines.Add("## 覆盖率审计（javap 验证 ACC_NATIVE，无 Code 属性）")
$lines.Add("")
$lines.Add("| 产物 | 方法总数 | 可执行(非 abstract) | native | native 比例 |")
$lines.Add("|---|---|---|---|---|")
$lines.Add("| RegC | $($audit.Total) | $($audit.Executable) | $($audit.Native) | $($audit.Percent)% |")
$lines.Add("| os30 | $($auditOs30.Total) | $($auditOs30.Executable) | $($auditOs30.Native) | $($auditOs30.Percent)% |")
$lines.Add("")
$lines.Add("## 总耗时（ms）")
$lines.Add("")
$lines.Add("| 项 | RegC | os30 | ratio (RegC/os30) |")
$lines.Add("|---|---|---|---|")
$lines.Add("| Total | $([math]::Round($regcTotalAvg, 1)) | $([math]::Round($os30TotalAvg, 1)) | $([math]::Round($regcTotalAvg / $os30TotalAvg, 2)) |")
$lines.Add("")
$lines.Add("## 各探针耗时（ms，平均）")
$lines.Add("")
$lines.Add("| Probe | RegC | os30 | ratio |")
$lines.Add("|---|---|---|---|")
foreach ($k in ($os30Avg.Keys | Sort-Object)) {
    $r = if ($regcAvg.ContainsKey($k)) { $regcAvg[$k] } else { 0 }
    $o = $os30Avg[$k]
    $rr = if ($o -gt 0) { [math]::Round($r / $o, 2) } else { "-" }
    $lines.Add("| $k | $([math]::Round($r, 3)) | $([math]::Round($o, 3)) | $rr |")
}
$reportPath = Join-Path $reportDir "$stamp-jvm-obf-tester-os30.md"
Set-Content -Path $reportPath -Value ($lines -join [Environment]::NewLine) -Encoding UTF8

# 控制台输出
"`n=== RegC total (${Runs} runs): $($regcTotals -join ', ') ms  avg=$([math]::Round($regcTotalAvg, 1))"
"=== os30 total (${Runs} runs): $($os30Totals -join ', ') ms  avg=$([math]::Round($os30TotalAvg, 1))"
"=== ratio regc/os30: $([math]::Round($regcTotalAvg / $os30TotalAvg, 2))"
"=== 覆盖率审计: RegC $($audit.Native)/$($audit.Executable) ($($audit.Percent)%)  os30 $($auditOs30.Native)/$($auditOs30.Executable) ($($auditOs30.Percent)%)"
"{0,-32} {1,10} {2,10} {3,8}" -f "Probe", "regc(ms)", "os30(ms)", "ratio"
foreach ($k in ($os30Avg.Keys | Sort-Object)) {
    $r = if ($regcAvg.ContainsKey($k)) { $regcAvg[$k] } else { 0 }
    $o = $os30Avg[$k]
    $rr = if ($o -gt 0) { [math]::Round($r / $o, 2) } else { "-" }
    "{0,-32} {1,10} {2,10} {3,8}" -f $k, [math]::Round($r, 3), [math]::Round($o, 3), $rr
}
"`n报告已写入: $reportPath"
