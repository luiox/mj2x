#pragma once

#include <jbytecode/jbytecode.hpp>

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <optional>

namespace mj2x {

/**
 * @brief L1 MIR node kind.
 *
 * @details
 * L1 MIR keeps bytecode control flow and JVM operation families explicit while
 * hiding tree-node details from backends.  Stack-shape operations are preserved
 * as MIR nodes instead of being collapsed to NOP because the SSA builder needs
 * to apply JVM category-1/category-2 rules when it erases the operand stack.
 */
enum class MIRKind : uint8_t
{
    NOP,
    LABEL,
    CONSTANT,
    LOCAL_LOAD,
    LOCAL_STORE,
    STACK_DUP,
    STACK_DUP_X1,
    STACK_DUP_X2,
    STACK_DUP2,
    STACK_DUP2_X1,
    STACK_DUP2_X2,
    STACK_SWAP,
    STACK_POP,
    STACK_POP2,
    BINARY_OP,
    UNARY_OP,
    CMP,
    CAST,
    IINC,
    BRANCH,
    BRANCH_ZERO,
    UNCOND_BRANCH,
    SWITCH,
    RETURN,
    RETURN_VALUE,
    NEW_OBJECT,
    NEW_ARRAY,
    MULTI_NEW_ARRAY,
    ARRAY_LENGTH,
    ARRAY_LOAD,
    ARRAY_STORE,
    FIELD_GET,
    FIELD_PUT,
    STATIC_GET,
    STATIC_PUT,
    INVOKE,
    INVOKE_DYNAMIC,
    CHECK_CAST,
    INSTANCE_OF,
    MONITOR_ENTER,
    MONITOR_EXIT,
    THROW,
    FRAME,
    LINE,
};

enum class BinaryOp : uint8_t
{
    ADD,
    SUB,
    MUL,
    DIV,
    REM,
    SHL,
    SHR,
    USHR,
    AND,
    OR,
    XOR,
};

enum class UnaryOp : uint8_t
{
    NEG,
};

enum class CmpOp : uint8_t
{
    EQ,
    NE,
    LT,
    GE,
    GT,
    LE,
};

enum class PrimitiveKind : uint8_t
{
    BOOLEAN,
    BYTE,
    CHAR,
    SHORT,
    INT,
    LONG,
    FLOAT,
    DOUBLE,
    VOID,
    OBJECT,
};

// -- MIRNode base --
struct MIRNode
{
    MIRKind kind;
    explicit MIRNode(MIRKind k)
        : kind(k)
    {}
    virtual ~MIRNode() = default;
};

// -- Concrete node types --

struct MIRNop : MIRNode
{
    MIRNop()
        : MIRNode(MIRKind::NOP)
    {}
};

struct MIRLabel : MIRNode
{
    // label 标识用 Label*（读取期稳定）。bytecodeOffset 在树形重建后被重置为 -1，不可靠。
    jbytecode::core::Label* labelNode = nullptr;
    explicit MIRLabel(jbytecode::core::Label* ln)
        : MIRNode(MIRKind::LABEL)
        , labelNode(ln)
    {}
};

struct MIRConstant : MIRNode
{
    std::variant<int64_t, double, std::nullptr_t, std::string, jbytecode::core::Type> value;
    PrimitiveKind type = PrimitiveKind::INT;
    MIRConstant()
        : MIRNode(MIRKind::CONSTANT)
    {}
};

struct MIRLocalLoad : MIRNode
{
    int           varIndex = 0;
    PrimitiveKind type     = PrimitiveKind::INT;
    MIRLocalLoad()
        : MIRNode(MIRKind::LOCAL_LOAD)
    {}
};

struct MIRLocalStore : MIRNode
{
    int           varIndex = 0;
    PrimitiveKind type     = PrimitiveKind::INT;
    MIRLocalStore()
        : MIRNode(MIRKind::LOCAL_STORE)
    {}
};

struct MIRIinc : MIRNode
{
    int varIndex  = 0;
    int increment = 0;
    MIRIinc()
        : MIRNode(MIRKind::IINC)
    {}
};

struct MIRStackOp : MIRNode
{
    using MIRNode::MIRNode;
    // Top-first operand stack type snapshot before this stack-shape opcode.
    // StackC uses it to distinguish category-2 values from category-1 pairs.
    std::vector<PrimitiveKind> stackBeforeTop;
};

struct MIRBinaryOp : MIRNode
{
    BinaryOp      op   = BinaryOp::ADD;
    PrimitiveKind type = PrimitiveKind::INT;
    MIRBinaryOp()
        : MIRNode(MIRKind::BINARY_OP)
    {}
};

struct MIRUnaryOp : MIRNode
{
    UnaryOp       op   = UnaryOp::NEG;
    PrimitiveKind type = PrimitiveKind::INT;
    MIRUnaryOp()
        : MIRNode(MIRKind::UNARY_OP)
    {}
};

struct MIRCmp : MIRNode
{
    PrimitiveKind type       = PrimitiveKind::LONG;
    bool          nanGreater = false;
    MIRCmp()
        : MIRNode(MIRKind::CMP)
    {}
};

struct MIRCast : MIRNode
{
    PrimitiveKind from = PrimitiveKind::INT;
    PrimitiveKind to   = PrimitiveKind::INT;
    MIRCast()
        : MIRNode(MIRKind::CAST)
    {}
};

struct MIRBranch : MIRNode
{
    CmpOp                   op          = CmpOp::EQ;
    PrimitiveKind           type        = PrimitiveKind::INT;
    jbytecode::core::Label* targetLabel = nullptr;
    MIRBranch()
        : MIRNode(MIRKind::BRANCH)
    {}
};

struct MIRBranchZero : MIRNode
{
    CmpOp                   op          = CmpOp::EQ;
    PrimitiveKind           type        = PrimitiveKind::INT;
    jbytecode::core::Label* targetLabel = nullptr;
    MIRBranchZero()
        : MIRNode(MIRKind::BRANCH_ZERO)
    {}
};

struct MIRUncondBranch : MIRNode
{
    jbytecode::core::Label* targetLabel = nullptr;
    MIRUncondBranch()
        : MIRNode(MIRKind::UNCOND_BRANCH)
    {}
};

struct MIRSwitch : MIRNode
{
    struct Case
    {
        int                     key;
        jbytecode::core::Label* targetLabel;
    };
    jbytecode::core::Label* defaultLabel = nullptr;
    std::vector<Case>       cases;
    MIRSwitch()
        : MIRNode(MIRKind::SWITCH)
    {}
};

struct MIRReturn : MIRNode
{
    MIRReturn()
        : MIRNode(MIRKind::RETURN)
    {}
};

struct MIRReturnValue : MIRNode
{
    PrimitiveKind type = PrimitiveKind::INT;
    MIRReturnValue()
        : MIRNode(MIRKind::RETURN_VALUE)
    {}
};

struct MIRNewObject : MIRNode
{
    std::string typeDesc;
    MIRNewObject()
        : MIRNode(MIRKind::NEW_OBJECT)
    {}
};

struct MIRNewArray : MIRNode
{
    std::string   typeDesc;
    PrimitiveKind componentType = PrimitiveKind::OBJECT;
    int           dims          = 1;
    MIRNewArray()
        : MIRNode(MIRKind::NEW_ARRAY)
    {}
};

struct MIRArrayLength : MIRNode
{
    MIRArrayLength()
        : MIRNode(MIRKind::ARRAY_LENGTH)
    {}
};

struct MIRArrayLoad : MIRNode
{
    PrimitiveKind componentType = PrimitiveKind::INT;
    MIRArrayLoad()
        : MIRNode(MIRKind::ARRAY_LOAD)
    {}
};

struct MIRArrayStore : MIRNode
{
    PrimitiveKind componentType = PrimitiveKind::INT;
    MIRArrayStore()
        : MIRNode(MIRKind::ARRAY_STORE)
    {}
};

struct MIRFieldGet : MIRNode
{
    std::string owner, name, desc;
    MIRFieldGet()
        : MIRNode(MIRKind::FIELD_GET)
    {}
};

struct MIRFieldPut : MIRNode
{
    std::string owner, name, desc;
    MIRFieldPut()
        : MIRNode(MIRKind::FIELD_PUT)
    {}
};

struct MIRStaticGet : MIRNode
{
    std::string owner, name, desc;
    MIRStaticGet()
        : MIRNode(MIRKind::STATIC_GET)
    {}
};

struct MIRStaticPut : MIRNode
{
    std::string owner, name, desc;
    MIRStaticPut()
        : MIRNode(MIRKind::STATIC_PUT)
    {}
};

struct MIRInvoke : MIRNode
{
    std::string owner, name, desc;
    bool        isInterface  = false;
    int         invokeOpcode = 0;   // INVOKEVIRTUAL/SPECIAL/STATIC/INTERFACE
    MIRInvoke()
        : MIRNode(MIRKind::INVOKE)
    {}
};

struct MIRInvokeDynamic : MIRNode
{
    std::string                            name, desc;
    std::optional<jbytecode::core::Handle> bsm;
    std::vector<ca::core::Any>             bsmArgs;
    MIRInvokeDynamic()
        : MIRNode(MIRKind::INVOKE_DYNAMIC)
    {}
};

// multianewarray：保留描述符和维度数，SSA/RegC 通过 multi-array helper 执行。
struct MIRMultiNewArray : MIRNode
{
    std::string typeDesc;
    int         dims = 0;
    MIRMultiNewArray()
        : MIRNode(MIRKind::MULTI_NEW_ARRAY)
    {}
};

struct MIRCheckCast : MIRNode
{
    std::string typeDesc;
    MIRCheckCast()
        : MIRNode(MIRKind::CHECK_CAST)
    {}
};

struct MIRInstanceOf : MIRNode
{
    std::string typeDesc;
    MIRInstanceOf()
        : MIRNode(MIRKind::INSTANCE_OF)
    {}
};

struct MIRMonitor : MIRNode
{
    bool enter = true;
    MIRMonitor()
        : MIRNode(MIRKind::MONITOR_ENTER)
    {}
};

struct MIRThrow : MIRNode
{
    MIRThrow()
        : MIRNode(MIRKind::THROW)
    {}
};

/**
 * @brief StackMapTable frame metadata kept for SSA block-entry stack args.
 *
 * @details
 * Most frame metadata remains verifier-only and is ignored by backends. The
 * operand-stack part is different: javac can split bytecode so a join block
 * starts with a value already on the JVM operand stack, for example ternary
 * expressions compiled as two branches that each push a value before `ireturn`.
 * SSA erases the construction-time stack, so CodeConverter preserves the
 * frame's stack item types here and SSABuilder turns them into synthetic block
 * arguments. Handler-entry exception objects are modeled separately from the
 * exception table and do not rely on these stack items.
 */
struct MIRFrame : MIRNode
{
    int                        frameType = 0;
    std::vector<PrimitiveKind> stackTypes;
    MIRFrame()
        : MIRNode(MIRKind::FRAME)
    {}
};

}   // namespace mj2x
