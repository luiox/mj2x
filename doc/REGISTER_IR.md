# Register IR Design

## Purpose

Register IR is mj2x's L3 representation between block-argument SSA and the
RegC backend.  It turns SSA values into flat virtual registers and removes SSA
block arguments from the final control-flow graph.

The main defensive property is structural: RegIR has no JVM operand-stack slot
operand.  RegC emits C locals from virtual registers instead of reconstructing
the old CBackend `stack[]` / `sp` pattern.

## Inputs and Outputs

RegIR consumes `SSAFunction` from `src/mj2x/mir/ssa/`.  It does not read raw
bytecode and does not repeat SSA construction.  Its output is `RegFunction`,
which owns:

- method symbol metadata copied from SSA
- parameter registers
- basic blocks
- register-form instructions
- terminators whose successors carry explicit edge copies

RegIR is still virtual-register IR.  It does not allocate physical machine
registers, assign stack frame slots, or emit C.  Those belong to RegC.

## Value Model

Every non-literal SSA value maps to a virtual register.  The first RegIR stage
keeps the SSA value id as the register id, which makes debugging and dump
comparison straightforward.  Integer, floating, null, and opaque values can
remain immediates when they do not need a register definition.

There is deliberately no operand kind for JVM stack slot, local slot, `sp`, or
`cstack`.  The validation helper walks all instructions, terminators, and edge
copies to enforce that invariant.

## Block-Argument Elimination

SSA represents phi semantics through block arguments:

```text
predecessor -> successor(arg0, arg1)
successor(arg0, arg1):
```

RegIR removes the successor block-argument list.  Each successor edge carries
the copies needed before control reaches the target:

```text
predecessor -> successor { r10 <- r3; r11 <- 0 }
successor:
```

Copies are stored on the edge rather than blindly appended to the predecessor
block instruction list.  This matters for branch and switch terminators because
each outgoing edge may pass different values.

## Instruction Shape

RegIR mirrors the SSA instruction families: unary, binary, compare, convert,
field access, array access, calls, allocation, intrinsics, and barriers.  It
uses tag fields and plain structs rather than RTTI.  The active fields are
selected by `RegInstructionKind`, matching the style already used by SSA.

RegIR is intentionally not an optimizer.  It preserves operation order and
effect metadata so later passes can decide what is movable.

## Testing Expectations

RegIR tests should verify both behavior of the lowering shape and structural
constraints:

- arithmetic and returns become register operations
- loop block arguments become edge copies
- branch and switch edge transfers stay edge-specific
- dumps contain no `cstack`, `sp`, or SSA block-argument slot text
- `validateRegIRNoStackSlots` succeeds

RegC tests will build on this layer by asserting generated C avoids stack-machine
anchors as well.
