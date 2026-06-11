#include <gmock/gmock.h>

#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/frontend/method_stack.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/frontend/opcode_utils.hpp"
#include "jbytecode/jbytecode.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

namespace {

ca::str::Utf8StringRef toRef(const char* s) {
    auto len = std::strlen(s);
    return ca::str::Utf8StringRef(reinterpret_cast<const ca::u8*>(s), len, len);
}

}

// ============================================================
// TypeUtils tests
// ============================================================

TEST(TypeUtilsTest, JvmSortToPrimitive) {
    EXPECT_EQ(jvmSortToPrimitive(Type::INT), PrimitiveKind::INT);
    EXPECT_EQ(jvmSortToPrimitive(Type::LONG), PrimitiveKind::LONG);
    EXPECT_EQ(jvmSortToPrimitive(Type::FLOAT), PrimitiveKind::FLOAT);
    EXPECT_EQ(jvmSortToPrimitive(Type::DOUBLE), PrimitiveKind::DOUBLE);
    EXPECT_EQ(jvmSortToPrimitive(Type::BOOLEAN), PrimitiveKind::BOOLEAN);
    EXPECT_EQ(jvmSortToPrimitive(Type::VOID), PrimitiveKind::VOID);
    EXPECT_EQ(jvmSortToPrimitive(99), PrimitiveKind::OBJECT); // unknown → OBJECT
}

TEST(TypeUtilsTest, JvmDescToPrimitive) {
    EXPECT_EQ(jvmDescToPrimitive("I"), PrimitiveKind::INT);
    EXPECT_EQ(jvmDescToPrimitive("J"), PrimitiveKind::LONG);
    EXPECT_EQ(jvmDescToPrimitive("V"), PrimitiveKind::VOID);
    EXPECT_EQ(jvmDescToPrimitive("Ljava/lang/Object;"), PrimitiveKind::OBJECT);
    EXPECT_EQ(jvmDescToPrimitive("[I"), PrimitiveKind::OBJECT);
}

TEST(TypeUtilsTest, PrimitiveStackSize) {
    EXPECT_EQ(primitiveStackSize(PrimitiveKind::INT), 1);
    EXPECT_EQ(primitiveStackSize(PrimitiveKind::LONG), 2);
    EXPECT_EQ(primitiveStackSize(PrimitiveKind::DOUBLE), 2);
    EXPECT_EQ(primitiveStackSize(PrimitiveKind::OBJECT), 1);
}

// ============================================================
// OpcodeUtils tests
// ============================================================

TEST(OpcodeUtilsTest, BinaryOpMapping) {
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::IADD), BinaryOp::ADD);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::LADD), BinaryOp::ADD);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::ISUB), BinaryOp::SUB);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::IMUL), BinaryOp::MUL);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::IDIV), BinaryOp::DIV);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::IREM), BinaryOp::REM);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::ISHL), BinaryOp::SHL);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::ISHR), BinaryOp::SHR);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::IUSHR), BinaryOp::USHR);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::IAND), BinaryOp::AND);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::IOR), BinaryOp::OR);
    EXPECT_EQ(opcodeToBinaryOp(Opcodes::IXOR), BinaryOp::XOR);
}

TEST(OpcodeUtilsTest, BranchOpcodeMapping) {
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPEQ), CmpOp::EQ);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPNE), CmpOp::NE);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPLT), CmpOp::LT);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPGE), CmpOp::GE);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPGT), CmpOp::GT);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPLE), CmpOp::LE);
}

TEST(OpcodeUtilsTest, BranchZeroOpcodeMapping) {
    EXPECT_EQ(branchZeroOpcodeToCmpOp(Opcodes::IFEQ), CmpOp::EQ);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(Opcodes::IFNE), CmpOp::NE);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(Opcodes::IFLT), CmpOp::LT);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(Opcodes::IFGE), CmpOp::GE);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(Opcodes::IFGT), CmpOp::GT);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(Opcodes::IFLE), CmpOp::LE);
}

// ============================================================
// MethodStack tests
// ============================================================

TEST(MethodStackTest, StaticMethodParameterTypes) {
    std::vector<ca::str::Utf8StringRef> noExcepts;
    MethodNode mn(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, toRef("add"), toRef("(II)I"), toRef(""), noExcepts);
    mn.maxStack = 2;
    mn.maxLocals = 2;

    MethodStack ms(mn);
    EXPECT_EQ(ms.maxStack(), 2);
    EXPECT_EQ(ms.maxLocals(), 2);
    EXPECT_EQ(ms.getLocalType(0), PrimitiveKind::INT);
    EXPECT_EQ(ms.getLocalType(1), PrimitiveKind::INT);
}

TEST(MethodStackTest, InstanceMethodHasThis) {
    std::vector<ca::str::Utf8StringRef> noExcepts;
    MethodNode mn(Opcodes::ACC_PUBLIC, toRef("getValue"), toRef("()I"), toRef(""), noExcepts);
    mn.maxLocals = 1;

    MethodStack ms(mn);
    EXPECT_EQ(ms.getLocalType(0), PrimitiveKind::OBJECT); // this
}

TEST(MethodStackTest, LongAndDoubleTakeTwoSlots) {
    std::vector<ca::str::Utf8StringRef> noExcepts;
    MethodNode mn(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, toRef("test"), toRef("(JD)V"), toRef(""), noExcepts);
    mn.maxLocals = 4;

    MethodStack ms(mn);
    EXPECT_EQ(ms.getLocalType(0), PrimitiveKind::LONG);   // slot 0-1
    EXPECT_EQ(ms.getLocalType(2), PrimitiveKind::DOUBLE); // slot 2-3
}

// ============================================================
// CodeConverter tests
// ============================================================

// Helper: create a MethodNode with given body instructions
static std::unique_ptr<MethodNode> makeMethod(
    int access, const std::string& name, const std::string& desc,
    std::initializer_list<AbstractInsnNode*> insns)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(access, toRef(name.c_str()), toRef(desc.c_str()), toRef(""), noExcepts);
    mn->maxStack = 10;
    mn->maxLocals = 10;
    for (auto* insn : insns)
        mn->instructions.add(insn);
    return mn;
}

TEST(CodeConverterTest, EmptyReturnsEntryBlock) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "empty", "()V", {});

    CodeConverter cc;
    auto blocks = cc.convert(*mn);

    ASSERT_GE(blocks.size(), 1);
    EXPECT_EQ(blocks[0]->id, 0); // entry block
    EXPECT_EQ(blocks[0]->nodes.size(), 0);
}

TEST(CodeConverterTest, SimpleReturn) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "ret", "()V", {new InsnNode(Opcodes::RETURN)});

    CodeConverter cc;
    auto blocks = cc.convert(*mn);

    ASSERT_GE(blocks.size(), 1);
    auto& nodes = blocks[0]->nodes;
    ASSERT_GE(nodes.size(), 1);
    EXPECT_EQ(nodes.back()->kind, MIRKind::RETURN);
}

TEST(CodeConverterTest, ReturnValue) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "retVal", "()I", {new InsnNode(Opcodes::IRETURN)});

    CodeConverter cc;
    auto blocks = cc.convert(*mn);

    auto& nodes = blocks[0]->nodes;
    EXPECT_EQ(nodes.back()->kind, MIRKind::RETURN_VALUE);
}

TEST(CodeConverterTest, LocalLoadStore) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "ldst", "(I)I", {
        new VarInsnNode(Opcodes::ILOAD, 0),
        new InsnNode(Opcodes::IRETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    // Find LOAD node
    bool foundLoad = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::LOCAL_LOAD) {
            EXPECT_EQ(static_cast<MIRLocalLoad*>(n.get())->varIndex, 0);
            foundLoad = true;
        }
    }
    EXPECT_TRUE(foundLoad);
}

TEST(CodeConverterTest, BinaryOp) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "add", "(II)I", {
        new VarInsnNode(Opcodes::ILOAD, 0),
        new VarInsnNode(Opcodes::ILOAD, 1),
        new InsnNode(Opcodes::IADD),
        new InsnNode(Opcodes::IRETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundBinary = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::BINARY_OP) {
            EXPECT_EQ(static_cast<MIRBinaryOp*>(n.get())->op, BinaryOp::ADD);
            foundBinary = true;
        }
    }
    EXPECT_TRUE(foundBinary);
}

TEST(CodeConverterTest, FieldAccess) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC,
                         "getX", "()I", {
        new VarInsnNode(Opcodes::ALOAD, 0), // this
        new FieldInsnNode(Opcodes::GETFIELD, toRef("Foo"), toRef("x"), toRef("I")),
        new InsnNode(Opcodes::IRETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundGet = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::FIELD_GET) {
            auto* fg = static_cast<MIRFieldGet*>(n.get());
            EXPECT_EQ(fg->name, "x");
            EXPECT_EQ(fg->owner, "Foo");
            EXPECT_EQ(fg->desc, "I");
            foundGet = true;
        }
    }
    EXPECT_TRUE(foundGet);
}

TEST(CodeConverterTest, MethodInvoke) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "call", "()V", {
        new MethodInsnNode(Opcodes::INVOKESTATIC, toRef("Other"), toRef("run"), toRef("()V"), false),
        new InsnNode(Opcodes::RETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundInvoke = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::INVOKE) {
            auto* inv = static_cast<MIRInvoke*>(n.get());
            EXPECT_EQ(inv->name, "run");
            EXPECT_EQ(inv->owner, "Other");
            foundInvoke = true;
        }
    }
    EXPECT_TRUE(foundInvoke);
}

TEST(CodeConverterTest, UnconditionalBranch) {
    auto* label = new LabelNode();
    label->label->bytecodeOffset = 10;

    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "br", "()V", {
        new JumpInsnNode(Opcodes::GOTO, label->label),
        new InsnNode(Opcodes::RETURN),
        label,
        new InsnNode(Opcodes::RETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);

    ASSERT_GE(blocks.size(), 2);

    // Find entry block (id=0) and check it has GOTO
    Block* entryBlock = nullptr;
    for (auto* b : blocks) {
        if (b->id == 0) { entryBlock = b; break; }
    }
    ASSERT_NE(entryBlock, nullptr);

    bool foundGoto = false;
    for (auto& n : entryBlock->nodes) {
        if (n->kind == MIRKind::UNCOND_BRANCH) {
            EXPECT_EQ(static_cast<MIRUncondBranch*>(n.get())->targetLabel, 10);
            foundGoto = true;
        }
    }
    EXPECT_TRUE(foundGoto);
}

TEST(CodeConverterTest, DumpOutput) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "add", "(II)I", {
        new VarInsnNode(Opcodes::ILOAD, 0),
        new VarInsnNode(Opcodes::ILOAD, 1),
        new InsnNode(Opcodes::IADD),
        new InsnNode(Opcodes::IRETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto dump = CodeConverter::dumpMIR(blocks);

    EXPECT_FALSE(dump.empty());
    EXPECT_NE(dump.find("B0:"), std::string::npos);
    EXPECT_NE(dump.find("BINARY"), std::string::npos);
    EXPECT_NE(dump.find("RETURN_VALUE"), std::string::npos);
}

TEST(CodeConverterTest, ConstantInstructions) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "consts", "()V", {
        new InsnNode(Opcodes::ICONST_0),
        new InsnNode(Opcodes::ICONST_1),
        new InsnNode(Opcodes::LCONST_0),
        new InsnNode(Opcodes::FCONST_0),
        new InsnNode(Opcodes::DCONST_0),
        new InsnNode(Opcodes::ACONST_NULL),
        new InsnNode(Opcodes::POP2), // clean up long
        new InsnNode(Opcodes::POP2), // clean up double
        new InsnNode(Opcodes::POP),  // clean up float
        new InsnNode(Opcodes::POP2), // clean up null + iconst_0
        new InsnNode(Opcodes::RETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    int constCount = 0;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CONSTANT) constCount++;
    }
    EXPECT_EQ(constCount, 6);
}

TEST(CodeConverterTest, NewObjectAndInvokeSpecial) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "create", "()Ljava/lang/Object;", {
        new TypeInsnNode(Opcodes::NEW, toRef("java/lang/Object")),
        new InsnNode(Opcodes::DUP),
        new MethodInsnNode(Opcodes::INVOKESPECIAL, toRef("java/lang/Object"), toRef("<init>"), toRef("()V"), false),
        new InsnNode(Opcodes::ARETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundNew = false, foundDup = false, foundInvoke = false;
    for (auto& n : nodes) {
        switch (n->kind) {
        case MIRKind::NEW_OBJECT:
            EXPECT_EQ(static_cast<MIRNewObject*>(n.get())->typeDesc, "java/lang/Object");
            foundNew = true;
            break;
        case MIRKind::STACK_DUP:
            foundDup = true;
            break;
        case MIRKind::INVOKE:
            EXPECT_EQ(static_cast<MIRInvoke*>(n.get())->name, "<init>");
            foundInvoke = true;
            break;
        default:
            break;
        }
    }
    EXPECT_TRUE(foundNew);
    EXPECT_TRUE(foundDup);
    EXPECT_TRUE(foundInvoke);
}

TEST(CodeConverterTest, SwitchInstructions) {
    auto* defaultLabel = new LabelNode();
    defaultLabel->label->bytecodeOffset = 100;
    auto* caseLabel = new LabelNode();
    caseLabel->label->bytecodeOffset = 50;

    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "sw", "()V", {
        new TableSwitchInsnNode(0, 10, defaultLabel->label, {caseLabel->label}),
        defaultLabel,
        new InsnNode(Opcodes::RETURN),
        caseLabel,
        new InsnNode(Opcodes::RETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);

    ASSERT_GE(blocks.size(), 1);

    // Find entry block
    Block* entryBlock = nullptr;
    for (auto* b : blocks) {
        if (b->id == 0) { entryBlock = b; break; }
    }
    ASSERT_NE(entryBlock, nullptr);

    bool foundSwitch = false;
    for (auto& n : entryBlock->nodes) {
        if (n->kind == MIRKind::SWITCH) {
            auto* sw = static_cast<MIRSwitch*>(n.get());
            EXPECT_EQ(sw->defaultLabel, 100);
            ASSERT_EQ(sw->cases.size(), 1);
            foundSwitch = true;
        }
    }
    EXPECT_TRUE(foundSwitch);
}
