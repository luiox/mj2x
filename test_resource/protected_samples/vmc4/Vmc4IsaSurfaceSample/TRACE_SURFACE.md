# Vmc4IsaSurfaceSample Trace Surface

Sample: `protected_samples.vmc4.Vmc4IsaSurfaceSample`

Backend: `vmc4`

Target issue: https://github.com/luiox/morpher/issues/762

Purpose: first VMC4 protected sample for mjdbg observation reports and the
N-7 ISA-recovery dynamic cross-check. It exercises pure-computation semantics
so the trace surface stays focused on native entry wrappers and VM execution
side effects, with no JNI helper (field/array/call/alloc/intrinsic) surface.

Expected surfaces:

- Java method trace for the public static entry methods (`fib`/`compute`/`mix`/
  `scale`/`cmp`).
- Native/JNI entry wrapper trace for the generated VMC4 native methods
  (RegisterNatives binding of the `mj2x_vm_exec_*` wrappers).
- Negative surface expectation: no JNI lookup/invoke/field/array/string/
  exception/monitor helper anchors are expected (pure-computation sample),
  and no RegC runtime helper surface (`regc_runtime`) applies to VMC4.
- ISA recovery cross-check (N-7): the static ISA-recovery report
  (`mjdbg-isa-recovery-runner`) must label the instance handlers as
  COPY/UNARY/BINARY/COMPARE/CONVERT/SUPER_INC/SUPER_DEC/BRANCH/JUMP/RETURN/
  HALT (per generated instance), and the trace must confirm the native
  wrappers actually ran (MethodEntry captured, no missing surface).

Report gate:

- Generate the VMC4 protected class and DLL from the committed Java source
  (`mj2x-cli --backend vmc4 ... --compile`, MSVC toolchain required).
- Run mjdbg Java/native trace against the generated sample.
- Build `report observation --sample Vmc4IsaSurfaceSample` from daemon
  trace records; the report must include expected/observed/missing surfaces,
  analyzer findings, and hardening suggestions.
- Optionally cross-check with `mjdbg-isa-recovery-runner --input <generated.c>
  --trace-surface <expected>` (N-7 ISA recovery report).
