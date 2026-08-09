# 2026-08-09 JavaObfuscatorTest external benchmark (post-fix)

Input: external `JavaObfuscatorTest/bench/bin/TEST.jar` provided outside the
repository. The binary fixture is not committed.

Command:

```powershell
pwsh -ExecutionPolicy Bypass -File mj2x/benchmark/java_obfuscator_test_benchmark.ps1 `
  -InputJar E:\canrad\reverse\jvm\obf\JavaObfuscatorTest\bench\bin\TEST.jar `
  -Runs 3
```

Environment:

- repo commit: `5547b5572` (SSA entry back-edge fix) + uncommitted fixes:
  - `init_clinit_mover_preprocessor.cpp`: clone 前预登记 label 映射（修复
    `<init>` 搬运后向前跳悬空引用，Window 构造器可转 native）
  - `libjbytecode LabelNode::clone`: ASM 语义（先查映射命中复用，不再无条件新建）
  - `regc_intrinsic_registry.cpp`: C ABI 声明包 `extern "C"`（修复 MSVC C++17
    链接失败 LNK2019）
  - `regc_runtime_template.cpp`: `_mj2x_regc_env`/`_mj2x_regc_ctx` 改线程局部
    （C++ thread_local / MSVC C __declspec(thread) / 其余 _Thread_local），
    修复多线程 native 并发时 env 互相覆盖导致的 StackOverflow
- `JAVA_HOME`: `D:\sdk\java\jdk17`

Summary:

| metric | value |
|---|---:|
| runs | 3 |
| RegC generation | 67 ms |
| native xmake build | 3042 ms |
| baseline `Calc` average | 30.33 ms |
| RegC full-native `Calc` average | 520.33 ms |
| RegC / baseline ratio | 17.154 |

Regression vs 2026-07-10 (commit `88fa609b`, ratio 27.85x):

| metric | 2026-07-10 | 2026-08-09 |
|---|---:|---:|
| native build | 4457 ms | 3042 ms |
| RegC `Calc` average | 779.8 ms | 520.33 ms |
| ratio | 27.85 | 17.15 |

Ratio improved ~38%; the remaining ~17x gap is dominated by per-call JNI
dispatch / cache lookups, to be addressed by indy bootstrap and cache work.
