# mj2x benchmark

`java_obfuscator_test_benchmark.ps1` runs the external-style fixture at
`test/java_obfuscator_test/TEST.jar` as a full native RegC benchmark.

It measures:

- baseline JVM run of `TEST.jar`;
- full `mj2x-cli --backend regc --generate-xmake` conversion;
- xmake build time for the generated native project;
- JVM run of the generated jar after embedding the xmake-built native library.

Example:

```powershell
powershell -ExecutionPolicy Bypass -File mj2x/benchmark/java_obfuscator_test_benchmark.ps1 -Runs 5
```

Results are written under `test/java_obfuscator_test/benchmark-current/`:

- `perf-results.csv`: per-run `Calc: Nms` and process time.
- `summary.txt`: baseline/regc averages and ratio.
- `regc-full-xmake/`: generated xmake project and native build artifacts.
