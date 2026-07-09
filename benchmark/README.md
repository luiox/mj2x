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
