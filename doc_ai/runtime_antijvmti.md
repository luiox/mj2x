# Runtime Anti-JVMTI Design

## Purpose

`runtime/src/antijvmti/` targets the JNI vtable hook anchor used by c2j-style
agents.  R3 needs a runtime place where generated native code can detect that
the active JNI function table is no longer the one it captured.

## Design

The probe core stays mockable: it snapshots an opaque `env->functions` address
and later compares the current address with that snapshot.  Argument scanners
recognize common agent launch options (`-javaagent`, `-agentpath`,
`-agentlib`) and JDWP/IDE debugging options separately.

The live slice adds signals that cannot be covered by argument scanning alone:

- current-process module enumeration matching known attach/agent/dump families such as
  Arthas, ByteBuddy, JDWP, JVMTI dump agents, and mjt-dump style tools;
- `JavaVM::GetEnv(JVMTI_VERSION_1_2)` plus `GetCapabilities` for bytecode,
  class-hook, retransform, and tag combinations relevant to class dumping.

Windows module collection uses Toolhelp snapshots; Linux parses
`/proc/self/maps`. Both probes report `Unavailable`, `Success`, or `Failed`
instead of treating missing facts as clean.

## Boundaries

JVMTI capabilities belong to a specific `jvmtiEnv`. Standard JVMTI cannot
enumerate environments owned by unrelated agents, so capability query is not
claimed as cross-agent proof. Module enumeration and JNI-table stability remain
independent complementary signals. The module reports facts only; the detection
policy decides whether a signal or probe failure denies execution.

## Validation

`mj2x_runtime_test` verifies stable and replaced JNI table pointers, agent/JDWP
option detection, suspicious module-name matching, module enumeration,
capability aggregation, null-VM availability semantics, and fail-closed policy.
