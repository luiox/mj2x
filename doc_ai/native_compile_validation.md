# mj2x Native Compile Validation Record

> 本记录只保存实际执行的 native 编译证据。`unverified` 不等同于通过，也不以 fake tool
> 测试替代真实平台结果。

## 2026-07-23 Windows x64

- Host: Windows x64, JDK 17 (`JAVA_HOME=D:\sdk\java\jdk17`)
- MSVC: Visual Studio 2022 x64
- Clang: `D:\sdk\clang+llvm-18.1.8-x86_64-pc-windows-msvc\bin`
- Zig: `0.17.0-dev.986+f3544a707`
- xmake/CMake: PATH 中可用

| Toolchain | Source / mode | Real evidence | Result |
|---|---|---|---|
| MSVC | C file, RegC JAR, output path with spaces | `CliTest.CompileRegCWithMsvcSupportsOutputDirectoryContainingSpaces` | pass |
| MSVC | C pipe fallback, class | `CliTest.PipeCompileWithMsvcRemovesTemporaryCSource` | pass; MSVC uses cleaned temporary `.c`, not stdin |
| MSVC | RegC C++17 file, CRT C ABI | `CliTest.MsvcCompilesRegcCppSource` | pass |
| Clang | C pipe, class | `CliTest.PipeCompileWithClangDoesNotWriteCSource` | pass |
| Clang | C pipe, RegC JAR + embedded library | `CliTest.PipeCompileRegCJarEmbedsNativeLoaderAndLibraryWithClang` | pass |
| Clang++ | RegC C++17 file, CRT C ABI | `CliTest.ClangCompilesRegcCppSource` | pass |
| Clang | LLVM IR direct `.ll` link, loop/switch class + JVM baseline | `CliTest.LlvmClangAndLlcCompileLoopSwitchAndRunJvm` (`--llvm-tool clang`) | pass |
| llc + Clang | LLVM IR `.ll` → object → DLL, loop/switch class + JVM baseline | `CliTest.LlvmClangAndLlcCompileLoopSwitchAndRunJvm` (`--llvm-tool llc`) | pass |
| Zig | C file class + C pipe RegC JAR + JVM baseline | `CliTest.RealZigCompilesClassAndJarAndRunsJvm` | pass |
| Zig | RegC C++17 file class/JAR + JVM baseline | `CliTest.RealZigCompilesRegcCppClassAndRunsJvm` | pass |
| xmake | StackC project and RegC JAR temporary project | `CliTest.GenerateXmakeAndCompileBuildsNativeLibrary`, `CliTest.XmakeCompilerBuildsRegCJarAndCleansTemporaryProject` | pass |
| xmake | RegC C++17 `.cpp` project | `CliTest.XmakeCompilesRegcCppProject` | pass |
| CMake | RegC C++17 `.cpp` project + copied CRT | `CliTest.GenerateCMakeProjectForRegcCppCopiesCrt` | pass |

The Windows run executed the nine-toolchain matrix with real toolchains. The complete filtered
test command passed 9/9. The separate C++ project regression suite passed 9/9, including CMake,
xmake, MSVC, Zig class/JAR JVM execution, configuration, and negative cases.

LLVM IR extension validation additionally passed 7/7 in one filtered local run: core scalar and
fail-closed tests, CLI `.ll` / JSON / negative-path tests, and the real clang plus `llc -> clang`
JVM loop/switch baseline test. LLVM IR does not currently claim CMake/xmake, pipe, GCC, Zig, Linux,
or macOS validation.

## Unverified Platforms And Tools

| Platform / tool | Status | Reason |
|---|---|---|
| Windows GCC / g++ | unverified | `gcc` and `g++` were not present on PATH. |
| Linux GCC / Clang / Zig / xmake | unverified | no Linux execution environment was available in this validation run. |
| macOS Clang / Zig / xmake | unverified | no macOS execution environment was available in this validation run. |

CI is responsible for Linux execution evidence after its runner has completed. This document must
be extended with a dated command and result before any platform moves from `unverified` to `pass`.
