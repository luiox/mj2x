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

TEST(OpcodeUtilsTest, ResultTypeMapping) {
    EXPECT_EQ(opcodeToResultType(Opcodes::IADD), PrimitiveKind::INT);
    EXPECT_EQ(opcodeToResultType(Opcodes::LADD), PrimitiveKind::LONG);
    EXPECT_EQ(opcodeToResultType(Opcodes::FREM), PrimitiveKind::FLOAT);
    EXPECT_EQ(opcodeToResultType(Opcodes::DDIV), PrimitiveKind::DOUBLE);
    EXPECT_EQ(opcodeToResultType(Opcodes::LSHL), PrimitiveKind::LONG);
    EXPECT_EQ(opcodeToResultType(Opcodes::DNEG), PrimitiveKind::DOUBLE);
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
    MethodNode mn(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, ca::str::Utf8StringRef::from_cstr("add"), ca::str::Utf8StringRef::from_cstr("(II)I"), ca::str::Utf8StringRef::from_cstr(""), noExcepts);
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
    MethodNode mn(Opcodes::ACC_PUBLIC, ca::str::Utf8StringRef::from_cstr("getValue"), ca::str::Utf8StringRef::from_cstr("()I"), ca::str::Utf8StringRef::from_cstr(""), noExcepts);
    mn.maxLocals = 1;

    MethodStack ms(mn);
    EXPECT_EQ(ms.getLocalType(0), PrimitiveKind::OBJECT); // this
}

TEST(MethodStackTest, LongAndDoubleTakeTwoSlots) {
    std::vector<ca::str::Utf8StringRef> noExcepts;
    MethodNode mn(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, ca::str::Utf8StringRef::from_cstr("test"), ca::str::Utf8StringRef::from_cstr("(JD)V"), ca::str::Utf8StringRef::from_cstr(""), noExcepts);
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
    auto mn = std::make_unique<MethodNode>(access, ca::str::Utf8StringRef::from_cstr(name.c_str()), ca::str::Utf8StringRef::from_cstr(desc.c_str()), ca::str::Utf8StringRef::from_cstr(""), noExcepts);
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

TEST(CodeConverterTest, CastAndNumericCompare) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "casts", "(ID)I", {
        new VarInsnNode(Opcodes::ILOAD, 0),
        new InsnNode(Opcodes::I2L),
        new VarInsnNode(Opcodes::DLOAD, 1),
        new InsnNode(Opcodes::D2I),
        new InsnNode(Opcodes::LCMP),
        new InsnNode(Opcodes::IRETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundI2L = false;
    bool foundD2I = false;
    bool foundD2L = false;
    bool foundCmp = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CAST) {
            auto* cast = static_cast<MIRCast*>(n.get());
            if (cast->from == PrimitiveKind::INT && cast->to == PrimitiveKind::LONG)
                foundI2L = true;
            if (cast->from == PrimitiveKind::DOUBLE && cast->to == PrimitiveKind::INT)
                foundD2I = true;
            if (cast->from == PrimitiveKind::DOUBLE && cast->to == PrimitiveKind::LONG)
                foundD2L = true;
        } else if (n->kind == MIRKind::CMP) {
            auto* cmp = static_cast<MIRCmp*>(n.get());
            EXPECT_EQ(cmp->type, PrimitiveKind::LONG);
            EXPECT_FALSE(cmp->nanGreater);
            foundCmp = true;
        }
    }
    EXPECT_TRUE(foundI2L);
    EXPECT_TRUE(foundD2I);
    EXPECT_FALSE(foundD2L);
    EXPECT_TRUE(foundCmp);
}

TEST(CodeConverterTest, DoubleToLongCast) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "d2l", "(D)J", {
        new VarInsnNode(Opcodes::DLOAD, 0),
        new InsnNode(Opcodes::D2L),
        new InsnNode(Opcodes::LRETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundD2L = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CAST) {
            auto* cast = static_cast<MIRCast*>(n.get());
            if (cast->from == PrimitiveKind::DOUBLE && cast->to == PrimitiveKind::LONG)
                foundD2L = true;
        }
    }
    EXPECT_TRUE(foundD2L);
}

TEST(CodeConverterTest, FloatingCompareNaNDirection) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "cmp", "(FFDD)I", {
        new VarInsnNode(Opcodes::FLOAD, 0),
        new VarInsnNode(Opcodes::FLOAD, 1),
        new InsnNode(Opcodes::FCMPG),
        new VarInsnNode(Opcodes::DLOAD, 2),
        new VarInsnNode(Opcodes::DLOAD, 4),
        new InsnNode(Opcodes::DCMPL),
        new InsnNode(Opcodes::IADD),
        new InsnNode(Opcodes::IRETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundFcmpg = false;
    bool foundDcmpl = false;
    for (auto& n : nodes) {
        if (n->kind != MIRKind::CMP)
            continue;
        auto* cmp = static_cast<MIRCmp*>(n.get());
        if (cmp->type == PrimitiveKind::FLOAT && cmp->nanGreater)
            foundFcmpg = true;
        if (cmp->type == PrimitiveKind::DOUBLE && !cmp->nanGreater)
            foundDcmpl = true;
    }
    EXPECT_TRUE(foundFcmpg);
    EXPECT_TRUE(foundDcmpl);
}

TEST(CodeConverterTest, FieldAccess) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC,
                         "getX", "()I", {
        new VarInsnNode(Opcodes::ALOAD, 0), // this
        new FieldInsnNode(Opcodes::GETFIELD, ca::str::Utf8StringRef::from_cstr("Foo"), ca::str::Utf8StringRef::from_cstr("x"), ca::str::Utf8StringRef::from_cstr("I")),
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
        new MethodInsnNode(Opcodes::INVOKESTATIC, ca::str::Utf8StringRef::from_cstr("Other"), ca::str::Utf8StringRef::from_cstr("run"), ca::str::Utf8StringRef::from_cstr("()V"), false),
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

TEST(CodeConverterTest, StringLdcKeepsLiteralValue) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "literal", "()Ljava/lang/String;", {
        new LdcInsnNode(std::string("hello \"jni\"")),
        new InsnNode(Opcodes::ARETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundString = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CONSTANT) {
            auto* c = static_cast<MIRConstant*>(n.get());
            if (c->type == PrimitiveKind::OBJECT && std::holds_alternative<std::string>(c->value)) {
                EXPECT_EQ(std::get<std::string>(c->value), "hello \"jni\"");
                foundString = true;
            }
        }
    }
    EXPECT_TRUE(foundString);
}

TEST(CodeConverterTest, ArrayInstructionsCarryComponentType) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "arrays", "()V", {
        new InsnNode(Opcodes::ICONST_2),
        new IntInsnNode(Opcodes::NEWARRAY, Opcodes::T_LONG),
        new InsnNode(Opcodes::DUP),
        new InsnNode(Opcodes::ICONST_0),
        new InsnNode(Opcodes::LCONST_1),
        new InsnNode(Opcodes::LASTORE),
        new InsnNode(Opcodes::DUP),
        new InsnNode(Opcodes::ICONST_0),
        new InsnNode(Opcodes::LALOAD),
        new InsnNode(Opcodes::POP2),
        new InsnNode(Opcodes::POP),
        new InsnNode(Opcodes::RETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundNewLongArray = false;
    bool foundLongStore = false;
    bool foundLongLoad = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::NEW_ARRAY) {
            auto* a = static_cast<MIRNewArray*>(n.get());
            if (a->componentType == PrimitiveKind::LONG)
                foundNewLongArray = true;
        } else if (n->kind == MIRKind::ARRAY_STORE) {
            auto* s = static_cast<MIRArrayStore*>(n.get());
            if (s->componentType == PrimitiveKind::LONG)
                foundLongStore = true;
        } else if (n->kind == MIRKind::ARRAY_LOAD) {
            auto* l = static_cast<MIRArrayLoad*>(n.get());
            if (l->componentType == PrimitiveKind::LONG)
                foundLongLoad = true;
        }
    }
    EXPECT_TRUE(foundNewLongArray);
    EXPECT_TRUE(foundLongStore);
    EXPECT_TRUE(foundLongLoad);
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
            // targetLabel 现在是 Label* 身份（不再用 bytecodeOffset，见 R1a.3）
            EXPECT_EQ(static_cast<MIRUncondBranch*>(n.get())->targetLabel, label->label);
            foundGoto = true;
        }
    }
    EXPECT_TRUE(foundGoto);
}

TEST(CodeConverterTest, JsrAndRetBecomeUnsupportedBoundaryNodes) {
    auto* label = new LabelNode();
    label->label->bytecodeOffset = 10;

    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "legacySubroutine", "()V", {
        new JumpInsnNode(Opcodes::JSR, label->label),
        new InsnNode(Opcodes::RETURN),
        label,
        new VarInsnNode(Opcodes::RET, 0),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);

    bool foundJsr = false;
    bool foundRet = false;
    for (auto* block : blocks) {
        for (auto& n : block->nodes) {
            if (n->kind != MIRKind::INVOKE_DYNAMIC) continue;
            auto* unsupported = static_cast<MIRInvokeDynamic*>(n.get());
            foundJsr = foundJsr || unsupported->name == "jsr";
            foundRet = foundRet || unsupported->name == "ret";
        }
    }

    EXPECT_TRUE(foundJsr);
    EXPECT_TRUE(foundRet);
}

TEST(CodeConverterTest, InvokeDynamicBecomesUnsupportedBoundaryNode) {
    auto bsm = Handle(Opcodes::H_INVOKESTATIC,
                      ca::str::Utf8StringRef::from_cstr("java/lang/invoke/StringConcatFactory"),
                      ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants"),
                      ca::str::Utf8StringRef::from_cstr("()V"),
                      false);
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "concat", "()Ljava/lang/String;", {
        new InvokeDynamicInsnNode(ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants"),
                                  ca::str::Utf8StringRef::from_cstr("()Ljava/lang/String;"),
                                  bsm, {}),
        new InsnNode(Opcodes::ARETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    ASSERT_GE(blocks.size(), 1u);

    bool foundIndy = false;
    for (auto* block : blocks) {
        for (auto& n : block->nodes) {
            if (n->kind != MIRKind::INVOKE_DYNAMIC) continue;
            auto* unsupported = static_cast<MIRInvokeDynamic*>(n.get());
            foundIndy = unsupported->name == "makeConcatWithConstants" &&
                        unsupported->desc == "()Ljava/lang/String;";
        }
    }

    EXPECT_TRUE(foundIndy);
}

TEST(CodeConverterTest, IfNullBranchZeroHasObjectType) {
    auto* label = new LabelNode();
    label->label->bytecodeOffset = 10;

    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "isNull", "(Ljava/lang/Object;)I", {
        new VarInsnNode(Opcodes::ALOAD, 0),
        new JumpInsnNode(Opcodes::IFNULL, label->label),
        new InsnNode(Opcodes::ICONST_0),
        new InsnNode(Opcodes::IRETURN),
        label,
        new InsnNode(Opcodes::ICONST_1),
        new InsnNode(Opcodes::IRETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);

    bool foundObjectNullBranch = false;
    for (auto* block : blocks) {
        for (auto& n : block->nodes) {
            if (n->kind == MIRKind::BRANCH_ZERO) {
                auto* bz = static_cast<MIRBranchZero*>(n.get());
                EXPECT_EQ(bz->op, CmpOp::EQ);
                EXPECT_EQ(bz->type, PrimitiveKind::OBJECT);
                EXPECT_EQ(bz->targetLabel, label->label);
                foundObjectNullBranch = true;
            }
        }
    }
    EXPECT_TRUE(foundObjectNullBranch);
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

TEST(CodeConverterTest, StackShapeInstructionsArePreserved) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "stackShape", "()V", {
        new InsnNode(Opcodes::ICONST_0),
        new InsnNode(Opcodes::POP),
        new InsnNode(Opcodes::LCONST_0),
        new InsnNode(Opcodes::POP2),
        new InsnNode(Opcodes::ICONST_1),
        new InsnNode(Opcodes::ICONST_2),
        new InsnNode(Opcodes::ICONST_3),
        new InsnNode(Opcodes::DUP2_X1),
        new InsnNode(Opcodes::POP2),
        new InsnNode(Opcodes::POP2),
        new InsnNode(Opcodes::ICONST_1),
        new InsnNode(Opcodes::ICONST_2),
        new InsnNode(Opcodes::ICONST_3),
        new InsnNode(Opcodes::ICONST_4),
        new InsnNode(Opcodes::DUP2_X2),
        new InsnNode(Opcodes::RETURN),
    });

    CodeConverter cc;
    auto blocks = cc.convert(*mn);
    auto& nodes = blocks[0]->nodes;

    bool foundPop = false;
    bool foundPop2 = false;
    bool foundDup2X1 = false;
    bool foundDup2X2 = false;
    for (auto& n : nodes) {
        foundPop = foundPop || n->kind == MIRKind::STACK_POP;
        foundPop2 = foundPop2 || n->kind == MIRKind::STACK_POP2;
        foundDup2X1 = foundDup2X1 || n->kind == MIRKind::STACK_DUP2_X1;
        foundDup2X2 = foundDup2X2 || n->kind == MIRKind::STACK_DUP2_X2;
    }

    EXPECT_TRUE(foundPop);
    EXPECT_TRUE(foundPop2);
    EXPECT_TRUE(foundDup2X1);
    EXPECT_TRUE(foundDup2X2);
}

TEST(CodeConverterTest, NewObjectAndInvokeSpecial) {
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "create", "()Ljava/lang/Object;", {
        new TypeInsnNode(Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
        new InsnNode(Opcodes::DUP),
        new MethodInsnNode(Opcodes::INVOKESPECIAL, ca::str::Utf8StringRef::from_cstr("java/lang/Object"), ca::str::Utf8StringRef::from_cstr("<init>"), ca::str::Utf8StringRef::from_cstr("()V"), false),
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
            // defaultLabel 现在是 Label* 身份（不再用 bytecodeOffset，见 R1a.3）
            EXPECT_EQ(sw->defaultLabel, defaultLabel->label);
            ASSERT_EQ(sw->cases.size(), 1);
            foundSwitch = true;
        }
    }
    EXPECT_TRUE(foundSwitch);
}

