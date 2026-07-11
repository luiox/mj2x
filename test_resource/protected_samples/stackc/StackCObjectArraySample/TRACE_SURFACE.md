# StackCObjectArraySample Trace Surface

Sample: `protected_samples.stackc.StackCObjectArraySample`

Backend: `stackc`

Target issue: https://github.com/luiox/morpher/issues/438

Purpose: first real mj2x protected sample for mjdbg observation reports.

Expected surfaces:

- Java method trace for the public static entry methods.
- JNI lookup: `FindClass`, `GetMethodID`, `GetStaticMethodID`, `GetFieldID`, `GetStaticFieldID`.
- JNI invoke: typed `Call*MethodA` dispatch where generated C calls Java methods.
- JNI field access: field get/set anchors for `value` and `sink`.
- JNI array access: primitive array creation and region access.
- JNI string access: string literal materialization through UTF/string JNI APIs.

Report gate:

- Run mj2x protected sample generation.
- Run mjdbg trace against the generated sample.
- Build `report observation --sample StackCObjectArraySample` from daemon trace records.
- The report must include expected surfaces, observed surfaces, missing surfaces, analyzer findings, and hardening suggestions.
