#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <type_traits>

#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/frontend/type_utils.hpp"
#include "mj2x/frontend/opcode_utils.hpp"
#include "jbytecode/jbytecode.hpp"

#include "test_helper.hpp"

// B8b：测试侧 Label 统一归宿（进程寿命 scratch arena）——LabelNode() 默认构造
// 已随旧池制度退役，Label 一律 arena 分配。
static garena::Arena& labelScratchArena()
{
    static garena::Arena arena;
    return arena;
}


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
    EXPECT_EQ(jvmSortToPrimitive(jbytecode::core::Type::INT), PrimitiveKind::INT);
    EXPECT_EQ(jvmSortToPrimitive(jbytecode::core::Type::LONG), PrimitiveKind::LONG);
    EXPECT_EQ(jvmSortToPrimitive(jbytecode::core::Type::FLOAT), PrimitiveKind::FLOAT);
    EXPECT_EQ(jvmSortToPrimitive(jbytecode::core::Type::DOUBLE), PrimitiveKind::DOUBLE);
    EXPECT_EQ(jvmSortToPrimitive(jbytecode::core::Type::BOOLEAN), PrimitiveKind::BOOLEAN);
    EXPECT_EQ(jvmSortToPrimitive(jbytecode::core::Type::VOID), PrimitiveKind::VOID);
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
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::IADD), BinaryOp::ADD);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::LADD), BinaryOp::ADD);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::ISUB), BinaryOp::SUB);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::IMUL), BinaryOp::MUL);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::IDIV), BinaryOp::DIV);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::IREM), BinaryOp::REM);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::ISHL), BinaryOp::SHL);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::ISHR), BinaryOp::SHR);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::IUSHR), BinaryOp::USHR);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::IAND), BinaryOp::AND);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::IOR), BinaryOp::OR);
    EXPECT_EQ(opcodeToBinaryOp(jbytecode::core::Opcodes::IXOR), BinaryOp::XOR);
}

TEST(OpcodeUtilsTest, ResultTypeMapping)
{
    EXPECT_EQ(opcodeToResultType(jbytecode::core::Opcodes::IADD), PrimitiveKind::INT);
    EXPECT_EQ(opcodeToResultType(jbytecode::core::Opcodes::LADD), PrimitiveKind::LONG);
    EXPECT_EQ(opcodeToResultType(jbytecode::core::Opcodes::FREM), PrimitiveKind::FLOAT);
    EXPECT_EQ(opcodeToResultType(jbytecode::core::Opcodes::DDIV), PrimitiveKind::DOUBLE);
    EXPECT_EQ(opcodeToResultType(jbytecode::core::Opcodes::LSHL), PrimitiveKind::LONG);
    EXPECT_EQ(opcodeToResultType(jbytecode::core::Opcodes::DNEG), PrimitiveKind::DOUBLE);
}

TEST(OpcodeUtilsTest, BranchOpcodeMapping)
{
    EXPECT_EQ(branchOpcodeToCmpOp(jbytecode::core::Opcodes::IF_ICMPEQ), CmpOp::EQ);
    EXPECT_EQ(branchOpcodeToCmpOp(jbytecode::core::Opcodes::IF_ICMPNE), CmpOp::NE);
    EXPECT_EQ(branchOpcodeToCmpOp(jbytecode::core::Opcodes::IF_ICMPLT), CmpOp::LT);
    EXPECT_EQ(branchOpcodeToCmpOp(jbytecode::core::Opcodes::IF_ICMPGE), CmpOp::GE);
    EXPECT_EQ(branchOpcodeToCmpOp(jbytecode::core::Opcodes::IF_ICMPGT), CmpOp::GT);
    EXPECT_EQ(branchOpcodeToCmpOp(jbytecode::core::Opcodes::IF_ICMPLE), CmpOp::LE);
}

TEST(OpcodeUtilsTest, BranchZeroOpcodeMapping)
{
    EXPECT_EQ(branchZeroOpcodeToCmpOp(jbytecode::core::Opcodes::IFEQ), CmpOp::EQ);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(jbytecode::core::Opcodes::IFNE), CmpOp::NE);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(jbytecode::core::Opcodes::IFLT), CmpOp::LT);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(jbytecode::core::Opcodes::IFGE), CmpOp::GE);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(jbytecode::core::Opcodes::IFGT), CmpOp::GT);
    EXPECT_EQ(branchZeroOpcodeToCmpOp(jbytecode::core::Opcodes::IFLE), CmpOp::LE);
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
    // #921 staticArena 退役：Type::staticArena() 已删除。测试夹具的节点名/描述符
    // 借用须活过本 helper 返回——用本测试文件私有静态池（测试专属，非产物路径）。
    static ca::str::Utf8StringArena     testArena;
    auto&                               arena = testArena;
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
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC, "empty", "()V", {});

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);

    ASSERT_GE(blocks.size(), 1);
    EXPECT_EQ(blocks[0]->id, 0);   // entry block
    EXPECT_EQ(blocks[0]->nodes.size(), 0);
}

TEST(CodeConverterTest, SimpleReturn)
{
    auto mn = makeMethod(
        jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC, "ret", "()V", {new InsnNode(jbytecode::core::Opcodes::RETURN)});

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);

    ASSERT_GE(blocks.size(), 1);
    auto& nodes = blocks[0]->nodes;
    ASSERT_GE(nodes.size(), 1);
    EXPECT_EQ(nodes.back()->kind, MIRKind::RETURN);
}

TEST(CodeConverterTest, ReturnValue)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "retVal",
                         "()I",
                         {new InsnNode(jbytecode::core::Opcodes::IRETURN)});

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);

    auto& nodes = blocks[0]->nodes;
    EXPECT_EQ(nodes.back()->kind, MIRKind::RETURN_VALUE);
}

TEST(CodeConverterTest, LocalLoadStore)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "ldst",
                         "(I)I",
                         {
                             new VarInsnNode(jbytecode::core::Opcodes::ILOAD, 0),
                             new InsnNode(jbytecode::core::Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    // Find LOAD node
    bool foundLoad = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::LOCAL_LOAD) {
            EXPECT_EQ(ca::core::tag_cast::cast<MIRLocalLoad>(n.get())->varIndex, 0);
            foundLoad = true;
        }
    }
    EXPECT_TRUE(foundLoad);
}

TEST(CodeConverterTest, BinaryOp)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "add",
                         "(II)I",
                         {
                             new VarInsnNode(jbytecode::core::Opcodes::ILOAD, 0),
                             new VarInsnNode(jbytecode::core::Opcodes::ILOAD, 1),
                             new InsnNode(jbytecode::core::Opcodes::IADD),
                             new InsnNode(jbytecode::core::Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    bool foundBinary = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::BINARY_OP) {
            EXPECT_EQ(ca::core::tag_cast::cast<MIRBinaryOp>(n.get())->op, BinaryOp::ADD);
            foundBinary = true;
        }
    }
    EXPECT_TRUE(foundBinary);
}

TEST(CodeConverterTest, CastAndNumericCompare)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "casts",
                         "(ID)I",
                         {
                             new VarInsnNode(jbytecode::core::Opcodes::ILOAD, 0),
                             new InsnNode(jbytecode::core::Opcodes::I2L),
                             new VarInsnNode(jbytecode::core::Opcodes::DLOAD, 1),
                             new InsnNode(jbytecode::core::Opcodes::D2I),
                             new InsnNode(jbytecode::core::Opcodes::LCMP),
                             new InsnNode(jbytecode::core::Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    bool foundI2L = false;
    bool foundD2I = false;
    bool foundD2L = false;
    bool foundCmp = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CAST) {
            auto* cast = ca::core::tag_cast::cast<MIRCast>(n.get());
            if (cast->from == PrimitiveKind::INT && cast->to == PrimitiveKind::LONG)
                foundI2L = true;
            if (cast->from == PrimitiveKind::DOUBLE && cast->to == PrimitiveKind::INT)
                foundD2I = true;
            if (cast->from == PrimitiveKind::DOUBLE && cast->to == PrimitiveKind::LONG)
                foundD2L = true;
        }
        else if (n->kind == MIRKind::CMP) {
            auto* cmp = ca::core::tag_cast::cast<MIRCmp>(n.get());
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
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "d2l",
                         "(D)J",
                         {
                             new VarInsnNode(jbytecode::core::Opcodes::DLOAD, 0),
                             new InsnNode(jbytecode::core::Opcodes::D2L),
                             new InsnNode(jbytecode::core::Opcodes::LRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    bool foundD2L = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CAST) {
            auto* cast = ca::core::tag_cast::cast<MIRCast>(n.get());
            if (cast->from == PrimitiveKind::DOUBLE && cast->to == PrimitiveKind::LONG)
                foundD2L = true;
        }
    }
    EXPECT_TRUE(foundD2L);
}

TEST(CodeConverterTest, FloatingCompareNaNDirection)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "cmp",
                         "(FFDD)I",
                         {
                             new VarInsnNode(jbytecode::core::Opcodes::FLOAD, 0),
                             new VarInsnNode(jbytecode::core::Opcodes::FLOAD, 1),
                             new InsnNode(jbytecode::core::Opcodes::FCMPG),
                             new VarInsnNode(jbytecode::core::Opcodes::DLOAD, 2),
                             new VarInsnNode(jbytecode::core::Opcodes::DLOAD, 4),
                             new InsnNode(jbytecode::core::Opcodes::DCMPL),
                             new InsnNode(jbytecode::core::Opcodes::IADD),
                             new InsnNode(jbytecode::core::Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    bool foundFcmpg = false;
    bool foundDcmpl = false;
    for (auto& n : nodes) {
        if (n->kind != MIRKind::CMP) continue;
        auto* cmp = ca::core::tag_cast::cast<MIRCmp>(n.get());
        if (cmp->type == PrimitiveKind::FLOAT && cmp->nanGreater) foundFcmpg = true;
        if (cmp->type == PrimitiveKind::DOUBLE && !cmp->nanGreater) foundDcmpl = true;
    }
    EXPECT_TRUE(foundFcmpg);
    EXPECT_TRUE(foundDcmpl);
}

TEST(CodeConverterTest, FieldAccess)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC,
                         "getX",
                         "()I",
                         {
                             new VarInsnNode(jbytecode::core::Opcodes::ALOAD, 0),   // this
                             new FieldInsnNode(jbytecode::core::Opcodes::GETFIELD,
                                               ca::str::Utf8StringRef::from_cstr("Foo"),
                                               ca::str::Utf8StringRef::from_cstr("x"),
                                               ca::str::Utf8StringRef::from_cstr("I")),
                             new InsnNode(jbytecode::core::Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    bool foundGet = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::FIELD_GET) {
            auto* fg = ca::core::tag_cast::cast<MIRFieldGet>(n.get());
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
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "call",
                         "()V",
                         {
                             new MethodInsnNode(jbytecode::core::Opcodes::INVOKESTATIC,
                                                ca::str::Utf8StringRef::from_cstr("Other"),
                                                ca::str::Utf8StringRef::from_cstr("run"),
                                                ca::str::Utf8StringRef::from_cstr("()V"),
                                                false),
                             new InsnNode(jbytecode::core::Opcodes::RETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    bool foundInvoke = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::INVOKE) {
            auto* inv = ca::core::tag_cast::cast<MIRInvoke>(n.get());
            EXPECT_EQ(inv->name, "run");
            EXPECT_EQ(inv->owner, "Other");
            foundInvoke = true;
        }
    }
    EXPECT_TRUE(foundInvoke);
}

TEST(CodeConverterTest, StringLdcKeepsLiteralValue)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "literal",
                         "()Ljava/lang/String;",
                         {
                             new LdcInsnNode(std::string("hello \"jni\"")),
                             new InsnNode(jbytecode::core::Opcodes::ARETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    bool foundString = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CONSTANT) {
            auto* c = ca::core::tag_cast::cast<MIRConstant>(n.get());
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
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "arrays",
                         "()V",
                         {
                             new InsnNode(jbytecode::core::Opcodes::ICONST_2),
                             new IntInsnNode(jbytecode::core::Opcodes::NEWARRAY, jbytecode::core::Opcodes::T_LONG),
                             new InsnNode(jbytecode::core::Opcodes::DUP),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_0),
                             new InsnNode(jbytecode::core::Opcodes::LCONST_1),
                             new InsnNode(jbytecode::core::Opcodes::LASTORE),
                             new InsnNode(jbytecode::core::Opcodes::DUP),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_0),
                             new InsnNode(jbytecode::core::Opcodes::LALOAD),
                             new InsnNode(jbytecode::core::Opcodes::POP2),
                             new InsnNode(jbytecode::core::Opcodes::POP),
                             new InsnNode(jbytecode::core::Opcodes::RETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    bool foundNewLongArray = false;
    bool foundLongStore    = false;
    bool foundLongLoad     = false;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::NEW_ARRAY) {
            auto* a = ca::core::tag_cast::cast<MIRNewArray>(n.get());
            if (a->componentType == PrimitiveKind::LONG) foundNewLongArray = true;
        }
        else if (n->kind == MIRKind::ARRAY_STORE) {
            auto* s = ca::core::tag_cast::cast<MIRArrayStore>(n.get());
            if (s->componentType == PrimitiveKind::LONG) foundLongStore = true;
        }
        else if (n->kind == MIRKind::ARRAY_LOAD) {
            auto* l = ca::core::tag_cast::cast<MIRArrayLoad>(n.get());
            if (l->componentType == PrimitiveKind::LONG) foundLongLoad = true;
        }
    }
    EXPECT_TRUE(foundNewLongArray);
    EXPECT_TRUE(foundLongStore);
    EXPECT_TRUE(foundLongLoad);
}

TEST(CodeConverterTest, UnconditionalBranch)
{
    auto* label                  = new LabelNode(labelScratchArena().create<Label>());
    label->label->bytecodeOffset = 10;

    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "br",
                         "()V",
                         {
                             new JumpInsnNode(jbytecode::core::Opcodes::GOTO, label->label),
                             new InsnNode(jbytecode::core::Opcodes::RETURN),
                             label,
                             new InsnNode(jbytecode::core::Opcodes::RETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);

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
            auto* gotoNode = ca::core::tag_cast::cast<MIRUncondBranch>(n.get());
            // 批 3b DOM 读路径：目标是 LabelPool 合成标签，与树侧 Label*
            // 身份等价性经「目标块内 MIRLabel 同指针」断言。
            bool targetAnchored = false;
            for (auto* b : blocks) {
                for (auto& nn : b->nodes) {
                    if (nn->kind == MIRKind::LABEL &&
                        static_cast<const MIRLabel*>(nn.get())->labelId ==
                            gotoNode->targetLabel) {
                        targetAnchored = true;
                    }
                }
            }
            EXPECT_TRUE(targetAnchored);
            foundGoto = true;
        }
    }
    EXPECT_TRUE(foundGoto);
}

TEST(CodeConverterTest, JsrAndRetBecomeUnsupportedBoundaryNodes)
{
    auto* label                  = new LabelNode(labelScratchArena().create<Label>());
    label->label->bytecodeOffset = 10;

    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "legacySubroutine",
                         "()V",
                         {
                             new JumpInsnNode(jbytecode::core::Opcodes::JSR, label->label),
                             new InsnNode(jbytecode::core::Opcodes::RETURN),
                             label,
                             new VarInsnNode(jbytecode::core::Opcodes::RET, 0),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);

    bool foundJsr = false;
    bool foundRet = false;
    for (auto* block : blocks) {
        for (auto& n : block->nodes) {
            if (n->kind != MIRKind::INVOKE_DYNAMIC) continue;
            auto* unsupported = ca::core::tag_cast::cast<MIRInvokeDynamic>(n.get());
            foundJsr          = foundJsr || unsupported->name == "jsr";
            foundRet          = foundRet || unsupported->name == "ret";
        }
    }

    EXPECT_TRUE(foundJsr);
    EXPECT_TRUE(foundRet);
}

TEST(CodeConverterTest, InvokeDynamicCarriesBootstrapMetadata)
{
    auto bsm = jbytecode::core::Handle(jbytecode::core::Opcodes::H_INVOKESTATIC,
                      ca::str::Utf8StringRef::from_cstr("java/lang/invoke/StringConcatFactory"),
                      ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants"),
                      ca::str::Utf8StringRef::from_cstr(
                          "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                          "Ljava/lang/invoke/MethodType;Ljava/lang/String;"
                          "[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;"),
                      false);
    auto mn  = makeMethod(
        jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
        "concat",
        "()Ljava/lang/String;",
        {
            new InvokeDynamicInsnNode(ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants"),
                                      ca::str::Utf8StringRef::from_cstr("()Ljava/lang/String;"),
                                      bsm,
                                      // 批 3b：垫片走字节回环，bsm arg 须是旧 writer
                                      // 可序列化的载荷（Utf8StringRef→CONSTANT_String）；
                                      // std::string 不在其分派内会写出废索引 0。
                                       {ca::core::Any(
                                           ca::str::Utf8StringRef::from_cstr("v=\x01"))}),
            new InsnNode(jbytecode::core::Opcodes::ARETURN),
        });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    ASSERT_GE(blocks.size(), 1u);

    bool foundIndy = false;
    for (auto* block : blocks) {
        for (auto& n : block->nodes) {
            if (n->kind != MIRKind::INVOKE_DYNAMIC) continue;
            auto* unsupported = ca::core::tag_cast::cast<MIRInvokeDynamic>(n.get());
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
    auto* label                  = new LabelNode(labelScratchArena().create<Label>());
    label->label->bytecodeOffset = 10;

    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "isNull",
                         "(Ljava/lang/Object;)I",
                         {
                             new VarInsnNode(jbytecode::core::Opcodes::ALOAD, 0),
                             new JumpInsnNode(jbytecode::core::Opcodes::IFNULL, label->label),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_0),
                             new InsnNode(jbytecode::core::Opcodes::IRETURN),
                             label,
                             new InsnNode(jbytecode::core::Opcodes::ICONST_1),
                             new InsnNode(jbytecode::core::Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);

    bool foundObjectNullBranch = false;
    for (auto* block : blocks) {
        for (auto& n : block->nodes) {
            if (n->kind == MIRKind::BRANCH_ZERO) {
                auto* bz = ca::core::tag_cast::cast<MIRBranchZero>(n.get());
                EXPECT_EQ(bz->op, CmpOp::EQ);
                EXPECT_EQ(bz->type, PrimitiveKind::OBJECT);
                // 批 3b DOM 读路径：目标是 LabelPool 合成标签，与树侧 Label*
                // 身份等价性经「目标块内 MIRLabel 同指针」断言。
                bool targetAnchored = false;
                for (auto* b : blocks) {
                    for (auto& nn : b->nodes) {
                        if (nn->kind == MIRKind::LABEL &&
                            static_cast<const MIRLabel*>(nn.get())->labelId ==
                                bz->targetLabel) {
                            targetAnchored = true;
                        }
                    }
                }
                EXPECT_TRUE(targetAnchored);
                foundObjectNullBranch = true;
            }
        }
    }
    EXPECT_TRUE(foundObjectNullBranch);
}

TEST(CodeConverterTest, DumpOutput)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "add",
                         "(II)I",
                         {
                             new VarInsnNode(jbytecode::core::Opcodes::ILOAD, 0),
                             new VarInsnNode(jbytecode::core::Opcodes::ILOAD, 1),
                             new InsnNode(jbytecode::core::Opcodes::IADD),
                             new InsnNode(jbytecode::core::Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto          dump   = CodeConverter::dumpMIR(blocks);

    EXPECT_FALSE(dump.empty());
    EXPECT_NE(dump.find("B0:"), std::string::npos);
    EXPECT_NE(dump.find("BINARY"), std::string::npos);
    EXPECT_NE(dump.find("RETURN_VALUE"), std::string::npos);
}

TEST(CodeConverterTest, ConstantInstructions)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "consts",
                         "()V",
                         {
                             new InsnNode(jbytecode::core::Opcodes::ICONST_0),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_1),
                             new InsnNode(jbytecode::core::Opcodes::LCONST_0),
                             new InsnNode(jbytecode::core::Opcodes::FCONST_0),
                             new InsnNode(jbytecode::core::Opcodes::DCONST_0),
                             new InsnNode(jbytecode::core::Opcodes::ACONST_NULL),
                             new InsnNode(jbytecode::core::Opcodes::POP2),   // clean up long
                             new InsnNode(jbytecode::core::Opcodes::POP2),   // clean up double
                             new InsnNode(jbytecode::core::Opcodes::POP),    // clean up float
                             new InsnNode(jbytecode::core::Opcodes::POP2),   // clean up null + iconst_0
                             new InsnNode(jbytecode::core::Opcodes::RETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    int constCount = 0;
    for (auto& n : nodes) {
        if (n->kind == MIRKind::CONSTANT) constCount++;
    }
    EXPECT_EQ(constCount, 6);
}

TEST(CodeConverterTest, StackShapeInstructionsArePreserved)
{
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                         "stackShape",
                         "()V",
                         {
                             new InsnNode(jbytecode::core::Opcodes::ICONST_0),
                             new InsnNode(jbytecode::core::Opcodes::POP),
                             new InsnNode(jbytecode::core::Opcodes::LCONST_0),
                             new InsnNode(jbytecode::core::Opcodes::POP2),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_1),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_2),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_3),
                             new InsnNode(jbytecode::core::Opcodes::DUP2_X1),
                             new InsnNode(jbytecode::core::Opcodes::POP2),
                             new InsnNode(jbytecode::core::Opcodes::POP2),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_1),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_2),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_3),
                             new InsnNode(jbytecode::core::Opcodes::ICONST_4),
                             new InsnNode(jbytecode::core::Opcodes::DUP2_X2),
                             new InsnNode(jbytecode::core::Opcodes::RETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
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
            auto* op   = ca::core::tag_cast::cast<MIRStackOp>(n.get());
            pop2IsWide = pop2IsWide || (!op->stackBeforeTop.empty() &&
                                        op->stackBeforeTop[0] == PrimitiveKind::LONG);
        }
        if (n->kind == MIRKind::STACK_DUP2_X1) {
            foundDup2X1   = true;
            auto* op      = ca::core::tag_cast::cast<MIRStackOp>(n.get());
            dup2X1HasPair = op->stackBeforeTop.size() >= 3 &&
                            op->stackBeforeTop[0] == PrimitiveKind::INT &&
                            op->stackBeforeTop[1] == PrimitiveKind::INT &&
                            op->stackBeforeTop[2] == PrimitiveKind::INT;
        }
        if (n->kind == MIRKind::STACK_DUP2_X2) {
            foundDup2X2   = true;
            auto* op      = ca::core::tag_cast::cast<MIRStackOp>(n.get());
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
        jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
        "create",
        "()Ljava/lang/Object;",
        {
            new TypeInsnNode(jbytecode::core::Opcodes::NEW, ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
            new InsnNode(jbytecode::core::Opcodes::DUP),
            new MethodInsnNode(jbytecode::core::Opcodes::INVOKESPECIAL,
                               ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                               ca::str::Utf8StringRef::from_cstr("<init>"),
                               ca::str::Utf8StringRef::from_cstr("()V"),
                               false),
            new InsnNode(jbytecode::core::Opcodes::ARETURN),
        });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);
    auto&         nodes  = blocks[0]->nodes;

    bool foundNew = false, foundDup = false, foundInvoke = false;
    for (auto& n : nodes) {
        switch (n->kind) {
        case MIRKind::NEW_OBJECT:
            EXPECT_EQ(ca::core::tag_cast::cast<MIRNewObject>(n.get())->typeDesc, "java/lang/Object");
            foundNew = true;
            break;
        case MIRKind::STACK_DUP: foundDup = true; break;
        case MIRKind::INVOKE:
            EXPECT_EQ(ca::core::tag_cast::cast<MIRInvoke>(n.get())->name, "<init>");
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
    // 批 3b 起转换走 DOM 读路径（树经 plain 序列化字节往返），夹具必须是
    // 合法字节码：tableswitch 的 labels 数 == max-min+1（旧树模型不序列化，
    // 非法夹具不再可用）。目标身份断言改为 DOM 合成语义：switch 的目标
    // Label* 与目标块内 MIRLabel 的 labelNode 同指针（LabelPool 同 pc 同身份）。
    auto* defaultLabel = new LabelNode(labelScratchArena().create<Label>());
    auto* caseLabel0   = new LabelNode(labelScratchArena().create<Label>());
    auto* caseLabel1   = new LabelNode(labelScratchArena().create<Label>());

    auto mn =
        makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC,
                   "sw",
                   "()V",
                   {
                       new TableSwitchInsnNode(0, 1, defaultLabel->label,
                                               {caseLabel0->label, caseLabel1->label}),
                       defaultLabel,
                       new InsnNode(jbytecode::core::Opcodes::RETURN),
                       caseLabel0,
                       new InsnNode(jbytecode::core::Opcodes::RETURN),
                       caseLabel1,
                       new InsnNode(jbytecode::core::Opcodes::RETURN),
                   });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto          blocks = test::convertMethodNode(cc, *mn, arena);

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
            auto* sw = ca::core::tag_cast::cast<MIRSwitch>(n.get());
            ASSERT_EQ(sw->cases.size(), 2u);
            // 目标身份：每个目标 Label* 必有含对应 MIRLabel 的块（键 = case 0..1）。
            auto blockHasLabel = [&](int l) {
                for (auto* b : blocks) {
                    for (auto& nn : b->nodes) {
                        if (nn->kind != MIRKind::LABEL) continue;
                        if (static_cast<const MIRLabel*>(nn.get())->labelId == l) return true;
                    }
                }
                return false;
            };
            EXPECT_TRUE(blockHasLabel(sw->defaultLabel));
            EXPECT_TRUE(blockHasLabel(sw->cases[0].targetLabel));
            EXPECT_TRUE(blockHasLabel(sw->cases[1].targetLabel));
            // 目标互异（default 与两个 case 各自独立块）。
            EXPECT_NE(sw->defaultLabel, sw->cases[0].targetLabel);
            EXPECT_NE(sw->cases[0].targetLabel, sw->cases[1].targetLabel);
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
    // MIRConstant 的 variant string 槽（Type 槽批 3a 起为 mj2x::Type 门面）
    using ConstVariant = decltype(MIRConstant::value);
    static_assert(std::is_same_v<ConstVariant,
                                 std::variant<int64_t, double, std::nullptr_t,
                                              ca::str::Utf8StringRef, mj2x::Type>>,
                  "MIRConstant::value string slot must hold Utf8StringRef (#698)");

    // ---- 2) 运行期：intern 后内容正确、且 ref 指向 arena 内副本（data() 不指向源 cstr）----
    auto mn = makeMethod(jbytecode::core::Opcodes::ACC_PUBLIC,
                         "getX",
                         "()I",
                         {
                             new VarInsnNode(jbytecode::core::Opcodes::ALOAD, 0),
                             new FieldInsnNode(jbytecode::core::Opcodes::GETFIELD,
                                               ca::str::Utf8StringRef::from_cstr("Foo"),
                                               ca::str::Utf8StringRef::from_cstr("x"),
                                               ca::str::Utf8StringRef::from_cstr("I")),
                             new InsnNode(jbytecode::core::Opcodes::IRETURN),
                         });

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = test::convertMethodNode(cc, *mn, arena);

    MIRFieldGet* fg = nullptr;
    for (auto& n : blocks[0]->nodes) {
        if (n->kind == MIRKind::FIELD_GET) {
            fg = ca::core::tag_cast::cast<MIRFieldGet>(n.get());
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
