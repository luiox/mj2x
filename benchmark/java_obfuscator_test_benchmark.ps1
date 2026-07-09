param(
    [int]$Runs = 5,
    [string]$InputJar = "",
    [string]$FixtureSourceDir = "mj2x/test_resource/benchmark/java_obfuscator_style/src",
    [string]$MainClass = "pack.Main",
    [string]$WorkDir = "test/java_obfuscator_test/benchmark-current"
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
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
    if ($LogPath) {
        Set-Content -Path $LogPath -Value $text -Encoding UTF8
    }
    if ($exitCode -ne 0) {
        throw "$FilePath exited with $exitCode. See $LogPath"
    }
    return $text
}

function Ensure-JdkTools() {
    if ([string]::IsNullOrWhiteSpace($env:JAVA_HOME)) {
        throw "JAVA_HOME is not set"
    }
    $jdkBin = Join-Path $env:JAVA_HOME "bin"
    if (Test-Path $jdkBin) {
        $paths = $env:PATH -split [System.IO.Path]::PathSeparator
        if ($paths -notcontains $jdkBin) {
            $env:PATH = $jdkBin + [System.IO.Path]::PathSeparator + $env:PATH
        }
    }
    foreach ($tool in @("java", "javac", "jar")) {
        if (!(Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "$tool was not found. Ensure JAVA_HOME points at a JDK and JAVA_HOME/bin is on PATH."
        }
    }
}

function Build-FixtureJar([string]$SourceDir, [string]$JarPath, [string]$ClassName, [string]$BuildDir) {
    if (!(Test-Path $SourceDir)) {
        throw "Fixture source dir not found: $SourceDir"
    }

    $classesDir = Join-Path $BuildDir "classes"
    New-Item -ItemType Directory -Force -Path $classesDir | Out-Null

    $sourceList = Join-Path $BuildDir "sources.txt"
    $sources = Get-ChildItem -LiteralPath $SourceDir -Recurse -File -Filter "*.java" |
        Sort-Object FullName |
        ForEach-Object { $_.FullName }
    if ($sources.Count -eq 0) {
        throw "No Java sources found under $SourceDir"
    }
    Set-Content -Path $sourceList -Value $sources -Encoding UTF8

    Run-ProcessChecked "javac" @("-encoding", "UTF-8", "-d", $classesDir, "@$sourceList") `
        $BuildDir (Join-Path $BuildDir "javac.log") | Out-Null

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $JarPath) | Out-Null
    Run-ProcessChecked "jar" @("--create", "--file", $JarPath, "--main-class", $ClassName, "-C", $classesDir, ".") `
        $BuildDir (Join-Path $BuildDir "jar-create.log") | Out-Null
}

function Measure-JarRun([string]$JarPath, [string]$Case, [int]$Run, [string]$RunDir) {
    $logPath = Join-Path $RunDir "$Case-run-$Run.log"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $output = Run-ProcessChecked "java" @("-noverify", "-jar", $JarPath) $RunDir $logPath
    $sw.Stop()

    $calcMs = ""
    $match = [regex]::Match($output, "Calc:\s*(\d+)ms")
    if ($match.Success) {
        $calcMs = $match.Groups[1].Value
    }

    $issues = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($output -split "`r?`n")) {
        if ($line -match "FAIL|ERROR") {
            $issues.Add($line.Trim())
        }
    }

    [pscustomobject]@{
        Case = $Case
        Run = $Run
        CalcMs = $calcMs
        ProcessMs = [int64]$sw.ElapsedMilliseconds
        Issues = [string]::Join(" | ", $issues)
        Log = $logPath
    }
}

function Find-Cli() {
    $candidates = @(
        "build/windows/x64/release/mj2x-cli.exe",
        "build/artifacts/windows/x64/release/mj2x/package/bin/mj2x-cli.exe",
        "build/linux/x86_64/release/mj2x-cli",
        "build/macosx/x86_64/release/mj2x-cli"
    )
    foreach ($candidate in $candidates) {
        $path = Resolve-RepoPath $candidate
        if (Test-Path $path) {
            return $path
        }
    }
    throw "mj2x-cli was not found after build"
}

function Find-NativeLibrary([string]$ProjectDir) {
    $names = @("mj2x_native.dll", "libmj2x_native.so", "libmj2x_native.dylib")
    foreach ($name in $names) {
        $found = Get-ChildItem -Path $ProjectDir -Recurse -File -Filter $name -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($found) {
            return $found.FullName
        }
    }
    throw "xmake native library was not found under $ProjectDir"
}

function Native-Extension() {
    if ($IsWindows -or $env:OS -eq "Windows_NT") { return ".dll" }
    if ($IsMacOS) { return ".dylib" }
    return ".so"
}

function Embed-NativeLibrary([string]$JarPath, [string]$NativeLibrary, [string]$EntryName, [string]$TempDir) {
    $entryPath = Join-Path $TempDir ($EntryName -replace "/", [System.IO.Path]::DirectorySeparatorChar)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $entryPath) | Out-Null
    Copy-Item -LiteralPath $NativeLibrary -Destination $entryPath -Force
    Run-ProcessChecked "jar" @("uf", $JarPath, $EntryName) $TempDir (Join-Path $TempDir "jar-update.log") | Out-Null
}

$repo = Get-Location
$fixtureSourcePath = Resolve-RepoPath $FixtureSourceDir
$workDirPath = Resolve-RepoPath $WorkDir
$fixtureBuildDir = Join-Path $workDirPath "fixture-build"
$inputJarPath = if ([string]::IsNullOrWhiteSpace($InputJar)) {
    Join-Path $fixtureBuildDir "TEST.jar"
} else {
    Resolve-RepoPath $InputJar
}
$baselineDir = Join-Path $workDirPath "baseline"
$regcDir = Join-Path $workDirPath "regc-full-xmake"
$projectDir = Join-Path $regcDir "native_project"
$outJar = Join-Path $regcDir "TEST-regc-full-xmake.jar"
$csvPath = Join-Path $workDirPath "perf-results.csv"
$summaryPath = Join-Path $workDirPath "summary.txt"

Remove-Item -Recurse -Force $workDirPath -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $baselineDir, $regcDir, $fixtureBuildDir | Out-Null

Ensure-JdkTools

if ([string]::IsNullOrWhiteSpace($InputJar)) {
    Build-FixtureJar $fixtureSourcePath $inputJarPath $MainClass $fixtureBuildDir
}
elseif (!(Test-Path $inputJarPath)) {
    throw "Input jar not found: $inputJarPath"
}

Run-ProcessChecked "xmake" @("build", "-P", $repo.Path, "mj2x-cli") $repo.Path (Join-Path $workDirPath "build-mj2x-cli.log") | Out-Null
$cli = Find-Cli

$generateSw = [System.Diagnostics.Stopwatch]::StartNew()
Run-ProcessChecked $cli @(
    "-i", $inputJarPath,
    "-o", $outJar,
    "--backend", "regc",
    "--generate-xmake", $projectDir
) $repo.Path (Join-Path $regcDir "generation.log") | Out-Null
$generateSw.Stop()

$buildSw = [System.Diagnostics.Stopwatch]::StartNew()
Run-ProcessChecked "xmake" @("f", "-P", $projectDir, "-m", "release") $repo.Path (Join-Path $regcDir "xmake-config.log") | Out-Null
Run-ProcessChecked "xmake" @("-P", $projectDir, "mj2x_native") $repo.Path (Join-Path $regcDir "xmake-build.log") | Out-Null
$buildSw.Stop()

$nativeLibrary = Find-NativeLibrary $projectDir
$nativeEntryName = "TEST-regc-full-xmake" + (Native-Extension)
Embed-NativeLibrary $outJar $nativeLibrary ("META-INF/mj2x/native/" + $nativeEntryName) (Join-Path $regcDir "jar_embed")

$rows = New-Object System.Collections.Generic.List[object]
for ($i = 1; $i -le $Runs; $i++) {
    $rows.Add((Measure-JarRun $inputJarPath "baseline" $i $baselineDir))
}
for ($i = 1; $i -le $Runs; $i++) {
    $rows.Add((Measure-JarRun $outJar "regc-full-xmake" $i $regcDir))
}

$rows | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8

$baselineAvg = ($rows | Where-Object { $_.Case -eq "baseline" -and $_.CalcMs -ne "" } |
    Measure-Object -Property CalcMs -Average).Average
$regcAvg = ($rows | Where-Object { $_.Case -eq "regc-full-xmake" -and $_.CalcMs -ne "" } |
    Measure-Object -Property CalcMs -Average).Average
$ratio = if ($baselineAvg -and $regcAvg) { $regcAvg / $baselineAvg } else { 0 }
$javaVersion = ((Run-ProcessChecked "java" @("-version") $repo.Path (Join-Path $workDirPath "java-version.log")) -split "`r?`n")[0]
$gitCommit = (Run-ProcessChecked "git" @("rev-parse", "--short", "HEAD") $repo.Path (Join-Path $workDirPath "git-rev.log")).Trim()

$summary = @(
    "input=$inputJarPath",
    "fixture_source=$fixtureSourcePath",
    "main_class=$MainClass",
    "runs=$Runs",
    "git_commit=$gitCommit",
    "java_version=$javaVersion",
    "generate_ms=$([int64]$generateSw.ElapsedMilliseconds)",
    "native_build_ms=$([int64]$buildSw.ElapsedMilliseconds)",
    "baseline_calc_avg_ms=$([math]::Round($baselineAvg, 2))",
    "regc_full_xmake_calc_avg_ms=$([math]::Round($regcAvg, 2))",
    "regc_vs_baseline_ratio=$([math]::Round($ratio, 3))",
    "csv=$csvPath"
)
Set-Content -Path $summaryPath -Value $summary -Encoding UTF8
$summary | ForEach-Object { Write-Host $_ }
