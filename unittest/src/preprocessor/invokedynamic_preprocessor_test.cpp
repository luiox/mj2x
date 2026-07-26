// InvokeDynamicPreprocessor 单元测试（R6.1 阶段1）。
//
// 验证 StringConcatFactory 站点被展开成 StringBuilder 字节码（indy marker 消失），
// 且非 concat 的 indy（lambda / 自定义 bootstrap）保持 fail-closed 原样保留。
// 对齐 regc_invokedynamic_strategy.md §Tests：SSA/RegIR test showing the indy
// marker is gone before lowering + negative test 证明不支持形态仍 fail-closed。
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/core/any.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// 统计一个方法里指定 opcode / 节点类型的指令数量。
int countInsnType(MethodNode* m, int insnType)
{
    int n = 0;
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() == insnType) ++n;
    }
    return n;
}

// 统计对某 owner.name 的 MethodInsnNode 调用次数。
int countMethodCall(MethodNode* m, const std::string& owner, const std::string& name)
{
    int n = 0;
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
        auto* mi = static_cast<MethodInsnNode*>(insn);
        if (mi->owner.to_std_string() == owner && mi->name.to_std_string() == name) ++n;
    }
    return n;
}

// 返回方法里对 owner.name 的首个 MethodInsnNode 的 opcode（找不到返回 -1）。
int firstCallOpcode(MethodNode* m, const std::string& owner, const std::string& name)
{
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
        auto* mi = static_cast<MethodInsnNode*>(insn);
        if (mi->owner.to_std_string() == owner && mi->name.to_std_string() == name)
            return mi->getOpcode();
    }
    return -1;
}

// 返回合成类里第一个非 <init> 方法（即 SAM 转调方法）。
MethodNode* synthSamMethod(CompilerIndex& index, const std::string& synthName)
{
    auto* cls = index.getClass(synthName);
    if (!cls || !cls->classNode) return nullptr;
    for (auto* m : cls->classNode->methods) {
        if (m->name.to_std_string() != "<init>") return m;
    }
    return nullptr;
}

// 在 index 里造一个类，含一个方法，方法体是给定指令。返回方法指针（归 index 所有）。
MethodNode* addMethodToIndex(CompilerIndex& index, const std::string& className,
                             const std::string& methodName, const std::string& methodDesc,
                             const std::vector<AbstractInsnNode*>& insns)
{
    auto& arena = index.arena();
    auto  cn    = std::make_unique<ClassNode>();
    cn->name      = arena.intern(className.c_str());
    cn->version   = Opcodes::V1_8;
    cn->access    = Opcodes::ACC_PUBLIC;
    cn->superName = arena.intern("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto*                               mn = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                              arena.intern(methodName.c_str()),
                              arena.intern(methodDesc.c_str()), arena.intern(""), noExcepts);
    mn->maxStack  = 10;
    mn->maxLocals = 4;
    for (auto* insn : insns) mn->instructions.add(insn);
    cn->methods.push_back(mn);

    CompilerClass cls;
    cls.classNode = std::move(cn);
    index.addClass(className, std::move(cls));
    return index.getClass(className)->classNode->methods.back();
}

Handle stringConcatBsm(CompilerIndex& index, const char* name)
{
    auto& arena = index.arena();
    return Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/StringConcatFactory"),
                  arena.intern(name),
                  arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;Ljava/lang/String;"
                               "[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;"));
}

}   // namespace

// makeConcatWithConstants 展开：indy marker 消失，出现 new StringBuilder + append + toString。
TEST(InvokeDynamicPreprocessorTest, ExpandsMakeConcatWithConstants)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    // recipe "a=,s="：两个动态参数（int, String），中间/前缀是字面量。
    std::string               recipe = "a=\x01,s=\x01";
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(recipe);

    auto* indy = new InvokeDynamicInsnNode(arena.intern("makeConcatWithConstants"),
                                           arena.intern("(ILjava/lang/String;)Ljava/lang/String;"),
                                           stringConcatBsm(index, "makeConcatWithConstants"),
                                           bsmArgs);
    MethodNode* m =
        addMethodToIndex(index, "Concat", "join", "(ILjava/lang/String;)Ljava/lang/String;",
                         {new VarInsnNode(Opcodes::ILOAD, 0), new VarInsnNode(Opcodes::ALOAD, 1),
                          indy, new InsnNode(Opcodes::ARETURN)});

    InvokeDynamicPreprocessor().process(index);

    // indy marker 应消失。
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    // 应出现 StringBuilder 构造 + toString。
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "<init>"), 1);
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "toString"), 1);
    // 两个动态参数 → 至少两次 append；加两段字面量("a=" 和 ",s=") 共 4 次 append。
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "append"), 4);
    // maxLocals 应增大以容纳暂存槽（原 4，int+ref 各占 1 → >= 6）。
    EXPECT_GE(m->maxLocals, 6);
}

// makeConcat（无 recipe 常量）展开：全动态参数顺序拼接。
TEST(InvokeDynamicPreprocessorTest, ExpandsMakeConcatWithoutConstants)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    auto* indy = new InvokeDynamicInsnNode(
        arena.intern("makeConcat"),
        arena.intern("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
        stringConcatBsm(index, "makeConcat"), std::vector<ca::core::Any>{});
    MethodNode* m = addMethodToIndex(
        index, "Concat2", "cat", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        {new VarInsnNode(Opcodes::ALOAD, 0), new VarInsnNode(Opcodes::ALOAD, 1), indy,
         new InsnNode(Opcodes::ARETURN)});

    InvokeDynamicPreprocessor().process(index);

    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "<init>"), 1);
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "toString"), 1);
    // 两个动态 String 参数 → 两次 append，无字面量。
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "append"), 2);
}

Handle lambdaMetafactoryBsm(CompilerIndex& index)
{
    auto& arena = index.arena();
    return Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/LambdaMetafactory"),
                  arena.intern("metafactory"),
                  arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;"
                               "Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)"
                               "Ljava/lang/invoke/CallSite;"));
}

// 无状态 lambda（方法引用）展开：indy 消失、换成 new 合成类，合成类进 index，私有目标提权。
TEST(InvokeDynamicPreprocessorTest, ExpandsStatelessLambda)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    // bsmArgs: [0]=samMethodType (I)I, [1]=implHandle Lam.base(I)I static, [2]=instantiated (I)I。
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(Handle(Opcodes::H_INVOKESTATIC, arena.intern("Lam"), arena.intern("base"),
                                arena.intern("(I)I")));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(I)I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("applyAsInt"),
                                           arena.intern("()Ljava/util/function/IntUnaryOperator;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "Lam", "make",
                                     "()Ljava/util/function/IntUnaryOperator;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});
    // 私有目标方法 base，展开后应被提权。
    std::vector<ca::str::Utf8StringRef> noExc;
    auto* base = new MethodNode(Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC, arena.intern("base"),
                                arena.intern("(I)I"), arena.intern(""), noExc);
    base->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    base->instructions.add(new InsnNode(Opcodes::IRETURN));
    base->maxStack  = 1;
    base->maxLocals = 1;
    index.getClass("Lam")->classNode->methods.push_back(base);

    InvokeDynamicPreprocessor().process(index);

    // indy 消失，换成 NEW + INVOKESPECIAL <init>。
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::TYPE_INSN), 1);   // NEW
    // 合成类入 index。
    EXPECT_NE(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
    // 私有目标方法已提权（去 private、置 public）。
    EXPECT_FALSE(base->access & Opcodes::ACC_PRIVATE);
    EXPECT_TRUE(base->access & Opcodes::ACC_PUBLIC);
}

// 捕获 lambda 展开：indy desc 带捕获参数 (I)，impl 布局 = captured ++ sam = (I I)I。
// indy 消失、换成 xSTORE 暂存 + new + xLOAD + invokespecial <init>(I)V；合成类带捕获字段。
TEST(InvokeDynamicPreprocessorTest, ExpandsCapturingLambda)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    // bsmArgs: [0]=samMethodType (I)I, [1]=impl Cap.lambda$make$0(I I)I static（捕获在前）,
    //          [2]=instantiated (I)I。
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(Handle(Opcodes::H_INVOKESTATIC, arena.intern("Cap"),
                                arena.intern("lambda$make$0"), arena.intern("(II)I")));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(I)I")));

    // 捕获 lambda：indy desc 带一个捕获参数 (I)，返回 functional interface。
    auto* indy =
        new InvokeDynamicInsnNode(arena.intern("applyAsInt"),
                                  arena.intern("(I)Ljava/util/function/IntUnaryOperator;"),
                                  lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "Cap", "make",
                                     "(I)Ljava/util/function/IntUnaryOperator;",
                                     {new VarInsnNode(Opcodes::ILOAD, 0), indy,
                                      new InsnNode(Opcodes::ARETURN)});
    // lambda$make$0 是 private static（javac 合成），展开后应被提权。
    std::vector<ca::str::Utf8StringRef> noExc;
    auto* impl = new MethodNode(Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC | Opcodes::ACC_SYNTHETIC,
                                arena.intern("lambda$make$0"), arena.intern("(II)I"),
                                arena.intern(""), noExc);
    impl->instructions.add(new InsnNode(Opcodes::ICONST_0));
    impl->instructions.add(new InsnNode(Opcodes::IRETURN));
    impl->maxStack  = 1;
    impl->maxLocals = 2;
    index.getClass("Cap")->classNode->methods.push_back(impl);

    InvokeDynamicPreprocessor().process(index);

    // indy 消失、换成 new。合成类入 index，含 1 个捕获字段 cap0。
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::TYPE_INSN), 1);   // NEW
    auto* synth = index.getClass("mj2x/lambda/Lambda$0");
    ASSERT_NE(synth, nullptr);
    EXPECT_EQ(synth->classNode->fields.size(), 1u);
    EXPECT_EQ(synth->classNode->fields[0]->name.to_std_string(), "cap0");
    // 私有目标提权。
    EXPECT_FALSE(impl->access & Opcodes::ACC_PRIVATE);
    EXPECT_TRUE(impl->access & Opcodes::ACC_PUBLIC);
}

// 无捕获实例方法引用（unbound，如 Rec::val）：receiver = SAM 首参。展开后合成类 SAM
// 体应含 INVOKEVIRTUAL Rec.val，站点换成 new 合成类。
TEST(InvokeDynamicPreprocessorTest, ExpandsUnboundInstanceRef)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    // samMethodType (Lp/Rec;)I：receiver 就是唯一 SAM 参数；impl 虚调用 Rec.val()I。
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(Lp/Rec;)I")));
    bsmArgs.emplace_back(Handle(Opcodes::H_INVOKEVIRTUAL, arena.intern("p/Rec"),
                                arena.intern("val"), arena.intern("()I")));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(Lp/Rec;)I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("apply"),
                                           arena.intern("()Lp/Fn;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "UB", "make", "()Lp/Fn;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    InvokeDynamicPreprocessor().process(index);

    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::TYPE_INSN), 1);   // NEW 合成类
    auto* synth = index.getClass("mj2x/lambda/Lambda$0");
    ASSERT_NE(synth, nullptr);
    EXPECT_EQ(synth->classNode->fields.size(), 0u);   // unbound：无捕获字段
    MethodNode* sam = synthSamMethod(index, "mj2x/lambda/Lambda$0");
    ASSERT_NE(sam, nullptr);
    EXPECT_EQ(firstCallOpcode(sam, "p/Rec", "val"), Opcodes::INVOKEVIRTUAL);
}

// 捕获实例方法引用（bound，如 rec::val）：receiver 被捕获。合成类含 1 个 cap0 字段，
// SAM 体先 GETFIELD cap0 再 INVOKEVIRTUAL Rec.val。
TEST(InvokeDynamicPreprocessorTest, ExpandsBoundInstanceRef)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    // samMethodType ()I：无 SAM 参数；receiver 来自 indy 捕获参数 (Lp/Rec;)。
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("()I")));
    bsmArgs.emplace_back(Handle(Opcodes::H_INVOKEVIRTUAL, arena.intern("p/Rec"),
                                arena.intern("val"), arena.intern("()I")));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("()I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("get"),
                                           arena.intern("(Lp/Rec;)Lp/Sup;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "BD", "make", "(Lp/Rec;)Lp/Sup;",
                                     {new VarInsnNode(Opcodes::ALOAD, 0), indy,
                                      new InsnNode(Opcodes::ARETURN)});

    InvokeDynamicPreprocessor().process(index);

    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    auto* synth = index.getClass("mj2x/lambda/Lambda$0");
    ASSERT_NE(synth, nullptr);
    ASSERT_EQ(synth->classNode->fields.size(), 1u);
    EXPECT_EQ(synth->classNode->fields[0]->name.to_std_string(), "cap0");
    EXPECT_EQ(synth->classNode->fields[0]->desc.to_std_string(), "Lp/Rec;");
    MethodNode* sam = synthSamMethod(index, "mj2x/lambda/Lambda$0");
    ASSERT_NE(sam, nullptr);
    EXPECT_EQ(countInsnType(sam, AbstractInsnNode::FIELD_INSN), 1);   // GETFIELD cap0
    EXPECT_EQ(firstCallOpcode(sam, "p/Rec", "val"), Opcodes::INVOKEVIRTUAL);
}

// 接口方法引用（H_INVOKEINTERFACE，如 List::size）：SAM 体应发射 INVOKEINTERFACE。
TEST(InvokeDynamicPreprocessorTest, ExpandsInterfaceMethodRef)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(Ljava/util/List;)I")));
    bsmArgs.emplace_back(Handle(Opcodes::H_INVOKEINTERFACE, arena.intern("java/util/List"),
                                arena.intern("size"), arena.intern("()I")));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(Ljava/util/List;)I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("apply"), arena.intern("()Lp/Fn;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "IF", "make", "()Lp/Fn;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    InvokeDynamicPreprocessor().process(index);

    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    MethodNode* sam = synthSamMethod(index, "mj2x/lambda/Lambda$0");
    ASSERT_NE(sam, nullptr);
    EXPECT_EQ(firstCallOpcode(sam, "java/util/List", "size"), Opcodes::INVOKEINTERFACE);
}

// 构造器引用（H_NEWINVOKESPECIAL，如 Rec::new）：SAM 体应为 NEW/DUP/args/INVOKESPECIAL
// <init>/ARETURN；包私有构造器展开后提权为 public。
TEST(InvokeDynamicPreprocessorTest, ExpandsConstructorRef)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    // samMethodType (I)Lp/Rec;：一个 int 参 → 造出 Rec；impl = Rec.<init>(I)V。
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(I)Lp/Rec;")));
    bsmArgs.emplace_back(Handle(Opcodes::H_NEWINVOKESPECIAL, arena.intern("p/Rec"),
                                arena.intern("<init>"), arena.intern("(I)V")));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(I)Lp/Rec;")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("make"), arena.intern("()Lp/Factory;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "p/Rec", "factory", "()Lp/Factory;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});
    // 包私有构造器 Rec.<init>(I)V：展开后应被提权。
    std::vector<ca::str::Utf8StringRef> noExc;
    auto* ctor = new MethodNode(0 /* package-private */, arena.intern("<init>"),
                                arena.intern("(I)V"), arena.intern(""), noExc);
    ctor->instructions.add(new VarInsnNode(Opcodes::ALOAD, 0));
    ctor->instructions.add(new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                              arena.intern("java/lang/Object"),
                                              arena.intern("<init>"), arena.intern("()V"), false));
    ctor->instructions.add(new InsnNode(Opcodes::RETURN));
    ctor->maxStack  = 1;
    ctor->maxLocals = 2;
    index.getClass("p/Rec")->classNode->methods.push_back(ctor);

    InvokeDynamicPreprocessor().process(index);

    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    MethodNode* sam = synthSamMethod(index, "mj2x/lambda/Lambda$0");
    ASSERT_NE(sam, nullptr);
    EXPECT_EQ(countInsnType(sam, AbstractInsnNode::TYPE_INSN), 1);          // NEW p/Rec
    EXPECT_EQ(firstCallOpcode(sam, "p/Rec", "<init>"), Opcodes::INVOKESPECIAL);
    EXPECT_EQ(countInsnType(sam, AbstractInsnNode::INSN), 2);               // DUP + ARETURN
    // 包私有构造器提权为 public。
    EXPECT_TRUE(ctor->access & Opcodes::ACC_PUBLIC);
}

// negative：不支持的 handle 形态（H_GETFIELD 字段句柄）保持 fail-closed，indy 保留、无合成类。
TEST(InvokeDynamicPreprocessorTest, LeavesUnsupportedHandleUnexpanded)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(Lp/Rec;)I")));
    bsmArgs.emplace_back(Handle(Opcodes::H_GETFIELD, arena.intern("p/Rec"),
                                arena.intern("field"), arena.intern("I")));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(Lp/Rec;)I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("apply"), arena.intern("()Lp/Fn;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "GF", "make", "()Lp/Fn;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    InvokeDynamicPreprocessor().process(index);

    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 1);
    EXPECT_EQ(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
}
