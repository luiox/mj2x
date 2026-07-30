#include <gmock/gmock.h>

#include <type_traits>

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

namespace {}

// ============================================================
// TypeUtils tests
// ============================================================

TEST(TypeUtilsTest, JvmSortToPrimitive)
{
    EXPECT_EQ(jvmSortToPrimitive(Type::INT), PrimitiveKind::INT);
    EXPECT_EQ(jvmSortToPrimitive(Type::LONG), PrimitiveKind::LONG);
    EXPECT_EQ(jvmSortToPrimitive(Type::FLOAT), PrimitiveKind::FLOAT);
    EXPECT_EQ(jvmSortToPrimitive(Type::DOUBLE), PrimitiveKind::DOUBLE);
    EXPECT_EQ(jvmSortToPrimitive(Type::BOOLEAN), PrimitiveKind::BOOLEAN);
    EXPECT_EQ(jvmSortToPrimitive(Type::VOID), PrimitiveKind::VOID);
    EXPECT_EQ(jvmSortToPrimitive(99), PrimitiveKind::OBJECT);   // unknown → OBJECT
}

TEST(TypeUtilsTest, JvmDescToPrimitive)
{
    EXPECT_EQ(jvmDescToPrimitive("I"), PrimitiveKind::INT);
    EXPECT_EQ(jvmDescToPrimitive("J"), PrimitiveKind::LONG);
    EXPECT_EQ(jvmDescToPrimitive("V"), PrimitiveKind::VOID);
    EXPECT_EQ(jvmDescToPrimitive("Ljava/lang/Object;"), PrimitiveKind::OBJECT);
    EXPECT_EQ(jvmDescToPrimitive("[I"), PrimitiveKind::OBJECT);
}

TEST(TypeUtilsTest, PrimitiveStackSize)
{
    EXPECT_EQ(primitiveStackSize(PrimitiveKind::INT), 1);
    EXPECT_EQ(primitiveStackSize(PrimitiveKind::LONG), 2);
    EXPECT_EQ(primitiveStackSize(PrimitiveKind::DOUBLE), 2);
    EXPECT_EQ(primitiveStackSize(PrimitiveKind::OBJECT), 1);
}

// ============================================================
// OpcodeUtils tests
// ============================================================

TEST(OpcodeUtilsTest, BinaryOpMapping)
{
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

TEST(OpcodeUtilsTest, ResultTypeMapping)
{
    EXPECT_EQ(opcodeToResultType(Opcodes::IADD), PrimitiveKind::INT);
    EXPECT_EQ(opcodeToResultType(Opcodes::LADD), PrimitiveKind::LONG);
    EXPECT_EQ(opcodeToResultType(Opcodes::FREM), PrimitiveKind::FLOAT);
    EXPECT_EQ(opcodeToResultType(Opcodes::DDIV), PrimitiveKind::DOUBLE);
    EXPECT_EQ(opcodeToResultType(Opcodes::LSHL), PrimitiveKind::LONG);
    EXPECT_EQ(opcodeToResultType(Opcodes::DNEG), PrimitiveKind::DOUBLE);
}

TEST(OpcodeUtilsTest, BranchOpcodeMapping)
{
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPEQ), CmpOp::EQ);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPNE), CmpOp::NE);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPLT), CmpOp::LT);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPGE), CmpOp::GE);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPGT), CmpOp::GT);
    EXPECT_EQ(branchOpcodeToCmpOp(Opcodes::IF_ICMPLE), CmpOp::LE);
}

TEST(OpcodeUtilsTest, BranchZeroOpcodeMapping)
{
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

TEST(MethodStackTest, StaticMethodParameterTypes)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    MethodNode                          mn(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                  ca::str::Utf8StringRef::from_cstr("add"),
                  ca::str::Utf8StringRef::from_cstr("(II)I"),
                  ca::str::Utf8StringRef::from_cstr(""),
                  noExcepts);
    mn.maxStack  = 2;
    mn.maxLocals = 2;

    MethodStack ms(mn);
    EXPECT_EQ(ms.maxStack(), 2);
    EXPECT_EQ(ms.maxLocals(), 2);
    EXPECT_EQ(ms.getLocalType(0), PrimitiveKind::INT);
    EXPECT_EQ(ms.getLocalType(1), PrimitiveKind::INT);
}

TEST(MethodStackTest, InstanceMethodHasThis)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    MethodNode                          mn(Opcodes::ACC_PUBLIC,
                  ca::str::Utf8StringRef::from_cstr("getValue"),
                  ca::str::Utf8StringRef::from_cstr("()I"),
                  ca::str::Utf8StringRef::from_cstr(""),
                  noExcepts);
    mn.maxLocals = 1;

    MethodStack ms(mn);
    EXPECT_EQ(ms.getLocalType(0), PrimitiveKind::OBJECT);   // this
}

TEST(MethodStackTest, LongAndDoubleTakeTwoSlots)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    MethodNode                          mn(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                  ca::str::Utf8StringRef::from_cstr("test"),
                  ca::str::Utf8StringRef::from_cstr("(JD)V"),
                  ca::str::Utf8StringRef::from_cstr(""),
                  noExcepts);
    mn.maxLocals = 4;

    MethodStack ms(mn);
    EXPECT_EQ(ms.getLocalType(0), PrimitiveKind::LONG);     // slot 0-1
    EXPECT_EQ(ms.getLocalType(2), PrimitiveKind::DOUBLE);   // slot 2-3
}

// ============================================================
// CodeConverter tests
// ============================================================

// Helper: create a MethodNode with given body instructions
static std::unique_ptr<MethodNode> makeMethod(int access, const std::string& name,
                                              const std::string&                       desc,
                                              std::initializer_list<AbstractInsnNode*> insns)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto&                               arena = Type::staticArena();
    auto                                mn    = std::make_unique<MethodNode>(access,
                                           arena.intern(name.c_str()),
                                           arena.intern(desc.c_str()),
                                           ca::str::Utf8StringRef::from_cstr(""),
                                           noExcepts);
    mn->maxStack                              = 10;
    mn->maxLocals                             = 10;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

TEST(CodeConverterTest, EmptyReturnsEntryBlock)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "empty", "()V", {});

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);

    ASSERT_GE(blocks.size(), 1);
    EXPECT_EQ(blocks[0]->id, 0);   // entry block
    EXPECT_EQ(blocks[0]->nodes.size(), 0);
}

TEST(CodeConverterTest, SimpleReturn)
{
    auto mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "ret", "()V", {new InsnNode(Opcodes::RETURN)});

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);

    ASSERT_GE(blocks.size(), 1);
    auto& nodes = blocks[0]->nodes;
    ASSERT_GE(nodes.size(), 1);
    EXPECT_EQ(nodes.back()->kind, MIRKind::RETURN);
}

TEST(CodeConverterTest, ReturnValue)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "retVal",
                         "()I",
                         {new InsnNode(Opcodes::IRETURN)});

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);

    auto& nodes = blocks[0]->nodes;
    EXPECT_EQ(nodes.back()->kind, MIRKind::RETURN_VALUE);
}

TEST(CodeConverterTest, LocalLoadStore)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "ldst",
                         "(I)I",
                         {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new InsnNode(Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    // Find LOAD node
    bool foundLoad = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::LOCAL_LOAD) {
            EXPECT_EQ(typed::cast<MIRLocalLoad>(n.get())->varIndex, 0);
            foundLoad = true;
        }
    }
    EXPECT_TRUE(foundLoad);
}

TEST(CodeConverterTest, BinaryOp)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "add",
                         "(II)I",
                         {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new VarInsnNode(Opcodes::ILOAD, 1),
                             new InsnNode(Opcodes::IADD),
                             new InsnNode(Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundBinary = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::BINARY_OP) {
            EXPECT_EQ(typed::cast<MIRBinaryOp>(n.get())->op, BinaryOp::ADD);
            foundBinary = true;
        }
    }
    EXPECT_TRUE(foundBinary);
}

TEST(CodeConverterTest, CastAndNumericCompare)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "casts",
                         "(ID)I",
                         {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new InsnNode(Opcodes::I2L),
                             new VarInsnNode(Opcodes::DLOAD, 1),
                             new InsnNode(Opcodes::D2I),
                             new InsnNode(Opcodes::LCMP),
                             new InsnNode(Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundI2L = false;
    bool foundD2I = false;
    bool foundD2L = false;
    bool foundCmp = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CAST) {
            auto* cast = typed::cast<MIRCast>(n.get());
            if (cast->from == PrimitiveKind::INT && cast->to == PrimitiveKind::LONG)
                foundI2L = true;
            if (cast->from == PrimitiveKind::DOUBLE && cast->to == PrimitiveKind::INT)
                foundD2I = true;
            if (cast->from == PrimitiveKind::DOUBLE && cast->to == PrimitiveKind::LONG)
                foundD2L = true;
        }
        else if (n->kind == MIRKind::CMP) {
            auto* cmp = typed::cast<MIRCmp>(n.get());
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

TEST(CodeConverterTest, DoubleToLongCast)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "d2l",
                         "(D)J",
                         {
                             new VarInsnNode(Opcodes::DLOAD, 0),
                             new InsnNode(Opcodes::D2L),
                             new InsnNode(Opcodes::LRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundD2L = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CAST) {
            auto* cast = typed::cast<MIRCast>(n.get());
            if (cast->from == PrimitiveKind::DOUBLE && cast->to == PrimitiveKind::LONG)
                foundD2L = true;
        }
    }
    EXPECT_TRUE(foundD2L);
}

TEST(CodeConverterTest, FloatingCompareNaNDirection)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "cmp",
                         "(FFDD)I",
                         {
                             new VarInsnNode(Opcodes::FLOAD, 0),
                             new VarInsnNode(Opcodes::FLOAD, 1),
                             new InsnNode(Opcodes::FCMPG),
                             new VarInsnNode(Opcodes::DLOAD, 2),
                             new VarInsnNode(Opcodes::DLOAD, 4),
                             new InsnNode(Opcodes::DCMPL),
                             new InsnNode(Opcodes::IADD),
                             new InsnNode(Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundFcmpg = false;
    bool foundDcmpl = false;
    for (auto& n : nodes) {
        if (n->kind != MIRKind::CMP) continue;
        auto* cmp = typed::cast<MIRCmp>(n.get());
        if (cmp->type == PrimitiveKind::FLOAT && cmp->nanGreater) foundFcmpg = true;
        if (cmp->type == PrimitiveKind::DOUBLE && !cmp->nanGreater) foundDcmpl = true;
    }
    EXPECT_TRUE(foundFcmpg);
    EXPECT_TRUE(foundDcmpl);
}

TEST(CodeConverterTest, FieldAccess)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC,
                         "getX",
                         "()I",
                         {
                             new VarInsnNode(Opcodes::ALOAD, 0),   // this
                             new FieldInsnNode(Opcodes::GETFIELD,
                                               ca::str::Utf8StringRef::from_cstr("Foo"),
                                               ca::str::Utf8StringRef::from_cstr("x"),
                                               ca::str::Utf8StringRef::from_cstr("I")),
                             new InsnNode(Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundGet = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::FIELD_GET) {
            auto* fg = typed::cast<MIRFieldGet>(n.get());
            EXPECT_EQ(fg->name, "x");
            EXPECT_EQ(fg->owner, "Foo");
            EXPECT_EQ(fg->desc, "I");
            foundGet = true;
        }
    }
    EXPECT_TRUE(foundGet);
}

TEST(CodeConverterTest, MethodInvoke)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "call",
                         "()V",
                         {
                             new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                ca::str::Utf8StringRef::from_cstr("Other"),
                                                ca::str::Utf8StringRef::from_cstr("run"),
                                                ca::str::Utf8StringRef::from_cstr("()V"),
                                                false),
                             new InsnNode(Opcodes::RETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundInvoke = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::INVOKE) {
            auto* inv = typed::cast<MIRInvoke>(n.get());
            EXPECT_EQ(inv->name, "run");
            EXPECT_EQ(inv->owner, "Other");
            foundInvoke = true;
        }
    }
    EXPECT_TRUE(foundInvoke);
}

TEST(CodeConverterTest, StringLdcKeepsLiteralValue)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "literal",
                         "()Ljava/lang/String;",
                         {
                             new LdcInsnNode(std::string("hello \"jni\"")),
                             new InsnNode(Opcodes::ARETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundString = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CONSTANT) {
            auto* c = typed::cast<MIRConstant>(n.get());
            if (c->type == PrimitiveKind::OBJECT &&
                std::holds_alternative<ca::str::Utf8StringRef>(c->value)) {
                EXPECT_EQ(std::get<ca::str::Utf8StringRef>(c->value).to_std_string(), "hello \"jni\"");
                foundString = true;
            }
        }
    }
    EXPECT_TRUE(foundString);
}

TEST(CodeConverterTest, ArrayInstructionsCarryComponentType)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "arrays",
                         "()V",
                         {
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

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundNewLongArray = false;
    bool foundLongStore    = false;
    bool foundLongLoad     = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::NEW_ARRAY) {
            auto* a = typed::cast<MIRNewArray>(n.get());
            if (a->componentType == PrimitiveKind::LONG) foundNewLongArray = true;
        }
        else if (n->kind == MIRKind::ARRAY_STORE) {
            auto* s = typed::cast<MIRArrayStore>(n.get());
            if (s->componentType == PrimitiveKind::LONG) foundLongStore = true;
        }
        else if (n->kind == MIRKind::ARRAY_LOAD) {
            auto* l = typed::cast<MIRArrayLoad>(n.get());
            if (l->componentType == PrimitiveKind::LONG) foundLongLoad = true;
        }
    }
    EXPECT_TRUE(foundNewLongArray);
    EXPECT_TRUE(foundLongStore);
    EXPECT_TRUE(foundLongLoad);
}

TEST(CodeConverterTest, UnconditionalBranch)
{
    auto* label                  = new LabelNode();
    label->label->bytecodeOffset = 10;

    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "br",
                         "()V",
                         {
                             new JumpInsnNode(Opcodes::GOTO, label->label),
                             new InsnNode(Opcodes::RETURN),
                             label,
                             new InsnNode(Opcodes::RETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);

    ASSERT_GE(blocks.size(), 2);

    // Find entry block (id=0) and check it has GOTO
    Block* entryBlock = nullptr;
    for (auto* b : blocks) {
        if (b->id == 0) {
            entryBlock = b;
            break;
        }
    }
    ASSERT_NE(entryBlock, nullptr);

    bool foundGoto = false;
    for (auto& n : entryBlock->nodes) {
        if (n->kind == MIRKind::UNCOND_BRANCH) {
            // targetLabel 现在是 Label* 身份（不再用 bytecodeOffset，见 R1a.3）
            EXPECT_EQ(typed::cast<MIRUncondBranch>(n.get())->targetLabel, label->label);
            foundGoto = true;
        }
    }
    EXPECT_TRUE(foundGoto);
}

TEST(CodeConverterTest, JsrAndRetBecomeUnsupportedBoundaryNodes)
{
    auto* label                  = new LabelNode();
    label->label->bytecodeOffset = 10;

    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "legacySubroutine",
                         "()V",
                         {
                             new JumpInsnNode(Opcodes::JSR, label->label),
                             new InsnNode(Opcodes::RETURN),
                             label,
                             new VarInsnNode(Opcodes::RET, 0),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);

    bool foundJsr = false;
    bool foundRet = false;
    for (auto* block : blocks) {
        for (auto& n : block->nodes) {
            if (n->kind != MIRKind::INVOKE_DYNAMIC) continue;
            auto* unsupported = typed::cast<MIRInvokeDynamic>(n.get());
            foundJsr          = foundJsr || unsupported->name == "jsr";
            foundRet          = foundRet || unsupported->name == "ret";
        }
    }

    EXPECT_TRUE(foundJsr);
    EXPECT_TRUE(foundRet);
}

TEST(CodeConverterTest, InvokeDynamicCarriesBootstrapMetadata)
{
    auto bsm = Handle(Opcodes::H_INVOKESTATIC,
                      ca::str::Utf8StringRef::from_cstr("java/lang/invoke/StringConcatFactory"),
                      ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants"),
                      ca::str::Utf8StringRef::from_cstr(
                          "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                          "Ljava/lang/invoke/MethodType;Ljava/lang/String;"
                          "[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;"),
                      false);
    auto mn  = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "concat",
        "()Ljava/lang/String;",
        {
            new InvokeDynamicInsnNode(ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants"),
                                      ca::str::Utf8StringRef::from_cstr("()Ljava/lang/String;"),
                                      bsm,
                                       {std::string("v=\x01")}),
            new InsnNode(Opcodes::ARETURN),
        });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    ASSERT_GE(blocks.size(), 1u);

    bool foundIndy = false;
    for (auto* block : blocks) {
        for (auto& n : block->nodes) {
            if (n->kind != MIRKind::INVOKE_DYNAMIC) continue;
            auto* unsupported = typed::cast<MIRInvokeDynamic>(n.get());
            foundIndy         = unsupported->name == "makeConcatWithConstants" &&
                        unsupported->desc == "()Ljava/lang/String;" &&
                        unsupported->bsm.has_value() &&
                        unsupported->bsm->getOwner().to_std_string() ==
                            "java/lang/invoke/StringConcatFactory" &&
                        unsupported->bsmArgs.size() == 1;
        }
    }

    EXPECT_TRUE(foundIndy);
}

TEST(CodeConverterTest, IfNullBranchZeroHasObjectType)
{
    auto* label                  = new LabelNode();
    label->label->bytecodeOffset = 10;

    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "isNull",
                         "(Ljava/lang/Object;)I",
                         {
                             new VarInsnNode(Opcodes::ALOAD, 0),
                             new JumpInsnNode(Opcodes::IFNULL, label->label),
                             new InsnNode(Opcodes::ICONST_0),
                             new InsnNode(Opcodes::IRETURN),
                             label,
                             new InsnNode(Opcodes::ICONST_1),
                             new InsnNode(Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);

    bool foundObjectNullBranch = false;
    for (auto* block : blocks) {
        for (auto& n : block->nodes) {
            if (n->kind == MIRKind::BRANCH_ZERO) {
                auto* bz = typed::cast<MIRBranchZero>(n.get());
                EXPECT_EQ(bz->op, CmpOp::EQ);
                EXPECT_EQ(bz->type, PrimitiveKind::OBJECT);
                EXPECT_EQ(bz->targetLabel, label->label);
                foundObjectNullBranch = true;
            }
        }
    }
    EXPECT_TRUE(foundObjectNullBranch);
}

TEST(CodeConverterTest, DumpOutput)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "add",
                         "(II)I",
                         {
                             new VarInsnNode(Opcodes::ILOAD, 0),
                             new VarInsnNode(Opcodes::ILOAD, 1),
                             new InsnNode(Opcodes::IADD),
                             new InsnNode(Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto          dump   = CodeConverter::dumpMIR(blocks);

    EXPECT_FALSE(dump.empty());
    EXPECT_NE(dump.find("B0:"), std::string::npos);
    EXPECT_NE(dump.find("BINARY"), std::string::npos);
    EXPECT_NE(dump.find("RETURN_VALUE"), std::string::npos);
}

TEST(CodeConverterTest, ConstantInstructions)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "consts",
                         "()V",
                         {
                             new InsnNode(Opcodes::ICONST_0),
                             new InsnNode(Opcodes::ICONST_1),
                             new InsnNode(Opcodes::LCONST_0),
                             new InsnNode(Opcodes::FCONST_0),
                             new InsnNode(Opcodes::DCONST_0),
                             new InsnNode(Opcodes::ACONST_NULL),
                             new InsnNode(Opcodes::POP2),   // clean up long
                             new InsnNode(Opcodes::POP2),   // clean up double
                             new InsnNode(Opcodes::POP),    // clean up float
                             new InsnNode(Opcodes::POP2),   // clean up null + iconst_0
                             new InsnNode(Opcodes::RETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    int constCount = 0;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CONSTANT) constCount++;
    }
    EXPECT_EQ(constCount, 6);
}

TEST(CodeConverterTest, StackShapeInstructionsArePreserved)
{
    auto mn = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                         "stackShape",
                         "()V",
                         {
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

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundPop      = false;
    bool foundPop2     = false;
    bool foundDup2X1   = false;
    bool foundDup2X2   = false;
    bool pop2IsWide    = false;
    bool dup2X1HasPair = false;
    bool dup2X2HasPair = false;
    for (auto& n : nodes) {
        foundPop = foundPop || n->kind == MIRKind::STACK_POP;
        if (n->kind == MIRKind::STACK_POP2) {
            foundPop2  = true;
            auto* op   = typed::cast<MIRStackOp>(n.get());
            pop2IsWide = pop2IsWide || (!op->stackBeforeTop.empty() &&
                                        op->stackBeforeTop[0] == PrimitiveKind::LONG);
        }
        if (n->kind == MIRKind::STACK_DUP2_X1) {
            foundDup2X1   = true;
            auto* op      = typed::cast<MIRStackOp>(n.get());
            dup2X1HasPair = op->stackBeforeTop.size() >= 3 &&
                            op->stackBeforeTop[0] == PrimitiveKind::INT &&
                            op->stackBeforeTop[1] == PrimitiveKind::INT &&
                            op->stackBeforeTop[2] == PrimitiveKind::INT;
        }
        if (n->kind == MIRKind::STACK_DUP2_X2) {
            foundDup2X2   = true;
            auto* op      = typed::cast<MIRStackOp>(n.get());
            dup2X2HasPair = op->stackBeforeTop.size() >= 4 &&
                            op->stackBeforeTop[0] == PrimitiveKind::INT &&
                            op->stackBeforeTop[1] == PrimitiveKind::INT &&
                            op->stackBeforeTop[2] == PrimitiveKind::INT &&
                            op->stackBeforeTop[3] == PrimitiveKind::INT;
        }
    }

    EXPECT_TRUE(foundPop);
    EXPECT_TRUE(foundPop2);
    EXPECT_TRUE(foundDup2X1);
    EXPECT_TRUE(foundDup2X2);
    EXPECT_TRUE(pop2IsWide);
    EXPECT_TRUE(dup2X1HasPair);
    EXPECT_TRUE(dup2X2HasPair);
}

TEST(CodeConverterTest, NewObjectAndInvokeSpecial)
{
    auto mn = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "create",
        "()Ljava/lang/Object;",
        {
            new TypeInsnNode(Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
            new InsnNode(Opcodes::DUP),
            new MethodInsnNode(Opcodes::INVOKESPECIAL,
                               ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                               ca::str::Utf8StringRef::from_cstr("<init>"),
                               ca::str::Utf8StringRef::from_cstr("()V"),
                               false),
            new InsnNode(Opcodes::ARETURN),
        });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);
    auto&         nodes  = blocks[0]->nodes;

    bool foundNew = false, foundDup = false, foundInvoke = false;
    for (auto& n : nodes) {
        switch (n->kind) {
        case MIRKind::NEW_OBJECT:
            EXPECT_EQ(typed::cast<MIRNewObject>(n.get())->typeDesc, "java/lang/Object");
            foundNew = true;
            break;
        case MIRKind::STACK_DUP: foundDup = true; break;
        case MIRKind::INVOKE:
            EXPECT_EQ(typed::cast<MIRInvoke>(n.get())->name, "<init>");
            foundInvoke = true;
            break;
        default: break;
        }
    }
    EXPECT_TRUE(foundNew);
    EXPECT_TRUE(foundDup);
    EXPECT_TRUE(foundInvoke);
}

TEST(CodeConverterTest, SwitchInstructions)
{
    auto* defaultLabel                  = new LabelNode();
    defaultLabel->label->bytecodeOffset = 100;
    auto* caseLabel                     = new LabelNode();
    caseLabel->label->bytecodeOffset    = 50;

    auto mn =
        makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                   "sw",
                   "()V",
                   {
                       new TableSwitchInsnNode(0, 10, defaultLabel->label, {caseLabel->label}),
                       defaultLabel,
                       new InsnNode(Opcodes::RETURN),
                       caseLabel,
                       new InsnNode(Opcodes::RETURN),
                   });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = cc.convert(*mn);

    ASSERT_GE(blocks.size(), 1);

    // Find entry block
    Block* entryBlock = nullptr;
    for (auto* b : blocks) {
        if (b->id == 0) {
            entryBlock = b;
            break;
        }
    }
    ASSERT_NE(entryBlock, nullptr);

    bool foundSwitch = false;
    for (auto& n : entryBlock->nodes) {
        if (n->kind == MIRKind::SWITCH) {
            auto* sw = typed::cast<MIRSwitch>(n.get());
            // defaultLabel 现在是 Label* 身份（不再用 bytecodeOffset，见 R1a.3）
            EXPECT_EQ(sw->defaultLabel, defaultLabel->label);
            ASSERT_EQ(sw->cases.size(), 1);
            foundSwitch = true;
        }
    }
    EXPECT_TRUE(foundSwitch);
}

// #698：MIR/SSA/RegIR 持久节点字段（JVM 符号 owner/name/desc/typeDesc + MIRConstant LDC
// String 槽）从自持 std::string 迁移到 arena-backed Utf8StringRef（spec §6.1）。
// 本测试做两层守卫：
//   1) 编译期类型断言——防止有人把字段类型悄悄改回 std::string；
//   2) 运行期内容 + lifetime 断言——确认 intern 后 ref 指向 arena 内副本、内容正确，
//      且 arena 析构前 ref 稳定（这是 #698 调试期发现的测试 harness lifetime 陷阱的回归门）。
TEST(CodeConverterTest, NodeFieldsHoldArenaBackedUtf8StringRef)
{
    // ---- 1) 编译期：节点字段类型必须是 Utf8StringRef（不是 std::string）----
    static_assert(std::is_same_v<decltype(MIRFieldGet::owner), ca::str::Utf8StringRef>,
                  "MIRFieldGet::owner must be Utf8StringRef (#698)");
    static_assert(std::is_same_v<decltype(MIRFieldGet::name), ca::str::Utf8StringRef>,
                  "MIRFieldGet::name must be Utf8StringRef (#698)");
    static_assert(std::is_same_v<decltype(MIRFieldGet::desc), ca::str::Utf8StringRef>,
                  "MIRFieldGet::desc must be Utf8StringRef (#698)");
    static_assert(std::is_same_v<decltype(MIRInvoke::owner), ca::str::Utf8StringRef>,
                  "MIRInvoke::owner must be Utf8StringRef (#698)");
    static_assert(std::is_same_v<decltype(MIRNewObject::typeDesc), ca::str::Utf8StringRef>,
                  "MIRNewObject::typeDesc must be Utf8StringRef (#698)");
    static_assert(std::is_same_v<decltype(MIRNewArray::typeDesc), ca::str::Utf8StringRef>,
                  "MIRNewArray::typeDesc must be Utf8StringRef (#698)");
    static_assert(std::is_same_v<decltype(MIRCheckCast::typeDesc), ca::str::Utf8StringRef>,
                  "MIRCheckCast::typeDesc must be Utf8StringRef (#698)");
    static_assert(std::is_same_v<decltype(MIRInstanceOf::typeDesc), ca::str::Utf8StringRef>,
                  "MIRInstanceOf::typeDesc must be Utf8StringRef (#698)");
    static_assert(std::is_same_v<decltype(MIRInvokeDynamic::name), ca::str::Utf8StringRef>,
                  "MIRInvokeDynamic::name must be Utf8StringRef (#698)");
    static_assert(std::is_same_v<decltype(MIRInvokeDynamic::desc), ca::str::Utf8StringRef>,
                  "MIRInvokeDynamic::desc must be Utf8StringRef (#698)");
    // MIRConstant 的 variant string 槽
    using ConstVariant = decltype(MIRConstant::value);
    static_assert(std::is_same_v<ConstVariant,
                                 std::variant<int64_t, double, std::nullptr_t,
                                              ca::str::Utf8StringRef, jbytecode::core::Type>>,
                  "MIRConstant::value string slot must hold Utf8StringRef (#698)");

    // ---- 2) 运行期：intern 后内容正确、且 ref 指向 arena 内副本（data() 不指向源 cstr）----
    auto mn = makeMethod(Opcodes::ACC_PUBLIC,
                         "getX",
                         "()I",
                         {
                             new VarInsnNode(Opcodes::ALOAD, 0),
                             new FieldInsnNode(Opcodes::GETFIELD,
                                               ca::str::Utf8StringRef::from_cstr("Foo"),
                                               ca::str::Utf8StringRef::from_cstr("x"),
                                               ca::str::Utf8StringRef::from_cstr("I")),
                             new InsnNode(Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);

    MIRFieldGet* fg = nullptr;
    for (auto& n : blocks[0]->nodes) {
        if (n->kind == MIRKind::FIELD_GET) {
            fg = typed::cast<MIRFieldGet>(n.get());
            break;
        }
    }
    ASSERT_NE(fg, nullptr);

    // 内容正确（Utf8StringRef operator== 接受 const char*）。
    EXPECT_EQ(fg->owner, "Foo");
    EXPECT_EQ(fg->name, "x");
    EXPECT_EQ(fg->desc, "I");

    // ref 指向 arena 内副本，不是源 cstr 字面量地址——这是 intern 去重生效的证据。
    // arena.intern 把内容拷进 arena chunk，源 cstr（栈上 from_cstr）与 arena 副本地址不同。
    const auto* srcOwnerCstr = "Foo";
    EXPECT_NE(static_cast<const void*>(fg->owner.data()),
              static_cast<const void*>(srcOwnerCstr));

    // 同一符号二次 intern 返回同一副本（去重），data() 指针相等。
    auto ownerAgain = arena.intern(ca::str::Utf8StringRef::from_cstr("Foo"));
    EXPECT_EQ(ownerAgain.data(), fg->owner.data());
}
