# Runtime Anti-JVMTI Design

## Purpose

`runtime/src/antijvmti/` targets the JNI vtable hook anchor used by c2j-style
agents.  R3 needs a runtime place where generated native code can detect that
the active JNI function table is no longer the one it captured.

## Design

The first slice keeps the probe mockable: it snapshots an opaque
`env->functions` address and later compares the current address with that
snapshot.  A separate argument scanner recognizes common agent launch options
such as `-javaagent`, `-agentpath`, `-agentlib`, and JDWP.

## Boundaries

This module does not yet enumerate loaded modules or query JVMTI capabilities.
Those probes require platform/JVM integration and should be layered above the
mockable core so tests can still inject deterministic tampering cases.

## Validation

`mj2x_runtime_test` verifies stable and replaced JNI table pointers and checks
agent-option detection without requiring a live JVM.
