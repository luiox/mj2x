#pragma once

#include <jbytecode/jbytecode.hpp>

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <memory>

namespace mj2x {

// -- MIR kind enum --
enum class MIRKind : uint8_t {
    NOP,
    LABEL,
    CONSTANT,
    LOCAL_LOAD,
    LOCAL_STORE,
    STACK_DUP,
    STACK_DUP_X1,
    STACK_DUP_X2,
    STACK_DUP2,
    STACK_SWAP,
    BINARY_OP,
    UNARY_OP,
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

enum class BinaryOp : uint8_t {
    ADD, SUB, MUL, DIV, REM,
    SHL, SHR, USHR,
    AND, OR, XOR,
};

enum class UnaryOp : uint8_t {
    NEG,
};

enum class CmpOp : uint8_t {
    EQ, NE, LT, GE, GT, LE,
};

enum class PrimitiveKind : uint8_t {
    BOOLEAN, BYTE, CHAR, SHORT, INT, LONG, FLOAT, DOUBLE, VOID, OBJECT,
};

// -- MIRNode base --
struct MIRNode {
    MIRKind kind;
    explicit MIRNode(MIRKind k) : kind(k) {}
    virtual ~MIRNode() = default;
};

// -- Concrete node types --

struct MIRNop : MIRNode {
    MIRNop() : MIRNode(MIRKind::NOP) {}
};

struct MIRLabel : MIRNode {
    int id;
    explicit MIRLabel(int id_) : MIRNode(MIRKind::LABEL), id(id_) {}
};

struct MIRConstant : MIRNode {
    std::variant<int64_t, double, std::nullptr_t> value;
    PrimitiveKind type = PrimitiveKind::INT;
    MIRConstant() : MIRNode(MIRKind::CONSTANT) {}
};

struct MIRLocalLoad : MIRNode {
    int varIndex = 0;
    PrimitiveKind type = PrimitiveKind::INT;
    MIRLocalLoad() : MIRNode(MIRKind::LOCAL_LOAD) {}
};

struct MIRLocalStore : MIRNode {
    int varIndex = 0;
    PrimitiveKind type = PrimitiveKind::INT;
    MIRLocalStore() : MIRNode(MIRKind::LOCAL_STORE) {}
};

struct MIRIinc : MIRNode {
    int varIndex = 0;
    int increment = 0;
    MIRIinc() : MIRNode(MIRKind::IINC) {}
};

struct MIRStackOp : MIRNode {
    using MIRNode::MIRNode;
};

struct MIRBinaryOp : MIRNode {
    BinaryOp op = BinaryOp::ADD;
    PrimitiveKind type = PrimitiveKind::INT;
    MIRBinaryOp() : MIRNode(MIRKind::BINARY_OP) {}
};

struct MIRUnaryOp : MIRNode {
    UnaryOp op = UnaryOp::NEG;
    PrimitiveKind type = PrimitiveKind::INT;
    MIRUnaryOp() : MIRNode(MIRKind::UNARY_OP) {}
};

struct MIRCast : MIRNode {
    PrimitiveKind from = PrimitiveKind::INT;
    PrimitiveKind to = PrimitiveKind::INT;
    MIRCast() : MIRNode(MIRKind::CAST) {}
};

struct MIRBranch : MIRNode {
    CmpOp op = CmpOp::EQ;
    PrimitiveKind type = PrimitiveKind::INT;
    int targetLabel = 0;
    MIRBranch() : MIRNode(MIRKind::BRANCH) {}
};

struct MIRBranchZero : MIRNode {
    CmpOp op = CmpOp::EQ;
    int targetLabel = 0;
    MIRBranchZero() : MIRNode(MIRKind::BRANCH_ZERO) {}
};

struct MIRUncondBranch : MIRNode {
    int targetLabel = 0;
    MIRUncondBranch() : MIRNode(MIRKind::UNCOND_BRANCH) {}
};

struct MIRSwitch : MIRNode {
    struct Case {
        int key;
        int targetLabel;
    };
    int defaultLabel = 0;
    std::vector<Case> cases;
    MIRSwitch() : MIRNode(MIRKind::SWITCH) {}
};

struct MIRReturn : MIRNode {
    MIRReturn() : MIRNode(MIRKind::RETURN) {}
};

struct MIRReturnValue : MIRNode {
    PrimitiveKind type = PrimitiveKind::INT;
    MIRReturnValue() : MIRNode(MIRKind::RETURN_VALUE) {}
};

struct MIRNewObject : MIRNode {
    std::string typeDesc;
    MIRNewObject() : MIRNode(MIRKind::NEW_OBJECT) {}
};

struct MIRNewArray : MIRNode {
    std::string typeDesc;
    int dims = 1;
    MIRNewArray() : MIRNode(MIRKind::NEW_ARRAY) {}
};

struct MIRArrayLength : MIRNode {
    MIRArrayLength() : MIRNode(MIRKind::ARRAY_LENGTH) {}
};

struct MIRArrayLoad : MIRNode {
    PrimitiveKind componentType = PrimitiveKind::INT;
    MIRArrayLoad() : MIRNode(MIRKind::ARRAY_LOAD) {}
};

struct MIRArrayStore : MIRNode {
    PrimitiveKind componentType = PrimitiveKind::INT;
    MIRArrayStore() : MIRNode(MIRKind::ARRAY_STORE) {}
};

struct MIRFieldGet : MIRNode {
    std::string owner, name, desc;
    MIRFieldGet() : MIRNode(MIRKind::FIELD_GET) {}
};

struct MIRFieldPut : MIRNode {
    std::string owner, name, desc;
    MIRFieldPut() : MIRNode(MIRKind::FIELD_PUT) {}
};

struct MIRStaticGet : MIRNode {
    std::string owner, name, desc;
    MIRStaticGet() : MIRNode(MIRKind::STATIC_GET) {}
};

struct MIRStaticPut : MIRNode {
    std::string owner, name, desc;
    MIRStaticPut() : MIRNode(MIRKind::STATIC_PUT) {}
};

struct MIRInvoke : MIRNode {
    std::string owner, name, desc;
    bool isInterface = false;
    int invokeOpcode = 0; // INVOKEVIRTUAL/SPECIAL/STATIC/INTERFACE
    MIRInvoke() : MIRNode(MIRKind::INVOKE) {}
};

struct MIRCheckCast : MIRNode {
    std::string typeDesc;
    MIRCheckCast() : MIRNode(MIRKind::CHECK_CAST) {}
};

struct MIRInstanceOf : MIRNode {
    std::string typeDesc;
    MIRInstanceOf() : MIRNode(MIRKind::INSTANCE_OF) {}
};

struct MIRMonitor : MIRNode {
    bool enter = true;
    MIRMonitor() : MIRNode(MIRKind::MONITOR_ENTER) {}
};

struct MIRThrow : MIRNode {
    MIRThrow() : MIRNode(MIRKind::THROW) {}
};

struct MIRFrame : MIRNode {
    MIRFrame() : MIRNode(MIRKind::FRAME) {}
};

} // namespace mj2x

