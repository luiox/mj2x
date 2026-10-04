# ============================================================================
# bootstrap_probe.ps1 — mj2x runtime bootstrap 四段分解微探针（issue #1183）。
#
# 对一个 RegC 转换产物 jar（含嵌入 native DLL）做 bootstrap 分段计量：
#   段 1 提取（extract 复刻计时；extract_new 为 #1183 改造后加载器同构复刻）/
#   段 2 加载（load: System.load 含 JNI_OnLoad）
#   段 2' 加载减 Loader 定义（load_predef）/ 段 3 逐类注册（reg_k，k 可缩放）
#   段 4 Loader 类定义 = load − load_predef / 总和锚点（full，污染 jar 端到端）。
#
# 方法论（沿 #887/#1161）：每轮独立冷 JVM，段内 System.nanoTime；预热
# -WarmupRuns 轮不计入统计，-Runs 轮计量取中位数，附 min~max/p95(nearest-rank)/CV%。
#
# 用法（双 JDK 逐个调用）：
#   pwsh -File mj2x/benchmark/bootstrap_probe/bootstrap_probe.ps1 `
#     -JdkHome "C:\Program Files\Microsoft\jdk-21.0.11.10-hotspot" `
#     -Jar test/jvm-obf-tester/jvm-obf-tester-regc.jar `
#     -Runs 9 -WarmupRuns 3
# 输出：<OutDir>/bootstrap-decomp-<jdktag>.csv + 控制台表。
# ============================================================================
param(
    [string]$JdkHome = "",
    [Parameter(Mandatory = $true)][string]$Jar,      # RegC 转换产物（未 stub，污染 jar）
    [int]$Runs = 9,
    [int]$WarmupRuns = 3,
    [string]$OutDir = "",                             # 默认 <repo>/test/jvm-obf-tester/bootstrap-probe
    [string]$KList = "1,4,13",                        # reg_k 缩放序列
    [int]$Rebuild = 1                                 # 1=重新编译探针类与 stub jar
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

# ---- JDK ----
if ($JdkHome -eq "") {
    if ($env:JAVA_HOME) { $JdkHome = $env:JAVA_HOME }
    else { throw "未指定 -JdkHome 且 JAVA_HOME 未设置" }
}
$java  = Join-Path $JdkHome "bin\java.exe"
$javac = Join-Path $JdkHome "bin\javac.exe"
$jarTool = Join-Path $JdkHome "bin\jar.exe"
if (-not (Test-Path $java)) { throw "JDK 无效: $JdkHome" }

# jdk 标签（`java -version` 首行主版本号，跨 JDK 列名用）
$versionLine = [string]((cmd /c "`"$java`" -version 2>&1" | Select-Object -First 1))
$major = if ($versionLine -match 'version "(\d+)') { $Matches[1] } else { "unknown" }
$jdkTag = "jdk$major"

# ---- 输出目录（测试产物只写 test/） ----
if ($OutDir -eq "") { $OutDir = Join-Path $repo "test\jvm-obf-tester\bootstrap-probe" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

# ---- 探针类编译 + stub jar 准备 ----
$probeDir = Join-Path $OutDir "classes"
$stubDir  = Join-Path $OutDir "stub"
if ($Rebuild -eq 1) {
    Remove-Item -Recurse -Force $probeDir, $stubDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $probeDir | Out-Null
    # BootstrapProbe（默认兼容级，由当前 javac 编译）
    & $javac -d $probeDir (Join-Path $PSScriptRoot "BootstrapProbe.java") 2>&1 | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0) { throw "BootstrapProbe 编译失败" }
    # NativeLoaderStub → 独立输出目录 → 替换进 jar 副本
    $stubSrc  = Join-Path $OutDir "stub-src"
    $stubCls  = Join-Path $stubSrc "classes"
    New-Item -ItemType Directory -Force $stubCls | Out-Null
    Copy-Item (Join-Path $PSScriptRoot "NativeLoaderStub.java") (Join-Path $stubSrc "NativeLoader.java") -Force
    & $javac -d $stubCls (Join-Path $stubSrc "NativeLoader.java") 2>&1 | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0) { throw "NativeLoaderStub 编译失败" }
    New-Item -ItemType Directory -Force $stubDir | Out-Null
    $stubJar = Join-Path $stubDir "probe-stub.jar"
    Copy-Item $Jar $stubJar -Force
    Push-Location $stubCls
    & $jarTool uf $stubJar "mj2x/runtime/NativeLoader.class" 2>&1 | Out-Null
    Pop-Location
    if ($LASTEXITCODE -ne 0) { throw "stub jar 更新失败" }
}
$stubJar = Join-Path $stubDir "probe-stub.jar"
if (-not (Test-Path $stubJar)) { throw "stub jar 缺失: $stubJar（-Rebuild 1 重跑）" }
$Jar = (Resolve-Path $Jar).Path   # 相对路径先固定：后续 Push-Location 会改变解析基准

# ---- 枚举转换类（含 ACC_NATIVE 方法的非 mj2x 类，javap 口径同覆盖率审计） ----
$auditDir = Join-Path $OutDir "audit-classes"
Remove-Item -Recurse -Force $auditDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $auditDir | Out-Null
Push-Location $auditDir
& $jarTool xf (Resolve-Path $Jar).Path 2>&1 | Out-Null
Pop-Location
$transformed = @()
Get-ChildItem -Path $auditDir -Recurse -Filter "*.class" | ForEach-Object {
    $rel = $_.FullName.Substring($auditDir.Length + 1) -replace "\\", "/"
    if ($rel -like "mj2x/*") { return }    # 桥接/NativeLoader 类不属于转换类集合
    $className = $rel -replace "\.class$", "" -replace "/", "."
    $javapOut = & (Join-Path $JdkHome "bin\javap.exe") -p -classpath $auditDir $className 2>&1
    $nativeCount = 0
    foreach ($line in $javapOut) {
        if ($line -match '^\s+[^=]*\([^;]*\);$' -and $line -match '\bnative\b') { $nativeCount++ }
    }
    if ($nativeCount -gt 0) {
        $transformed += [pscustomobject]@{ Class = $className; NativeMethods = $nativeCount }
    }
}
$transformed = $transformed | Sort-Object Class
if ($transformed.Count -eq 0) { throw "产物中未发现含 native 方法的转换类: $Jar" }
$classList = @($transformed | ForEach-Object { $_.Class })
$totalNativeMethods = ($transformed | Measure-Object -Sum NativeMethods).Sum
Write-Host "转换类 $($transformed.Count) 个 / native 方法 $totalNativeMethods 个（DLL 资源逐类注册）"

# ---- 资源路径（META-INF/mj2x/native/*.dll[.enc]） ----
$jarEntries = & $jarTool tf (Resolve-Path $Jar).Path 2>&1
$resourcePath = [string]($jarEntries |
    Where-Object { $_ -like "META-INF/mj2x/native/*" -and -not $_.EndsWith("/") } |
    Select-Object -First 1)
if ($resourcePath -eq "") { throw "产物无嵌入 native 资源: $Jar" }
$dllSize = ($auditDir | Get-ChildItem -Recurse -Filter "*.dll*" | Measure-Object -Sum Length).Sum
Write-Host "native 资源：$resourcePath（$dllSize 字节）"

# ---- 统计（中位数主指标 + min~max/p95 nearest-rank/CV%，同 #887 口径） ----
function Get-RunStats([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $n = $sorted.Count
    $median = if ($n -eq 0) { 0.0 }
              elseif ($n % 2 -eq 1) { [double]$sorted[[int](($n - 1) / 2)] }
              else { ([double]$sorted[[int]($n / 2 - 1)] + [double]$sorted[[int]($n / 2)]) / 2.0 }
    $mean = if ($n -gt 0) { [double]($sorted | Measure-Object -Average).Average } else { 0.0 }
    $min = if ($n -gt 0) { [double]$sorted[0] } else { 0.0 }
    $max = if ($n -gt 0) { [double]$sorted[$n - 1] } else { 0.0 }
    $p95 = if ($n -gt 0) { [double]$sorted[[math]::Min($n - 1, [int][math]::Ceiling(0.95 * $n) - 1)] } else { 0.0 }
    $variance = 0.0
    if ($n -gt 1) {
        $sumSq = 0.0
        foreach ($v in $sorted) { $sumSq += ($v - $mean) * ($v - $mean) }
        $variance = $sumSq / ($n - 1)
    }
    $stddev = [math]::Sqrt($variance)
    $cv = if ($mean -gt 0) { 100.0 * $stddev / $mean } else { 0.0 }
    return [pscustomobject]@{ Median = $median; Mean = $mean; Min = $min; Max = $max; P95 = $p95; StdDev = $stddev; CvPercent = $cv; Count = $n }
}

# ---- 单模式计量：预热 WarmupRuns 轮 + Runs 轮计量（每轮独立冷 JVM） ----
function Measure-Mode([string]$Mode, [string]$TargetJar, [string[]]$ExtraProps, [int]$K) {
    $props = @() + $ExtraProps
    if ($K -gt 0) { $props += "-Dmj2x.probe.k=$K" }
    for ($i = 1; $i -le $WarmupRuns; $i++) {
        & $java @props -cp "$TargetJar;$probeDir" BootstrapProbe $Mode $resourcePath @classList 2>&1 | Out-Null
    }
    $samples = New-Object System.Collections.Generic.List[double]
    for ($i = 1; $i -le $Runs; $i++) {
        $out = & $java @props -cp "$TargetJar;$probeDir" BootstrapProbe $Mode $resourcePath @classList 2>&1
        $hit = $false
        foreach ($line in $out) {
            if ($line -match '^SEG\s+(\S+)\s+(\d+)\s*$') {
                if ($Matches[1] -eq $Mode) {
                    $samples.Add([double]$Matches[2] / 1000000.0)   # ns → ms
                    $hit = $true
                }
            }
        }
        if (-not $hit) { throw "模式 $Mode 第 $i 轮未产出 SEG 行：$(($out -join ' | '))" }
    }
    return Get-RunStats $samples.ToArray()
}

# ---- 逐模式跑 ----
$kSeq = @($KList -split "," | ForEach-Object { [int]$_.Trim() })
$results = @()
$face = [ordered]@{
    "extract"     = @{ Jar = $Jar;     Props = @();                      K = 0 }
    "extract_new" = @{ Jar = $Jar;     Props = @();                      K = 0 }
    "load"        = @{ Jar = $stubJar; Props = @("-Dmj2x.probe.skip=1"); K = 0 }
    "load_predef" = @{ Jar = $stubJar; Props = @("-Dmj2x.probe.skip=1"); K = 0 }
}
foreach ($k in $kSeq) { $face["reg_$k"] = @{ Jar = $stubJar; Props = @("-Dmj2x.probe.skip=1"); K = $k } }
$face["full"] = @{ Jar = $Jar; Props = @(); K = 0 }

foreach ($mode in $face.Keys) {
    $cfg = $face[$mode]
    Write-Host "==> $mode（$($Runs + $WarmupRuns) 轮独立 JVM）..."
    $stats = Measure-Mode -Mode $mode -TargetJar $cfg.Jar -ExtraProps $cfg.Props -K $cfg.K
    $results += [pscustomobject]@{
        jdk = $jdkTag; mode = $mode; medianMs = [math]::Round($stats.Median, 3)
        minMs = [math]::Round($stats.Min, 3); maxMs = [math]::Round($stats.Max, 3)
        p95Ms = [math]::Round($stats.P95, 3); cvPercent = [math]::Round($stats.CvPercent, 1)
        runs = $stats.Count
    }
}

# ---- 汇总表（控制台 + CSV） ----
$table = $results | Format-Table jdk, mode, medianMs, minMs, maxMs, p95Ms, cvPercent, runs -AutoSize | Out-String
Write-Host $table
$csvPath = Join-Path $OutDir "bootstrap-decomp-$jdkTag.csv"
$results | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8
Write-Host "CSV: $csvPath"
$metaPath = Join-Path $OutDir "bootstrap-decomp-$jdkTag.meta.txt"
@"
jdkTag=$jdkTag
version=$versionLine
jar=$Jar
resource=$resourcePath
dllBytes=$dllSize
transformedClasses=$($transformed.Count)
nativeMethods=$totalNativeMethods
runs=$Runs
warmupRuns=$WarmupRuns
kList=$KList
"@ | Set-Content -Path $metaPath -Encoding UTF8
Write-Host "meta: $metaPath"
