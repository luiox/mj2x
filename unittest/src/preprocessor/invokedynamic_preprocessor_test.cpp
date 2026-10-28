// InvokeDynamicPreprocessor 单元测试（R6.1 阶段1）。
//
// 验证 StringConcatFactory 站点被展开成 StringBuilder 字节码（indy marker 消失），
// 且非 concat 的 indy（lambda / 自定义 bootstrap）保持 fail-closed 原样保留。
// 对齐 regc_invokedynamic_strategy.md §Tests：SSA/RegIR test showing the indy
// marker is gone before lowering + negative test 证明不支持形态仍 fail-closed。
#include <gmock/gmock.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/preprocessor/invokedynamic_preprocessor.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/core/any.hpp"
#include "test_helper.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// 树夹具暂存（键 = 类名，重建即替换）：批 6d 后 CompilerClass 无树槽，DOM 由
// classFromTree 一次性落定；shelf 让夹具树与回投影存活到测试结束（指针有效面）。
std::map<std::string, std::vector<std::unique_ptr<ClassNode>>>& treeShelf()
{
    static std::map<std::string, std::vector<std::unique_ptr<ClassNode>>> shelf;
    return shelf;
}

// 树夹具落 DOM + 入 index（同名替换）：建模完成后的唯一通道。
void addClassFromTree(CompilerIndex& index, const std::string& className)
{
    auto& shelf = treeShelf();
    index.addClass(className,
                   test::classFromTree(className, *shelf[className].back(), index.arena()));
}

// 按类名+方法名现查树方法（DOM 权威改写后经回投影现查，指针随投影存活）。
MethodNode* findMethodIn(CompilerIndex& index, const std::string& className,
                         const std::string& methodName)
{
    auto* cls = index.getClass(className);
    if (cls == nullptr) return nullptr;
    auto tree = test::treeFromDom(*cls, index.arena());
    if (tree == nullptr) return nullptr;
    // append-only projection shelf: earlier projections stay alive, so method
    // pointers taken from previous findMethodIn calls of the same class stay valid.
    auto& projections = treeShelf()[className];
    projections.push_back(std::move(tree));
    return test::findMethodNode(projections.back().get(), methodName);
}

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
    if (cls == nullptr) return nullptr;
    for (auto& cm : cls->methods) {
        if (cm.name != "<init>") {
            auto tree = test::treeFromDom(*cls, index.arena());
            if (tree == nullptr) return nullptr;
            // append-only projection shelf（同类既有投影保持存活）。
            auto& projections = treeShelf()[synthName];
            projections.push_back(std::move(tree));
            for (auto* m : projections.back()->methods) {
                if (m->name.to_std_string() != "<init>") return m;
            }
            return nullptr;
        }
    }
    return nullptr;
}

// 在 index 里造一个类，含一个方法 + 可选追加方法，方法体是给定指令。
// 返回方法指针（树侧，归 shelf 所有；DOM 快照随 classFromTree 一次性生成）。
MethodNode* addMethodToIndex(CompilerIndex& index, const std::string& className,
                             const std::string& methodName, const std::string& methodDesc,
                             const std::vector<AbstractInsnNode*>& insns,
                             std::vector<MethodNode*> extraMethods = {})
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
    for (auto* extra : extraMethods) cn->methods.push_back(extra);
    MethodNode* raw = mn;

    treeShelf()[className].push_back(std::move(cn));
    addClassFromTree(index, className);
    return raw;
}

jbytecode::core::Handle stringConcatBsm(CompilerIndex& index, const char* name)
{
    auto& arena = index.arena();
    return jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/StringConcatFactory"),
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
    // 载荷必须 intern（Utf8StringRef）：旧栈 writer 对 std::string bsm 参数
    // 不识别、落 CP 0，DOM 世界经树→字节→readClass 同步后即成坏索引。
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(arena.intern("a=\x01,s=\x01"));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("makeConcatWithConstants"),
                                           arena.intern("(ILjava/lang/String;)Ljava/lang/String;"),
                                           stringConcatBsm(index, "makeConcatWithConstants"),
                                           bsmArgs);
    MethodNode* m =
        addMethodToIndex(index, "Concat", "join", "(ILjava/lang/String;)Ljava/lang/String;",
                         {new VarInsnNode(Opcodes::ILOAD, 0), new VarInsnNode(Opcodes::ALOAD, 1),
                          indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Concat", "join");


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

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Concat2", "cat");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "<init>"), 1);
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "toString"), 1);
    // 两个动态 String 参数 → 两次 append，无字面量。
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "append"), 2);
}

jbytecode::core::Handle lambdaMetafactoryBsm(CompilerIndex& index)
{
    auto& arena = index.arena();
    return jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/LambdaMetafactory"),
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
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("Lam"), arena.intern("base"),
                                arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("applyAsInt"),
                                           arena.intern("()Ljava/util/function/IntUnaryOperator;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    // 私有目标方法 base，展开后应被提权（随夹具树一次落 DOM）。
    std::vector<ca::str::Utf8StringRef> noExc;
    auto* base = new MethodNode(Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC, arena.intern("base"),
                                arena.intern("(I)I"), arena.intern(""), noExc);
    base->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    base->instructions.add(new InsnNode(Opcodes::IRETURN));
    base->maxStack  = 1;
    base->maxLocals = 1;
    MethodNode* m = addMethodToIndex(index, "Lam", "make",
                                     "()Ljava/util/function/IntUnaryOperator;",
                                     {indy, new InsnNode(Opcodes::ARETURN)}, {base});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Lam", "make");
    base = findMethodIn(index, "Lam", "base");

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
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("Cap"),
                                arena.intern("lambda$make$0"), arena.intern("(II)I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));

    // 捕获 lambda：indy desc 带一个捕获参数 (I)，返回 functional interface。
    auto* indy =
        new InvokeDynamicInsnNode(arena.intern("applyAsInt"),
                                  arena.intern("(I)Ljava/util/function/IntUnaryOperator;"),
                                  lambdaMetafactoryBsm(index), bsmArgs);
    // lambda$make$0 是 private static（javac 合成），展开后应被提权（随夹具树一次落 DOM）。
    std::vector<ca::str::Utf8StringRef> noExc;
    auto* impl = new MethodNode(Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC | Opcodes::ACC_SYNTHETIC,
                                arena.intern("lambda$make$0"), arena.intern("(II)I"),
                                arena.intern(""), noExc);
    impl->instructions.add(new InsnNode(Opcodes::ICONST_0));
    impl->instructions.add(new InsnNode(Opcodes::IRETURN));
    impl->maxStack  = 1;
    impl->maxLocals = 2;
    MethodNode* m = addMethodToIndex(index, "Cap", "make",
                                     "(I)Ljava/util/function/IntUnaryOperator;",
                                     {new VarInsnNode(Opcodes::ILOAD, 0), indy,
                                      new InsnNode(Opcodes::ARETURN)}, {impl});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Cap", "make");
    impl = findMethodIn(index, "Cap", "lambda$make$0");


    // indy 消失、换成 new。合成类入 index，含 1 个捕获字段 cap0。
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::TYPE_INSN), 1);   // NEW
    auto* synth = index.getClass("mj2x/lambda/Lambda$0");
    ASSERT_NE(synth, nullptr);
    EXPECT_EQ(synth->fields.size(), 1u);
    EXPECT_EQ(synth->fields[0].name, "cap0");
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
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Lp/Rec;)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKEVIRTUAL, arena.intern("p/Rec"),
                                arena.intern("val"), arena.intern("()I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Lp/Rec;)I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("apply"),
                                           arena.intern("()Lp/Fn;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "UB", "make", "()Lp/Fn;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "UB", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::TYPE_INSN), 1);   // NEW 合成类
    auto* synth = index.getClass("mj2x/lambda/Lambda$0");
    ASSERT_NE(synth, nullptr);
    EXPECT_EQ(synth->fields.size(), 0u);   // unbound：无捕获字段
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
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("()I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKEVIRTUAL, arena.intern("p/Rec"),
                                arena.intern("val"), arena.intern("()I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("()I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("get"),
                                           arena.intern("(Lp/Rec;)Lp/Sup;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "BD", "make", "(Lp/Rec;)Lp/Sup;",
                                     {new VarInsnNode(Opcodes::ALOAD, 0), indy,
                                      new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "BD", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    auto* synth = index.getClass("mj2x/lambda/Lambda$0");
    ASSERT_NE(synth, nullptr);
    ASSERT_EQ(synth->fields.size(), 1u);
    EXPECT_EQ(synth->fields[0].name, "cap0");
    EXPECT_EQ(synth->fields[0].desc, "Lp/Rec;");
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
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Ljava/util/List;)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKEINTERFACE, arena.intern("java/util/List"),
                                arena.intern("size"), arena.intern("()I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Ljava/util/List;)I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("apply"), arena.intern("()Lp/Fn;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "IF", "make", "()Lp/Fn;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "IF", "make");


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
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)Lp/Rec;")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_NEWINVOKESPECIAL, arena.intern("p/Rec"),
                                arena.intern("<init>"), arena.intern("(I)V")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)Lp/Rec;")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("make"), arena.intern("()Lp/Factory;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    // 包私有构造器 Rec.<init>(I)V：展开后应被提权（随夹具树一次落 DOM）。
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
    MethodNode* m = addMethodToIndex(index, "p/Rec", "factory", "()Lp/Factory;",
                                     {indy, new InsnNode(Opcodes::ARETURN)}, {ctor});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "p/Rec", "factory");
    ctor = findMethodIn(index, "p/Rec", "<init>");


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
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Lp/Rec;)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_GETFIELD, arena.intern("p/Rec"),
                                arena.intern("field"), arena.intern("I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Lp/Rec;)I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("apply"), arena.intern("()Lp/Fn;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "GF", "make", "()Lp/Fn;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "GF", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 1);
    EXPECT_EQ(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
}

// ---- 阶段2d：装箱/桥接适配 ----

// 擦除泛型 + 基本类型 impl（Function<Integer,Integer> f = M::abs 形态）：
// sam (Object)Object、impl 静态 (I)I。SAM 体应为 CHECKCAST Integer / intValue 拆箱 /
// INVOKESTATIC impl / Integer.valueOf 装箱 / ARETURN。
TEST(InvokeDynamicPreprocessorTest, ExpandsErasedStaticRefWithBoxing)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(
        jbytecode::core::Type::getMethodType(arena.intern("(Ljava/lang/Object;)Ljava/lang/Object;")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("M"), arena.intern("abs"),
                                arena.intern("(I)I")));
    bsmArgs.emplace_back(
        jbytecode::core::Type::getMethodType(arena.intern("(Ljava/lang/Integer;)Ljava/lang/Integer;")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("apply"),
                                           arena.intern("()Ljava/util/function/Function;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "FnBox", "make", "()Ljava/util/function/Function;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "FnBox", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    MethodNode* sam = synthSamMethod(index, "mj2x/lambda/Lambda$0");
    ASSERT_NE(sam, nullptr);
    // 参数：CHECKCAST Integer + intValue()I 拆箱；返回：Integer.valueOf 装箱。
    EXPECT_EQ(countInsnType(sam, AbstractInsnNode::TYPE_INSN), 1);   // CHECKCAST Integer
    EXPECT_EQ(countMethodCall(sam, "java/lang/Integer", "intValue"), 1);
    EXPECT_EQ(firstCallOpcode(sam, "M", "abs"), Opcodes::INVOKESTATIC);
    EXPECT_EQ(countMethodCall(sam, "java/lang/Integer", "valueOf"), 1);
}

// 擦除 Consumer 形态（sam (Object)V、impl 返回 int）：SAM 体应 CHECKCAST String 收窄参数、
// POP 丢弃 impl 返回值后 RETURN。
TEST(InvokeDynamicPreprocessorTest, ExpandsErasedConsumerDroppingReturn)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Ljava/lang/Object;)V")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("C"), arena.intern("use"),
                                arena.intern("(Ljava/lang/String;)I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Ljava/lang/String;)V")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("accept"),
                                           arena.intern("()Ljava/util/function/Consumer;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "CnDrop", "make", "()Ljava/util/function/Consumer;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "CnDrop", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    MethodNode* sam = synthSamMethod(index, "mj2x/lambda/Lambda$0");
    ASSERT_NE(sam, nullptr);
    EXPECT_EQ(countInsnType(sam, AbstractInsnNode::TYPE_INSN), 1);   // CHECKCAST String
    // POP + RETURN（丢弃 int 返回值）。
    bool sawPop = false;
    for (auto* insn = sam->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getOpcode() == Opcodes::POP) sawPop = true;
    }
    EXPECT_TRUE(sawPop);
}

// 擦除 unbound 实例引用（ToIntFunction<String> f = String::length 形态）：receiver 从
// SAM 首参（Object）CHECKCAST 到 owner 后 INVOKEVIRTUAL。
TEST(InvokeDynamicPreprocessorTest, ExpandsErasedInstanceRefReceiverCast)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Ljava/lang/Object;)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKEVIRTUAL, arena.intern("java/lang/String"),
                                arena.intern("length"), arena.intern("()I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Ljava/lang/String;)I")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("applyAsInt"),
                                           arena.intern("()Ljava/util/function/ToIntFunction;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "RecvCast", "make",
                                     "()Ljava/util/function/ToIntFunction;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "RecvCast", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    MethodNode* sam = synthSamMethod(index, "mj2x/lambda/Lambda$0");
    ASSERT_NE(sam, nullptr);
    EXPECT_EQ(countInsnType(sam, AbstractInsnNode::TYPE_INSN), 1);   // CHECKCAST String
    EXPECT_EQ(firstCallOpcode(sam, "java/lang/String", "length"), Opcodes::INVOKEVIRTUAL);
}

// 是否存在指定 opcode 的指令。
bool hasOpcode(MethodNode* m, int opcode)
{
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getOpcode() == opcode) return true;
    }
    return false;
}

// 阶段2f：基本类型加宽（int 参 → long impl，i2l）。合成 SAM 体在 xLOAD 后发 I2L 再转调 impl。
TEST(InvokeDynamicPreprocessorTest, ExpandsPrimitiveWidening)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)V")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("W"), arena.intern("useLong"),
                                arena.intern("(J)V")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)V")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("accept"), arena.intern("()Lp/IntSink;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "Wide", "make", "()Lp/IntSink;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Wide", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    MethodNode* sam = synthSamMethod(index, "mj2x/lambda/Lambda$0");
    ASSERT_NE(sam, nullptr);
    EXPECT_TRUE(hasOpcode(sam, Opcodes::I2L));   // int 参加宽到 long
    EXPECT_EQ(firstCallOpcode(sam, "W", "useLong"), Opcodes::INVOKESTATIC);
}

// negative：基本类型收窄（long 参 → int impl）不合法、不在适配子集内，保持 fail-closed。
TEST(InvokeDynamicPreprocessorTest, LeavesPrimitiveNarrowingUnexpanded)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(J)V")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("N"), arena.intern("useInt"),
                                arena.intern("(I)V")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(J)V")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("accept"), arena.intern("()Lp/LongSink;"),
                                           lambdaMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "Narrow", "make", "()Lp/LongSink;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Narrow", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 1);
    EXPECT_EQ(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
}

// ---- 阶段2e：altMetafactory（serializable / marker interface / bridge）----

// altMetafactory bsm：desc 尾部是 [Object 变长（flags/markers/bridges）。
jbytecode::core::Handle lambdaAltMetafactoryBsm(CompilerIndex& index)
{
    auto& arena = index.arena();
    return jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/LambdaMetafactory"),
                  arena.intern("altMetafactory"),
                  arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;[Ljava/lang/Object;)"
                               "Ljava/lang/invoke/CallSite;"));
}

// 合成类是否 implements 指定接口内部名（DOM 接口表现查）。
bool synthImplements(CompilerIndex& index, const std::string& synthName, const std::string& iface)
{
    auto* cls = index.getClass(synthName);
    if (cls == nullptr || cls->dom == nullptr) return false;
    for (ca::u16 itf : cls->dom->interfaces) {
        if (cls->dom->className(itf) == iface) return true;
    }
    return false;
}

// FLAG_SERIALIZABLE：合成类追加实现 java/io/Serializable（marker，SAM 体不变）。
TEST(InvokeDynamicPreprocessorTest, ExpandsAltMetafactorySerializable)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("Lam"), arena.intern("base"),
                                arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(1);   // FLAG_SERIALIZABLE

    auto* indy = new InvokeDynamicInsnNode(arena.intern("applyAsInt"),
                                           arena.intern("()Ljava/util/function/IntUnaryOperator;"),
                                           lambdaAltMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "Lam", "make",
                                     "()Ljava/util/function/IntUnaryOperator;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Lam", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    ASSERT_NE(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
    EXPECT_TRUE(synthImplements(index, "mj2x/lambda/Lambda$0", "java/io/Serializable"));
    EXPECT_TRUE(
        synthImplements(index, "mj2x/lambda/Lambda$0", "java/util/function/IntUnaryOperator"));
}

// FLAG_MARKERS：合成类追加实现 bsmArgs 里列出的每个 Class marker 接口。
TEST(InvokeDynamicPreprocessorTest, ExpandsAltMetafactoryMarkerInterface)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("Lam"), arena.intern("base"),
                                arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(2);   // FLAG_MARKERS
    bsmArgs.emplace_back(1);   // markerCount
    bsmArgs.emplace_back(jbytecode::core::Type::getObjectType(arena.intern("java/lang/Cloneable")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("applyAsInt"),
                                           arena.intern("()Ljava/util/function/IntUnaryOperator;"),
                                           lambdaAltMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "Mk", "make",
                                     "()Ljava/util/function/IntUnaryOperator;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Mk", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    ASSERT_NE(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
    EXPECT_TRUE(synthImplements(index, "mj2x/lambda/Lambda$0", "java/lang/Cloneable"));
}

// FLAG_BRIDGES：合成类为每个 bridge 描述符额外发射一个 ACC_BRIDGE|ACC_SYNTHETIC 同名方法。
TEST(InvokeDynamicPreprocessorTest, ExpandsAltMetafactoryWithBridge)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    // 主 SAM 擦除为 (Object)Object，bridge 是具体 (String)String；impl 静态 id(String)String。
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(
        jbytecode::core::Type::getMethodType(arena.intern("(Ljava/lang/Object;)Ljava/lang/Object;")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("Id"), arena.intern("id"),
                                arena.intern("(Ljava/lang/String;)Ljava/lang/String;")));
    bsmArgs.emplace_back(
        jbytecode::core::Type::getMethodType(arena.intern("(Ljava/lang/String;)Ljava/lang/String;")));
    bsmArgs.emplace_back(4);   // FLAG_BRIDGES
    bsmArgs.emplace_back(1);   // bridgeCount
    bsmArgs.emplace_back(
        jbytecode::core::Type::getMethodType(arena.intern("(Ljava/lang/String;)Ljava/lang/String;")));

    auto* indy = new InvokeDynamicInsnNode(arena.intern("apply"), arena.intern("()Lp/Fn;"),
                                           lambdaAltMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "Br", "make", "()Lp/Fn;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Br", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    auto* synth = index.getClass("mj2x/lambda/Lambda$0");
    ASSERT_NE(synth, nullptr);
    // 主 SAM + bridge = 2 个 apply，其一带 ACC_BRIDGE（快照现查）。
    int applyCount = 0, bridgeCount = 0;
    for (const auto& cm : synth->methods) {
        if (cm.name != "apply") continue;
        ++applyCount;
        if (cm.access & Opcodes::ACC_BRIDGE) ++bridgeCount;
    }
    EXPECT_EQ(applyCount, 2);
    EXPECT_EQ(bridgeCount, 1);
}

// negative：altMetafactory 出现未知 flag 位（如 8）→ 保守 fail-closed，indy 保留、无合成类。
TEST(InvokeDynamicPreprocessorTest, LeavesUnknownAltFlagsUnexpanded)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern("Lam"), arena.intern("base"),
                                arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(8);   // 未知 flag 位

    auto* indy = new InvokeDynamicInsnNode(arena.intern("applyAsInt"),
                                           arena.intern("()Ljava/util/function/IntUnaryOperator;"),
                                           lambdaAltMetafactoryBsm(index), bsmArgs);
    MethodNode* m = addMethodToIndex(index, "Unk", "make",
                                     "()Ljava/util/function/IntUnaryOperator;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Unk", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 1);
    EXPECT_EQ(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
}

// ---- #1182：合成 lambda 类宿主包判定（包私有 SAM 不得落固定 mj2x/lambda 包）----

// 在 index 里造接口类（access 原样落 flags），含 public abstract apply(I)I。
void addInterfaceToIndex(CompilerIndex& index, const std::string& name, int access)
{
    auto& arena = index.arena();
    auto  cn    = std::make_unique<ClassNode>();
    cn->name      = arena.intern(name.c_str());
    cn->version   = Opcodes::V1_8;
    cn->access    = access | Opcodes::ACC_INTERFACE | Opcodes::ACC_ABSTRACT;
    cn->superName = arena.intern("java/lang/Object");
    std::vector<ca::str::Utf8StringRef> noExc;
    cn->methods.push_back(new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_ABSTRACT,
                                         arena.intern("apply"), arena.intern("(I)I"),
                                         arena.intern(""), noExc));
    index.addClass(name, test::classFromTree(name, *cn, arena));
}

// 在 caller 类造无状态 lambda 站点（SAM 返回类型 = samIface 内部名）+ 静态 impl 方法。
// 无状态：indy desc "()"（零捕获），impl 布局 = sam = (I)I。tag 供同 caller 双站点。
MethodNode* addLambdaCaller(CompilerIndex& index, const std::string& callerName,
                            const std::string& samIface, const std::string& tag = "")
{
    auto& arena = index.arena();
    const std::string makeName = "make" + tag;
    const std::string implName = "impl" + tag;
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Handle(Opcodes::H_INVOKESTATIC, arena.intern(callerName.c_str()),
                                arena.intern(implName.c_str()), arena.intern("(I)I")));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(I)I")));
    auto* indy = new InvokeDynamicInsnNode(
        arena.intern("apply"),
        arena.intern(("()" + std::string("L") + samIface + ";").c_str()),
        lambdaMetafactoryBsm(index), bsmArgs);
    std::vector<ca::str::Utf8StringRef> noExc;
    const std::string                   desc = "()L" + samIface + ";";
    auto* cls = index.getClass(callerName);
    if (cls == nullptr) {
        // 首站点：建类（make 方法即本站点方法，impl 随夹具树一次落 DOM）。
        auto* impl = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                    arena.intern(implName.c_str()), arena.intern("(I)I"),
                                    arena.intern(""), noExc);
        impl->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
        impl->instructions.add(new InsnNode(Opcodes::IRETURN));
        impl->maxStack  = 1;
        impl->maxLocals = 1;
        return addMethodToIndex(index, callerName, makeName, desc,
                                {indy, new InsnNode(Opcodes::ARETURN)}, {impl});
    }
    // 同 caller 第二站点：往 shelf 夹具树追加方法后整体重落 DOM（addClass 语义 = 替换）。
    auto& cn = treeShelf()[callerName].back();
    auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             arena.intern(makeName.c_str()), arena.intern(desc.c_str()),
                             arena.intern(""), noExc);
    m->instructions.add(indy);
    m->instructions.add(new InsnNode(Opcodes::ARETURN));
    m->maxStack  = 10;
    m->maxLocals = 4;
    auto* impl = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                arena.intern(implName.c_str()), arena.intern("(I)I"),
                                arena.intern(""), noExc);
    impl->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    impl->instructions.add(new InsnNode(Opcodes::IRETURN));
    impl->maxStack  = 1;
    impl->maxLocals = 1;
    cn->methods.push_back(m);
    cn->methods.push_back(impl);
    addClassFromTree(index, callerName);
    return m;
}

// 包私有 SAM（编译输入）→ 合成类落 SAM 同包，固定包不再产类（#1182 复现形态）。
TEST(InvokeDynamicPreprocessorTest, MovesSynthNextToPackagePrivateSam)
{
    CompilerIndex index;
    addInterfaceToIndex(index, "demo/pkg/Op", 0);   // 包私有（无 ACC_PUBLIC）
    MethodNode* m = addLambdaCaller(index, "demo/pkg/Caller", "demo/pkg/Op");

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "demo/pkg/Caller", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_NE(index.getClass("demo/pkg/Lambda$0"), nullptr);
    EXPECT_EQ(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
}

// public SAM（编译输入）→ 维持 mj2x/lambda 前缀（既有绿面零漂移）。
TEST(InvokeDynamicPreprocessorTest, KeepsMj2xLambdaPackageForPublicSam)
{
    CompilerIndex index;
    addInterfaceToIndex(index, "demo/pkg/Op", Opcodes::ACC_PUBLIC);
    MethodNode* m = addLambdaCaller(index, "demo/pkg/Caller", "demo/pkg/Op");

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "demo/pkg/Caller", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_NE(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
    EXPECT_EQ(index.getClass("demo/pkg/Lambda$0"), nullptr);
}

// 库 SAM（不在 index，跨包引用只可能 public）→ 维持 mj2x/lambda 前缀（零漂移面）。
TEST(InvokeDynamicPreprocessorTest, KeepsMj2xLambdaPackageForLibrarySam)
{
    CompilerIndex index;
    MethodNode*   m = addLambdaCaller(index, "demo/pkg/Caller", "demo/pkg/LibOp");

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "demo/pkg/Caller", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_NE(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
    EXPECT_EQ(index.getClass("demo/pkg/Lambda$0"), nullptr);
}

// default 包包私有 SAM → 合成类落 default 包（裸名 Lambda$0，不得产出非法名 /Lambda$0）。
// （#1182 评审收口 P1：无 '/' 时宿主包必须返回空串。）
TEST(InvokeDynamicPreprocessorTest, PlacesDefaultPackagePackagePrivateSamSynthInDefaultPackage)
{
    CompilerIndex index;
    addInterfaceToIndex(index, "PriOp", 0);   // default 包包私有接口（内部名无 '/'）
    MethodNode* m = addLambdaCaller(index, "Caller", "PriOp");

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Caller", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_NE(index.getClass("Lambda$0"), nullptr);
    EXPECT_EQ(index.getClass("/Lambda$0"), nullptr);
}

// 在 index 里造外围类，其 InnerClasses 属性声明嵌套 SAM 的访问位——javac 把
// 嵌套类型的真实 access 位记在外围类的 InnerClasses 属性而非嵌套类自身 flags。
void addOuterClassWithNestedSam(CompilerIndex& index, const std::string& outerName,
                                const std::string& samName, int samAccess)
{
    auto& arena = index.arena();
    auto  cn    = std::make_unique<ClassNode>();
    cn->name      = arena.intern(outerName.c_str());
    cn->version   = Opcodes::V1_8;
    cn->access    = Opcodes::ACC_PUBLIC;
    cn->superName = arena.intern("java/lang/Object");
    cn->innerClasses.push_back(cn->create<InnerClassNode>(
        arena.intern(samName.c_str()), arena.intern(outerName.c_str()),
        arena.intern(samName.substr(samName.rfind('$') + 1).c_str()), samAccess));
    index.addClass(outerName, test::classFromTree(outerName, *cn, arena));
}

// J-8（LambdasCapture 首曝形态）：SAM 是包私有嵌套接口且不在 index（单类输入
// 只带站点宿主），外围类在 index → 读 InnerClasses 真实访问位，非公开落 SAM 同包。
TEST(InvokeDynamicPreprocessorTest, PlacesSynthNextToNonPublicNestedSamMissingFromIndex)
{
    CompilerIndex index;
    addOuterClassWithNestedSam(index, "demo/pkg/Outer", "demo/pkg/Outer$Op", 0);
    MethodNode* m = addLambdaCaller(index, "demo/pkg/Caller", "demo/pkg/Outer$Op");

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "demo/pkg/Caller", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_NE(index.getClass("demo/pkg/Lambda$0"), nullptr);
    EXPECT_EQ(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
}

// 对称守卫：InnerClasses 标 public 的嵌套 SAM（不在 index）→ 维持 mj2x/lambda，
// 防止外围类命中即同包的过宽回落。
TEST(InvokeDynamicPreprocessorTest, KeepsMj2xLambdaPackageForPublicNestedSamMissingFromIndex)
{
    CompilerIndex index;
    addOuterClassWithNestedSam(index, "demo/pkg/Outer", "demo/pkg/Outer$Op",
                               Opcodes::ACC_PUBLIC);
    MethodNode* m = addLambdaCaller(index, "demo/pkg/Caller", "demo/pkg/Outer$Op");

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "demo/pkg/Caller", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_NE(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
    EXPECT_EQ(index.getClass("demo/pkg/Lambda$0"), nullptr);
}

// default 包 private 嵌套 SAM（javac 落盘不写 ACC_PRIVATE，非 public 位触发判定）
// → 同样落 default 包裸名（#1182 评审收口 P1 第二形态）。
TEST(InvokeDynamicPreprocessorTest, PlacesDefaultPackagePrivateNestedSamSynthInDefaultPackage)
{
    CompilerIndex index;
    addInterfaceToIndex(index, "Outer$Op", 0);   // default 包嵌套私有接口（javac 形态）
    MethodNode* m = addLambdaCaller(index, "Outer", "Outer$Op");

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "Outer", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_NE(index.getClass("Lambda$0"), nullptr);
    EXPECT_EQ(index.getClass("/Lambda$0"), nullptr);
}

// protected 嵌套 SAM 的 javac 落盘形态即 ACC_PUBLIC（javap 实证 0x0601）→ 谓词不
// 触发，维持 mj2x/lambda 且运行绿（行为钉；protected 分支在判定中实为死分支）。
TEST(InvokeDynamicPreprocessorTest, KeepsMj2xLambdaPackageForJavacProtectedNestedSam)
{
    CompilerIndex index;
    addInterfaceToIndex(index, "demo/pkg/Outer$Op", Opcodes::ACC_PUBLIC);   // javac 0x0601 形态
    MethodNode* m = addLambdaCaller(index, "demo/pkg/Outer", "demo/pkg/Outer$Op");

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m = findMethodIn(index, "demo/pkg/Outer", "make");


    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_NE(index.getClass("mj2x/lambda/Lambda$0"), nullptr);
    EXPECT_EQ(index.getClass("demo/pkg/Lambda$0"), nullptr);
}

// 用户类占用合成名 → 探测前跳，不静默覆盖；计数器推进，第二站点不回退撞名。
TEST(InvokeDynamicPreprocessorTest, SkipsUserClassOccupyingSynthName)
{
    CompilerIndex index;
    addInterfaceToIndex(index, "demo/pkg/Op", 0);
    // 用户恰有一个非库类叫 demo/pkg/Lambda$0（合法但极端），含标记方法 dummy。
    addMethodToIndex(index, "demo/pkg/Lambda$0", "dummy", "()V", {});
    MethodNode* m1 = addLambdaCaller(index, "demo/pkg/Caller", "demo/pkg/Op");
    MethodNode* m2 = addLambdaCaller(index, "demo/pkg/Caller", "demo/pkg/Op", "2");

    test::runWithDomSync(InvokeDynamicPreprocessor(), index);
    m1 = findMethodIn(index, "demo/pkg/Caller", "make");
    m2 = findMethodIn(index, "demo/pkg/Caller", "make2");


    EXPECT_EQ(countInsnType(m1, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    EXPECT_EQ(countInsnType(m2, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 0);
    // 用户类原样保留（未被合成类覆盖）。
    auto* user = index.getClass("demo/pkg/Lambda$0");
    ASSERT_NE(user, nullptr);
    bool hasDummy = false;
    for (const auto& cm : user->methods) {
        if (cm.name == "dummy") hasDummy = true;
    }
    EXPECT_TRUE(hasDummy);
    // 合成类跳到 Lambda$1，第二站点推到 Lambda$2（计数器确已跨过占用名推进）。
    EXPECT_NE(index.getClass("demo/pkg/Lambda$1"), nullptr);
    EXPECT_NE(index.getClass("demo/pkg/Lambda$2"), nullptr);
}
