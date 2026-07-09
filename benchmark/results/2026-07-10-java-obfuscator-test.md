# 2026-07-10 JavaObfuscatorTest external benchmark

Input: external `JavaObfuscatorTest/bench/bin/TEST.jar` provided outside the
repository. The binary fixture is not committed.

Command:

```powershell
pwsh -ExecutionPolicy Bypass -File mj2x/benchmark/java_obfuscator_test_benchmark.ps1 `
  -InputJar E:\canrad\reverse\jvm\obf\JavaObfuscatorTest\bench\bin\TEST.jar `
  -Runs 5
```

Environment:

- repo commit before benchmark-line edits: `88fa609b`
- `JAVA_HOME`: `D:\sdk\java\jdk17`
- JDK tool discovery required adding `JAVA_HOME/bin` to `PATH`

Summary:

| metric | value |
|---|---:|
| runs | 5 |
| RegC generation | 55 ms |
| native xmake build | 4457 ms |
| baseline `Calc` average | 28 ms |
| RegC full-native `Calc` average | 779.8 ms |
| RegC / baseline ratio | 27.85 |

This result is a baseline for later JNI-cache, intrinsic, and runtime work. It
is intentionally recorded as an external benchmark note instead of committing
the third-party `TEST.jar`.
