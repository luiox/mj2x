// InitClinitMoverPreprocessor 单元测试（搬运法，regc_conversion_decisions.md §7.1）。
//
// 验证：
// - <init>：super 调用留在骨架，body 搬进静态 __regc_init_<hash>，
//   骨架尾部补 aload_0 + 参数 load + invokestatic + return；
// - <clinit>：整个体搬进 __regc_clinit_<hash>，原 <clinit> 只留 invokestatic + return；
// - v1 保守边界：带异常表/切点前有 label 时保持原字节码不动。
#include "garena/arena.hpp"
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/preprocessor/init_clinit_mover_preprocessor.hpp"
#include "jbytecode/jbytecode.hpp"

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

namespace {

MethodNode* findMethod(CompilerClass& cls, const std::string& name)
{
    for (auto* m : cls.classNode->methods) {
        if (m->name.to_std_string() == name) return m;
    }
    return nullptr;
}

MethodNode* findMethodPrefix(CompilerClass& cls, const std::string& prefix)
{
    for (auto* m : cls.classNode->methods) {
        if (m->name.to_std_string().rfind(prefix, 0) == 0) return m;
    }
    return nullptr;
}

int countInsnType(MethodNode* m, int insnType)
{
    int n = 0;
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() == insnType) ++n;
    }
    return n;
}

int countCall(MethodNode* m, const std::string& owner, const std::string& name)
{
    int n = 0;
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
        auto* mi = static_cast<MethodInsnNode*>(insn);
        if (mi->owner.to_std_string() == owner && mi->name.to_std_string() == name) ++n;
    }
    return n;
}

bool hasOpcode(MethodNode* m, int opcode)
{
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getOpcode() == opcode) return true;
    }
    return false;
}

// 构造一个类：superName=java/lang/Object，给定的方法列表。
void buildClass(CompilerIndex& index, const std::string& className,
                std::vector<MethodNode*> methods)
{
    auto& arena = index.arena();
    auto  cn    = std::make_unique<ClassNode>();
    cn->name      = arena.intern(className.c_str());
    cn->version   = Opcodes::V1_8;
    cn->access    = Opcodes::ACC_PUBLIC;
    cn->superName = arena.intern("java/lang/Object");
    cn->methods   = std::move(methods);

    CompilerClass cls;
    cls.classNode = std::move(cn);
    for (auto* m : cls.classNode->methods) {
        CompilerMethod cm;
        cm.methodNode = m;
        cls.methods.push_back(std::move(cm));
    }
    index.addClass(className, std::move(cls));
}

// <init>：super 调用 + afterSuper 指令体。
MethodNode* makeInit(CompilerIndex& index, const std::string& desc,
                     const std::vector<AbstractInsnNode*>& afterSuper)
{
    auto& arena = index.arena();
    std::vector<ca::str::Utf8StringRef> noExc;
    auto* mn = new MethodNode(Opcodes::ACC_PUBLIC, arena.intern("<init>"),
                              arena.intern(desc.c_str()), arena.intern(""), noExc);
    mn->maxStack  = 10;
    mn->maxLocals = 8;
    mn->instructions.add(new VarInsnNode(Opcodes::ALOAD, 0));
    mn->instructions.add(new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                            arena.intern("java/lang/Object"),
                                            arena.intern("<init>"), arena.intern("()V"), false));
    for (auto* insn : afterSuper) mn->instructions.add(insn);
    return mn;
}

}   // namespace

// <init> 基本搬运：super 之后 body 进入生成方法，骨架只留 super + 转发调用。
TEST(InitClinitMoverTest, MovesInitBodyIntoStaticMirror)
{
    CompilerIndex index;

    auto* init = makeInit(index, "(I)V",
                          {new VarInsnNode(Opcodes::ILOAD, 1), new InsnNode(Opcodes::ICONST_1),
                           new InsnNode(Opcodes::IADD), new InsnNode(Opcodes::POP),
                           new InsnNode(Opcodes::RETURN)});
    buildClass(index, "p/Foo", {init});
    InitClinitMoverPreprocessor().process(index);

    CompilerClass& cls = *index.getClass("p/Foo");
    auto*          skeleton = findMethod(cls, "<init>");
    ASSERT_NE(skeleton, nullptr);
    // 骨架：aload_0 + super + aload_0 + 参数 load(ILOAD 1) + invokestatic + return = 6 条。
    EXPECT_EQ(skeleton->instructions.size(), 6);
    EXPECT_EQ(countCall(skeleton, "java/lang/Object", "<init>"), 1);
    EXPECT_TRUE(hasOpcode(skeleton, Opcodes::INVOKESTATIC));
    EXPECT_TRUE(hasOpcode(skeleton, Opcodes::ALOAD));
    EXPECT_FALSE(hasOpcode(skeleton, Opcodes::IADD));   // 计算体已搬走

    // 生成方法：__regc_init_*，static，desc=(Lp/Foo;I)V，含 IADD。
    auto* gen = findMethodPrefix(cls, "__regc_init_");
    ASSERT_NE(gen, nullptr);
    EXPECT_TRUE(gen->access & Opcodes::ACC_STATIC);
    EXPECT_EQ(gen->desc.to_std_string(), "(Lp/Foo;I)V");
    EXPECT_TRUE(hasOpcode(gen, Opcodes::IADD));
    EXPECT_TRUE(hasOpcode(gen, Opcodes::POP));
    EXPECT_TRUE(hasOpcode(gen, Opcodes::ICONST_1));

    // 生成方法已挂到 CompilerMethod 列表（供 MarkAll 标记）。
    bool inCompilerMethods = false;
    for (const auto& cm : cls.methods) {
        if (cm.methodNode == gen) inCompilerMethods = true;
    }
    EXPECT_TRUE(inCompilerMethods);
}

// <init> 带多参（含 long 占双槽）：骨架参数 load 槽位正确。
TEST(InitClinitMoverTest, InitArgsSlotLayout)
{
    CompilerIndex index;

    auto* init = makeInit(index, "(JLjava/lang/String;D)V",
                          {new InsnNode(Opcodes::RETURN)});
    buildClass(index, "p/Foo", {init});
    InitClinitMoverPreprocessor().process(index);

    CompilerClass& cls   = *index.getClass("p/Foo");
    auto*          skel  = findMethod(cls, "<init>");
    ASSERT_NE(skel, nullptr);
    // 精确值：aload_0, invokespecial super, aload_0, lload 1, aload 3, dload 4,
    // invokestatic, return = 8 条。
    EXPECT_EQ(skel->instructions.size(), 8);

    auto* gen = findMethodPrefix(cls, "__regc_init_");
    ASSERT_NE(gen, nullptr);
    EXPECT_EQ(gen->desc.to_std_string(), "(Lp/Foo;JLjava/lang/String;D)V");
    // 原 body 只有 return。
    EXPECT_EQ(countInsnType(gen, AbstractInsnNode::INSN), 1);
}

// <init> 骨架搬运后必须仍是合法的 <init>：唯一方法调用是 super 与转发调用。
TEST(InitClinitMoverTest, InitSkeletonCallsMirror)
{
    CompilerIndex index;
    auto* init = makeInit(index, "()V", {new InsnNode(Opcodes::RETURN)});
    buildClass(index, "Foo", {init});
    InitClinitMoverPreprocessor().process(index);

    CompilerClass& cls  = *index.getClass("Foo");
    auto*          skel = findMethod(cls, "<init>");
    ASSERT_NE(skel, nullptr);
    EXPECT_EQ(skel->instructions.size(), 5);   // aload0+super+aload0+invokestatic+return
    EXPECT_EQ(countInsnType(skel, AbstractInsnNode::METHOD_INSN), 2);
    EXPECT_TRUE(hasOpcode(skel, Opcodes::RETURN));
}

// 切点前有孤立 label（javac 行号锚点场景）→ 允许搬运，生成方法照常出现。
TEST(InitClinitMoverTest, MovesInitWithBareLabelInPrefix)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto* mn = new MethodNode(Opcodes::ACC_PUBLIC, arena.intern("<init>"),
                              arena.intern("()V"), arena.intern(""),
                              std::vector<ca::str::Utf8StringRef>{});
    mn->maxStack  = 4;
    mn->maxLocals = 1;
    mn->instructions.add(new LabelNode(labelScratchArena().create<Label>()));
    mn->instructions.add(new VarInsnNode(Opcodes::ALOAD, 0));
    mn->instructions.add(new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                            arena.intern("java/lang/Object"),
                                            arena.intern("<init>"), arena.intern("()V"), false));
    mn->instructions.add(new InsnNode(Opcodes::RETURN));
    buildClass(index, "Foo", {mn});

    InitClinitMoverPreprocessor().process(index);

    CompilerClass& cls = *index.getClass("Foo");
    EXPECT_EQ(cls.classNode->methods.size(), 2);
    EXPECT_EQ(mn->instructions.size(), 6);   // label+aload0+super+aload0+invokestatic+return
    EXPECT_TRUE(hasOpcode(mn, Opcodes::ALOAD));
    EXPECT_TRUE(hasOpcode(mn, Opcodes::INVOKESTATIC));
    EXPECT_TRUE(hasOpcode(mn, Opcodes::RETURN));
}

// 切点前有 FRAME → 不搬运（骨架帧状态复杂，v1 不处理）。
TEST(InitClinitMoverTest, SkipsInitWithFrameInPrefix)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto* mn = new MethodNode(Opcodes::ACC_PUBLIC, arena.intern("<init>"),
                              arena.intern("()V"), arena.intern(""),
                              std::vector<ca::str::Utf8StringRef>{});
    mn->maxStack  = 4;
    mn->maxLocals = 1;
    mn->instructions.add(new FrameNode(Opcodes::F_NEW,
                                       std::vector<ca::core::Any>{arena.intern("Foo")},
                                       std::vector<ca::core::Any>{}));
    mn->instructions.add(new VarInsnNode(Opcodes::ALOAD, 0));
    mn->instructions.add(new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                            arena.intern("java/lang/Object"),
                                            arena.intern("<init>"), arena.intern("()V"), false));
    mn->instructions.add(new InsnNode(Opcodes::RETURN));
    buildClass(index, "Foo", {mn});

    InitClinitMoverPreprocessor().process(index);

    CompilerClass& cls = *index.getClass("Foo");
    EXPECT_EQ(cls.classNode->methods.size(), 1);
    EXPECT_EQ(mn->instructions.size(), 4);
    EXPECT_TRUE(hasOpcode(mn, Opcodes::INVOKESPECIAL));
    EXPECT_TRUE(hasOpcode(mn, Opcodes::RETURN));
}

// body 分支向后引用骨架段 label → 跨切点引用，不搬运。
TEST(InitClinitMoverTest, SkipsInitWithCrossSegmentLabelRef)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto* headLabel = new LabelNode(labelScratchArena().create<Label>());
    auto* target = new LabelNode(labelScratchArena().create<Label>());
    auto* mn = new MethodNode(Opcodes::ACC_PUBLIC, arena.intern("<init>"),
                              arena.intern("()V"), arena.intern(""),
                              std::vector<ca::str::Utf8StringRef>{});
    mn->maxStack  = 4;
    mn->maxLocals = 1;
    mn->instructions.add(headLabel);
    mn->instructions.add(new VarInsnNode(Opcodes::ALOAD, 0));
    mn->instructions.add(new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                            arena.intern("java/lang/Object"),
                                            arena.intern("<init>"), arena.intern("()V"), false));
    mn->instructions.add(new JumpInsnNode(Opcodes::GOTO, headLabel->label));
    mn->instructions.add(target);
    mn->instructions.add(new InsnNode(Opcodes::RETURN));
    buildClass(index, "Foo", {mn});

    InitClinitMoverPreprocessor().process(index);

    CompilerClass& cls = *index.getClass("Foo");
    EXPECT_EQ(cls.classNode->methods.size(), 1);
    EXPECT_EQ(mn->instructions.size(), 6);   // 原样保留：label+aload0+super+goto+target+return
    EXPECT_TRUE(hasOpcode(mn, Opcodes::GOTO));
}

// 带异常表的构造器不搬运。
TEST(InitClinitMoverTest, SkipsInitWithTryCatch)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto* mn = makeInit(index, "()V", {new InsnNode(Opcodes::RETURN)});
    auto* tc = mn->create<TryCatchBlockNode>(new Label(), new Label(), new Label(),
                                     arena.intern("java/lang/Exception"));
    mn->tryCatchBlocks.push_back(tc);
    buildClass(index, "Foo", {mn});

    InitClinitMoverPreprocessor().process(index);

    CompilerClass& cls = *index.getClass("Foo");
    EXPECT_EQ(cls.classNode->methods.size(), 1);
    EXPECT_EQ(mn->instructions.size(), 3);   // aload0+super+return
}

// <clinit> 整段搬走，骨架只留 invokestatic + return。
TEST(InitClinitMoverTest, MovesClinitBody)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto* mn = new MethodNode(Opcodes::ACC_STATIC, arena.intern("<clinit>"),
                              arena.intern("()V"), arena.intern(""),
                              std::vector<ca::str::Utf8StringRef>{});
    mn->maxStack   = 1;
    mn->maxLocals  = 0;
    mn->instructions.add(new LdcInsnNode(0));
    mn->instructions.add(new InsnNode(Opcodes::POP));
    mn->instructions.add(new InsnNode(Opcodes::RETURN));
    buildClass(index, "Foo", {mn});

    InitClinitMoverPreprocessor().process(index);

    CompilerClass& cls = *index.getClass("Foo");
    auto*          clinit = findMethod(cls, "<clinit>");
    ASSERT_NE(clinit, nullptr);
    EXPECT_EQ(clinit->instructions.size(), 2);   // invokestatic + return
    EXPECT_TRUE(hasOpcode(clinit, Opcodes::INVOKESTATIC));
    EXPECT_TRUE(hasOpcode(clinit, Opcodes::RETURN));
    EXPECT_FALSE(hasOpcode(clinit, Opcodes::LDC));   // 体已搬走

    auto* gen = findMethodPrefix(cls, "__regc_clinit_");
    ASSERT_NE(gen, nullptr);
    EXPECT_TRUE(gen->access & Opcodes::ACC_STATIC);
    EXPECT_EQ(gen->desc.to_std_string(), "()V");
    EXPECT_TRUE(hasOpcode(gen, Opcodes::LDC));
    EXPECT_TRUE(hasOpcode(gen, Opcodes::POP));
}

// 无 <init>/<clinit> 的类完全不受影响。
TEST(InitClinitMoverTest, OrdinaryClassUntouched)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::str::Utf8StringRef> noExc;
    auto* mn = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                              arena.intern("run"), arena.intern("()V"),
                              arena.intern(""), noExc);
    mn->maxStack  = 1;
    mn->maxLocals = 0;
    mn->instructions.add(new InsnNode(Opcodes::RETURN));
    buildClass(index, "Foo", {mn});

    InitClinitMoverPreprocessor().process(index);

    CompilerClass& cls = *index.getClass("Foo");
    EXPECT_EQ(cls.classNode->methods.size(), 1);
    EXPECT_EQ(mn->instructions.size(), 1);
}