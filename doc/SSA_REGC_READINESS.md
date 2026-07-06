# SSA RegC Readiness Design

> Status: R1 readiness note
> Scope: `src/mj2x/mir/ssa/`
> Related docs: `IR_DESIGN.md`, `SSA_ALGORITHM.md`, `REGC_NEXT_STEPS.md`

## Purpose

The SSA layer is the handoff point between bytecode-shaped MIR and the future
RegC backend.  Its job is not to optimize aggressively yet; its job is to give
RegC a stackless, value-oriented view of a method where JVM operand-stack motion
has already disappeared.

This matters because the old CBackend emitted code around a visible `stack[]`
and `sp`.  That shape is easy to recognize and hard to optimize.  SSA instead
turns each meaningful JVM value into an SSAValue and keeps block transfer values
explicit on edges.  RegC can then generate local temporaries from values rather
than reconstructing a JVM stack machine.

## Boundary

SSA consumes L1 `Block[]` from `CodeConverter`.  It does not own those blocks
and does not reinterpret raw bytecode.  If a bytecode feature has already been
lowered into MIR, SSA either lowers it into a stackless instruction/terminator
or rejects it with a clear reason.

The current fail-closed boundary is intentionally small: `invokedynamic` remains
unsupported unless a preprocessor expands it first.  Other mainstream MIR
families needed by RegC are represented: constants, locals, arithmetic,
conversions, comparisons, branches, switch, returns, throw, fields, calls,
allocation, arrays, reference checks, monitor operations, and try/catch handler
entry values.

## Construction Model

The builder has a private construction frame:

```text
FrameState
  locals[slot] -> SSAValue*
  stack[]      -> SSAValue*
```

This frame is not IR.  It exists only while translating one block.  Stack
instructions such as `POP`, `POP2`, `DUP`, `DUP2_X*`, and `SWAP` rewrite this
temporary stack with JVM category-1/category-2 rules and do not create SSA
nodes.  The emitted IR therefore contains values and effects, not stack
positions.

Local values that cross block boundaries use block arguments.  The reaching-def
pass decides which local slots need a block argument at a block entry.  Each
predecessor then passes concrete values through the successor edge.  This keeps
phi semantics explicit without introducing a separate phi instruction family.

## Type Shape

Method parameters are bound by JVM local slot.  A static method `(JD)D` therefore
has parameter values at slots 0 and 2, matching JVM category-2 local layout.
Block argument types are inferred from the MIR definitions that reach the block
entry, so long/double locals keep their wide value type when they become phi-like
block arguments.

SSA still treats object and array references as `REF`.  The specific class or
array descriptor is preserved on instructions that need it later, such as
allocation and reference casts.  This keeps the core value type system compact
while leaving RegC enough metadata to emit runtime/JNI operations.

## Effect Shape

RegC cannot freely reorder every JVM-looking instruction.  Field access,
array access, allocation, calls, reference checks, monitors, and throws can
touch memory, resolve classes, call runtime helpers, or throw exceptions.

SSA records these facts in `SSAEffect`.  The markings are conservative by
design: they are there to keep future optimization and scheduling passes from
moving observable JVM operations across each other before the runtime model is
more precise.

## RegC Consumption

RegC should be able to walk each block as:

```text
for instruction in block.instructions:
    emit value-producing or effectful C for instruction
emit block.terminator
```

Calls carry arguments in source order.  Instance calls put the receiver first;
static calls contain only descriptor arguments.  Array and multi-array
allocation keep dimensions in source order.  Field references keep
owner/name/descriptor together.  These choices avoid forcing RegC to recover JVM
stack pop order.

Exception tables are carried as `SSAExceptionRegion` entries.  A handler block
owns a synthetic REF block argument for the caught exception object, and RegC
uses the region metadata to emit pending-exception dispatch without recovering
JVM stack shape.

## Remaining Risks

The current SSA builder is ready for a RegC MVP, but it is not a full verifier.
It assumes L1 blocks are already sane and uses fail-closed checks for local stack
underflow.  The accepted exception shape is the standard try/catch handler
single-object entry; arbitrary cross-block operand-stack merges and unexpanded
`invokedynamic` remain outside the accepted surface.  Those should become
explicit design slices before RegC claims full Java method coverage.
