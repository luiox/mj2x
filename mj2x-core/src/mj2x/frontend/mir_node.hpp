#pragma once

#include "libca/core/any.hpp"
#include "mj2x/core/jvm_type.hpp"
#include <libca/str/utf8_string.hpp>
#include "libca/core/tag_cast.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <optional>

namespace mj2x {

/// @brief L1 MIR node kind.
///
/// @details
/// L1 MIR keeps bytecode control flow and JVM operation families explicit while
/// hiding tree-node details from backends.  Stack-shape operations are preserved
/// as MIR nodes instead of being collapsed to NOP because the SSA builder needs
/// to apply JVM category-1/category-2 rules when it erases the operand stack.
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
// 接入 ca::core::tag_cast::cast/isa/dyn_cast 体系：每个子类构造时绑定固定 MIRKind，
// getType() 暴露该 tag 供 ca::core::tag_cast::isa 做运行期校验。downcast 应优先用
// ca::core::tag_cast::cast<T>（switch case 内、kind 已确定）或 ca::core::tag_cast::dyn_cast<T>
// （运行期类型不确定时），避免裸 static_cast 越过类型契约。
struct MIRNode
{
    MIRKind kind;
    explicit MIRNode(MIRKind k)
        : kind(k)
    {}
    virtual ~MIRNode() = default;

    /// ca::core::tag_cast::isa/dyn_cast 读取的 type tag。
    MIRKind getType() const { return kind; }
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
    // 批 6d 标签 id 化：label 标识 = CodeConverter 的顺序 id（同 pc 恒同 id）。
    int labelId = 0;
    explicit MIRLabel(int id)
        : MIRNode(MIRKind::LABEL)
        , labelId(id)
    {}
};

struct MIRConstant : MIRNode
{
    // string 槽持 arena-backed Utf8StringRef（LDC String 常量），由 CodeConverter 经
    // index.arena().intern(...) 入池（str-ownership-spec §6.1）。
    // Type 槽批 3a 起为 mj2x::Type 门面（旧栈 Type 只活在树侧 Any 载荷，
    // 经 CodeConverter 转换后入 MIR；libjbc 迁移账 plan_libjbc_migration.md 批 3）。
    std::variant<int64_t, double, std::nullptr_t, ca::str::Utf8StringRef, Type> value;
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
    CmpOp         op          = CmpOp::EQ;
    PrimitiveKind type        = PrimitiveKind::INT;
    int           targetLabel = 0;
    MIRBranch()
        : MIRNode(MIRKind::BRANCH)
    {}
};

struct MIRBranchZero : MIRNode
{
    CmpOp         op          = CmpOp::EQ;
    PrimitiveKind type        = PrimitiveKind::INT;
    int           targetLabel = 0;
    MIRBranchZero()
        : MIRNode(MIRKind::BRANCH_ZERO)
    {}
};

struct MIRUncondBranch : MIRNode
{
    int targetLabel = 0;
    MIRUncondBranch()
        : MIRNode(MIRKind::UNCOND_BRANCH)
    {}
};

struct MIRSwitch : MIRNode
{
    struct Case
    {
        int key;
        int targetLabel;
    };
    int               defaultLabel = 0;
    std::vector<Case> cases;
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
    ca::str::Utf8StringRef typeDesc;
    MIRNewObject()
        : MIRNode(MIRKind::NEW_OBJECT)
    {}
};

struct MIRNewArray : MIRNode
{
    ca::str::Utf8StringRef typeDesc;
    PrimitiveKind          componentType = PrimitiveKind::OBJECT;
    int                    dims          = 1;
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
    ca::str::Utf8StringRef owner, name, desc;
    MIRFieldGet()
        : MIRNode(MIRKind::FIELD_GET)
    {}
};

struct MIRFieldPut : MIRNode
{
    ca::str::Utf8StringRef owner, name, desc;
    MIRFieldPut()
        : MIRNode(MIRKind::FIELD_PUT)
    {}
};

struct MIRStaticGet : MIRNode
{
    ca::str::Utf8StringRef owner, name, desc;
    MIRStaticGet()
        : MIRNode(MIRKind::STATIC_GET)
    {}
};

struct MIRStaticPut : MIRNode
{
    ca::str::Utf8StringRef owner, name, desc;
    MIRStaticPut()
        : MIRNode(MIRKind::STATIC_PUT)
    {}
};

struct MIRInvoke : MIRNode
{
    ca::str::Utf8StringRef owner, name, desc;
    bool                   isInterface  = false;
    int                    invokeOpcode = 0;   // INVOKEVIRTUAL/SPECIAL/STATIC/INTERFACE
    MIRInvoke()
        : MIRNode(MIRKind::INVOKE)
    {}
};

struct MIRInvokeDynamic : MIRNode
{
    ca::str::Utf8StringRef                 name, desc;
    std::optional<mj2x::Handle> bsm;
    std::vector<ca::core::Any>             bsmArgs;
    MIRInvokeDynamic()
        : MIRNode(MIRKind::INVOKE_DYNAMIC)
    {}
};

// multianewarray：保留描述符和维度数，SSA/RegC 通过 multi-array helper 执行。
struct MIRMultiNewArray : MIRNode
{
    ca::str::Utf8StringRef typeDesc;
    int                    dims = 0;
    MIRMultiNewArray()
        : MIRNode(MIRKind::MULTI_NEW_ARRAY)
    {}
};

struct MIRCheckCast : MIRNode
{
    ca::str::Utf8StringRef typeDesc;
    MIRCheckCast()
        : MIRNode(MIRKind::CHECK_CAST)
    {}
};

struct MIRInstanceOf : MIRNode
{
    ca::str::Utf8StringRef typeDesc;
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

/// @brief StackMapTable frame metadata kept for SSA block-entry stack args.
///
/// @details
/// Most frame metadata remains verifier-only and is ignored by backends. The
/// operand-stack part is different: javac can split bytecode so a join block
/// starts with a value already on the JVM operand stack, for example ternary
/// expressions compiled as two branches that each push a value before `ireturn`.
/// SSA erases the construction-time stack, so CodeConverter preserves the
/// frame's stack item types here and SSABuilder turns them into synthetic block
/// arguments. Handler-entry exception objects are modeled separately from the
/// exception table and do not rely on these stack items.
struct MIRFrame : MIRNode
{
    int                        frameType = 0;
    std::vector<PrimitiveKind> stackTypes;
    MIRFrame()
        : MIRNode(MIRKind::FRAME)
    {}
};

}   // namespace mj2x

// ca::core::tag_cast::TypeOf 特化：把每个 MIR 子类映射到它构造时绑定的 MIRKind tag。
// 这样 ca::core::tag_cast::isa<MIRFoo>(node) / ca::core::tag_cast::dyn_cast<MIRFoo>(node) 可在运行期
// 校验 node->getType() 是否匹配，ca::core::tag_cast::cast<MIRFoo> 才能在已确定上下文安全使用。
namespace ca::core::tag_cast {
#define MJ2X_MIR_TYPEOF(MIR_TYPE, KIND)                          \
    template<>                                                    \
    struct TypeOf<::mj2x::MIR_TYPE>                               \
    {                                                             \
        static constexpr ::mj2x::MIRKind value = ::mj2x::MIRKind::KIND; \
    }
MJ2X_MIR_TYPEOF(MIRNop, NOP);
MJ2X_MIR_TYPEOF(MIRLabel, LABEL);
MJ2X_MIR_TYPEOF(MIRConstant, CONSTANT);
MJ2X_MIR_TYPEOF(MIRLocalLoad, LOCAL_LOAD);
MJ2X_MIR_TYPEOF(MIRLocalStore, LOCAL_STORE);
MJ2X_MIR_TYPEOF(MIRBinaryOp, BINARY_OP);
MJ2X_MIR_TYPEOF(MIRUnaryOp, UNARY_OP);
MJ2X_MIR_TYPEOF(MIRCmp, CMP);
MJ2X_MIR_TYPEOF(MIRCast, CAST);
MJ2X_MIR_TYPEOF(MIRIinc, IINC);
MJ2X_MIR_TYPEOF(MIRBranch, BRANCH);
MJ2X_MIR_TYPEOF(MIRBranchZero, BRANCH_ZERO);
MJ2X_MIR_TYPEOF(MIRUncondBranch, UNCOND_BRANCH);
MJ2X_MIR_TYPEOF(MIRSwitch, SWITCH);
MJ2X_MIR_TYPEOF(MIRReturn, RETURN);
MJ2X_MIR_TYPEOF(MIRReturnValue, RETURN_VALUE);
MJ2X_MIR_TYPEOF(MIRNewObject, NEW_OBJECT);
MJ2X_MIR_TYPEOF(MIRNewArray, NEW_ARRAY);
MJ2X_MIR_TYPEOF(MIRMultiNewArray, MULTI_NEW_ARRAY);
MJ2X_MIR_TYPEOF(MIRArrayLength, ARRAY_LENGTH);
MJ2X_MIR_TYPEOF(MIRArrayLoad, ARRAY_LOAD);
MJ2X_MIR_TYPEOF(MIRArrayStore, ARRAY_STORE);
MJ2X_MIR_TYPEOF(MIRFieldGet, FIELD_GET);
MJ2X_MIR_TYPEOF(MIRFieldPut, FIELD_PUT);
MJ2X_MIR_TYPEOF(MIRStaticGet, STATIC_GET);
MJ2X_MIR_TYPEOF(MIRStaticPut, STATIC_PUT);
MJ2X_MIR_TYPEOF(MIRInvoke, INVOKE);
MJ2X_MIR_TYPEOF(MIRInvokeDynamic, INVOKE_DYNAMIC);
MJ2X_MIR_TYPEOF(MIRCheckCast, CHECK_CAST);
MJ2X_MIR_TYPEOF(MIRInstanceOf, INSTANCE_OF);
MJ2X_MIR_TYPEOF(MIRMonitor, MONITOR_ENTER);
MJ2X_MIR_TYPEOF(MIRThrow, THROW);
MJ2X_MIR_TYPEOF(MIRFrame, FRAME);
#undef MJ2X_MIR_TYPEOF
}   // namespace ca::core::tag_cast
