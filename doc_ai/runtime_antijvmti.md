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

The second slice adds deterministic inputs for signals that cannot be covered
by argument scanning alone:

- loaded module names matching known attach/agent/dump families such as
  Arthas, ByteBuddy, JDWP, JVMTI dump agents, and mjt-dump style tools;
- a small JVMTI capability snapshot that flags bytecode/class-load-hook and
  retransform/tag combinations relevant to class dumping.

Runtime core does not enumerate modules or query a live JVM yet.  Platform/JVM
integration should collect those raw facts and pass them into this module.

## Boundaries

This module does not yet enumerate loaded modules or query JVMTI capabilities by
itself.  Those probes require platform/JVM integration and should remain layered
above the mockable core so tests can inject deterministic tampering cases.

## Validation

`mj2x_runtime_test` verifies stable and replaced JNI table pointers, agent/JDWP
option detection, suspicious module-name matching, JVMTI capability aggregation,
and full agent-signal aggregation without requiring a live JVM.
