# RegC Samples for mjdbg

These Java sources are stable inputs for generating RegC helper-style C samples.

Generated artifacts are not checked in.  The generator test writes them under:

```text
test/mj2x/protected_samples/regc/<SampleName>/
```

Run:

```powershell
xmake build mj2x_unittest
xmake run mj2x_unittest -- --gtest_filter=RegCTraceSamples.GenerateMjdbgFixtures
```

The generated C includes RegC JNI/native glue plus the RegIR-to-RegC helper
surface used to compare against CBackend's stack-machine C output.  mjdbg should
expect register locals such as `r0`/`r1`, labels such as `block_b0`, helper calls
such as `_mj2x_regc_alloc_object`, runtime ABI calls such as
`mj2x_regc_div_i32`, and exception-dispatch helpers such as
`_mj2x_regc_exception_matches` instead of `stack[]` / `sp` patterns.
