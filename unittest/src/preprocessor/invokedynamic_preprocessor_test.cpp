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

// negative：非 StringConcatFactory 的 indy（如 LambdaMetafactory）保持原样 fail-closed。
TEST(InvokeDynamicPreprocessorTest, LeavesLambdaIndyUnexpanded)
{
    CompilerIndex index;
    auto&         arena = index.arena();

    Handle lambdaBsm(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/LambdaMetafactory"),
                     arena.intern("metafactory"),
                     arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                                  "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;"
                                  "Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)"
                                  "Ljava/lang/invoke/CallSite;"));
    auto* indy = new InvokeDynamicInsnNode(arena.intern("applyAsInt"),
                                           arena.intern("()Ljava/util/function/IntUnaryOperator;"),
                                           lambdaBsm, std::vector<ca::core::Any>{});
    MethodNode* m = addMethodToIndex(index, "Lam", "make",
                                     "()Ljava/util/function/IntUnaryOperator;",
                                     {indy, new InsnNode(Opcodes::ARETURN)});

    InvokeDynamicPreprocessor().process(index);

    // indy 仍在（未被展开），交由 SSA fail-closed。
    EXPECT_EQ(countInsnType(m, AbstractInsnNode::INVOKE_DYNAMIC_INSN), 1);
    EXPECT_EQ(countMethodCall(m, "java/lang/StringBuilder", "<init>"), 0);
}
