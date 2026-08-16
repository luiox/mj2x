// LdcPreprocessor 单元测试。
//
// 核心判据：展开序列必须按 findXxx 描述符的参数顺序压栈——这是历史缺陷的
// 直接形态（旧实现压 Class, String(desc), MethodType, String(name)，多一个
// 参数且 name/MethodType 顺序颠倒，产出必 VerifyError 的字节码）。这里用一个
// 迷你栈校验器（LDC/INVOKE/GETSTATIC 抽象解释）对展开后的整方法做栈形一致性
// 检查，任何压栈顺序/数量错误都会被抓住，不需要真跑 JVM verifier。
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/preprocessor/ldc_preprocessor.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/core/any.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// 在 index 里造一个类，含一个静态方法，方法体是给定指令。返回方法指针（归 index 所有）。
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
    auto* mn = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                              arena.intern(methodName.c_str()),
                              arena.intern(methodDesc.c_str()), arena.intern(""), noExcepts);
    mn->maxStack  = 16;
    mn->maxLocals = 4;
    for (auto* insn : insns) mn->instructions.add(insn);
    cn->methods.push_back(mn);

    CompilerClass cls;
    cls.classNode = std::move(cn);
    index.addClass(className, std::move(cls));
    return index.getClass(className)->classNode->methods.back();
}

// ---- 迷你栈校验器：对 LDC/INVOKE/GETSTATIC 做抽象解释，模拟 verifier 的栈形检查 ----

// ldc cst → 栈描述符。
std::string ldcCstDesc(const ca::core::Any& cst)
{
    if (cst.as<ca::i32>() != nullptr) return "I";
    if (cst.as<ca::i64>() != nullptr) return "J";
    if (cst.as<float>() != nullptr) return "F";
    if (cst.as<double>() != nullptr) return "D";
    if (cst.as<std::string>() != nullptr || cst.as<ca::str::Utf8StringRef>() != nullptr) {
        return "Ljava/lang/String;";
    }
    if (auto* t = cst.as<Type>()) {
        if (t->getSort() == Type::METHOD) return "Ljava/lang/invoke/MethodType;";
        return "Ljava/lang/Class;";
    }
    return "<?>";
}

// expected（描述符 param）与 actual（栈顶描述符）是否类型兼容。
// 引用间只做 Object 兜底（跨类赋值关系属 verifier 的 Assignability，超出本测试目标）。
bool descCompatible(const std::string& expected, const std::string& actual)
{
    if (expected == actual) return true;
    return expected == "Ljava/lang/Object;" && actual.size() > 0 && actual[0] == 'L';
}

// 抽象解释方法体，返回是否全程栈形一致；失败原因写入 reason。
// 方法形参按 desc 预压栈（静态方法），返回时栈应只剩返回类型。
bool verifyStackShape(MethodNode* m, std::string& reason)
{
    std::vector<std::string> stack;
    const std::string        methodDesc = m->desc.to_std_string();
    for (const auto&         arg :
         Type::getArgumentTypes(ca::str::Utf8StringRef::from_string_view(methodDesc))) {
        // 本测试不用 long/double 形参，category 差异不影响判据。
        stack.push_back(arg.getDescriptor().to_std_string());
    }

    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() == AbstractInsnNode::LDC_INSN) {
            auto* ldc = static_cast<LdcInsnNode*>(insn);
            stack.push_back(ldcCstDesc(ldc->cst));
        }
        else if (insn->getType() == AbstractInsnNode::FIELD_INSN) {
            auto* fi = static_cast<FieldInsnNode*>(insn);
            if (fi->opcode == Opcodes::GETSTATIC) {
                stack.push_back(fi->desc.to_std_string());
            }
            else {
                reason = "unexpected field insn in expansion";
                return false;
            }
        }
        else if (insn->getType() == AbstractInsnNode::METHOD_INSN) {
            auto* mi = static_cast<MethodInsnNode*>(insn);
            // desc 先落具名局部（Type 持有的是视图，不能指向临时 string）。
            const std::string miDesc = mi->desc.to_std_string();
            const auto args = Type::getArgumentTypes(
                ca::str::Utf8StringRef::from_string_view(miDesc));
            const auto ret = Type::getReturnType(
                ca::str::Utf8StringRef::from_string_view(miDesc));
            const bool hasReceiver = mi->getOpcode() != Opcodes::INVOKESTATIC;
            const int  argCount    = static_cast<int>(args.size());
            const int  pops        = argCount + (hasReceiver ? 1 : 0);
            if (static_cast<int>(stack.size()) < pops) {
                reason = "stack underflow at " + mi->name.to_std_string();
                return false;
            }
            // 参数逆序弹栈（receiver 在帧底，参数在其上）。
            for (int i = argCount - 1; i >= 0; --i) {
                const std::string actual = stack.back();
                stack.pop_back();
                if (!descCompatible(args[static_cast<size_t>(i)].getDescriptor().to_std_string(),
                                    actual)) {
                    reason = "arg mismatch at " + mi->name.to_std_string() + ": expected " +
                             args[static_cast<size_t>(i)].getDescriptor().to_std_string() +
                             " got " + actual;
                    return false;
                }
            }
            if (hasReceiver) {
                const std::string receiver = stack.back();
                stack.pop_back();
                if (receiver != "L" + mi->owner.to_std_string() + ";") {
                    reason = "receiver mismatch at " + mi->name.to_std_string() + ": " + receiver;
                    return false;
                }
            }
            const std::string retDesc = ret.getDescriptor().to_std_string();
            if (retDesc != "V") stack.push_back(retDesc);
        }
        else if (insn->getType() == AbstractInsnNode::JUMP_INSN) {
            auto* ji = static_cast<JumpInsnNode*>(insn);
            if (ji->opcode == Opcodes::ARETURN) {
                if (stack.size() != 1) {
                    reason = "areturn with stack depth " + std::to_string(stack.size());
                    return false;
                }
                const std::string mDesc = m->desc.to_std_string();
                const auto ret = Type::getReturnType(
                    ca::str::Utf8StringRef::from_string_view(mDesc));
                if (!descCompatible(ret.getDescriptor().to_std_string(), stack.back())) {
                    reason = "areturn type mismatch: " + stack.back();
                    return false;
                }
                stack.pop_back();
            }
        }
    }
    if (!stack.empty()) {
        reason = "leftover stack depth " + std::to_string(stack.size());
        return false;
    }
    return true;
}

// 构造 ldc Handle + areturn 的方法，跑 preprocessor，断言栈形一致 + findXxx 名称。
Handle makeHandle(CompilerIndex& index, int tag, const char* owner, const char* name,
                  const char* desc)
{
    auto& arena = index.arena();
    return Handle(tag, arena.intern(owner), arena.intern(name), arena.intern(desc));
}

class LdcPreprocessorTest : public ::testing::Test
{
protected:
    // 跑一个「ldc <cst>; areturn」方法经过 preprocessor 的完整检查。
    static void runAndVerify(const ca::core::Any& cst, const char* methodDesc,
                             const char* expectFindName)
    {
        CompilerIndex index;
        std::vector<AbstractInsnNode*> insns;
        insns.push_back(new LdcInsnNode(cst));
        insns.push_back(new JumpInsnNode(Opcodes::ARETURN, nullptr));
        MethodNode* m = addMethodToIndex(index, "Sample", "run", methodDesc, insns);

        LdcPreprocessor pre;
        pre.process(index);

        std::string reason;
        ASSERT_TRUE(verifyStackShape(m, reason)) << reason;

        if (expectFindName != nullptr) {
            int found = 0;
            for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
                if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
                auto* mi = static_cast<MethodInsnNode*>(insn);
                if (mi->owner.to_std_string() == "java/lang/invoke/MethodHandles$Lookup" &&
                    mi->name.to_std_string() == expectFindName) {
                    ++found;
                }
            }
            EXPECT_EQ(found, 1) << "expected exactly one " << expectFindName << " call";
        }
    }
};

// method-tag：findVirtual/findStatic/findSpecial/findConstructor 的栈序与参数数。
TEST_F(LdcPreprocessorTest, ExpandInvokeVirtualHandle)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_INVOKEVIRTUAL, "Sample", "greet",
                          "(Ljava/lang/String;)Ljava/lang/String;");
    runAndVerify(ca::core::Any(h), "()Ljava/lang/invoke/MethodHandle;", "findVirtual");
}

TEST_F(LdcPreprocessorTest, ExpandInvokeInterfaceHandle)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_INVOKEINTERFACE, "Sample", "greet",
                          "(Ljava/lang/String;)Ljava/lang/String;");
    runAndVerify(ca::core::Any(h), "()Ljava/lang/invoke/MethodHandle;", "findVirtual");
}

TEST_F(LdcPreprocessorTest, ExpandInvokeStaticHandle)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_INVOKESTATIC, "Sample", "add", "(II)I");
    runAndVerify(ca::core::Any(h), "()Ljava/lang/invoke/MethodHandle;", "findStatic");
}

TEST_F(LdcPreprocessorTest, ExpandInvokeSpecialHandle)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_INVOKESPECIAL, "Sample", "secret", "(I)I");
    runAndVerify(ca::core::Any(h), "()Ljava/lang/invoke/MethodHandle;", "findSpecial");
}

TEST_F(LdcPreprocessorTest, ExpandNewInvokeSpecialHandle)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_NEWINVOKESPECIAL, "Sample", "<init>", "(I)V");
    runAndVerify(ca::core::Any(h), "()Ljava/lang/invoke/MethodHandle;", "findConstructor");
}

// field-tag：findGetter/findSetter 系，第 3 参是字段类型 Class。
TEST_F(LdcPreprocessorTest, ExpandGetFieldHandlePrimitive)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_GETFIELD, "Sample", "count", "I");
    runAndVerify(ca::core::Any(h), "()Ljava/lang/invoke/MethodHandle;", "findGetter");
}

TEST_F(LdcPreprocessorTest, ExpandPutFieldHandleReference)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_PUTFIELD, "Sample", "name", "Ljava/lang/String;");
    runAndVerify(ca::core::Any(h), "()Ljava/lang/invoke/MethodHandle;", "findSetter");
}

TEST_F(LdcPreprocessorTest, ExpandGetStaticHandle)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_GETSTATIC, "Sample", "total", "J");
    runAndVerify(ca::core::Any(h), "()Ljava/lang/invoke/MethodHandle;", "findStaticGetter");
}

TEST_F(LdcPreprocessorTest, ExpandPutStaticHandle)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_PUTSTATIC, "Sample", "ratio", "D");
    runAndVerify(ca::core::Any(h), "()Ljava/lang/invoke/MethodHandle;", "findStaticSetter");
}

// ldc MethodType：fromMethodDescriptorString(desc, callerClassLoader)。
TEST_F(LdcPreprocessorTest, ExpandMethodTypeConstant)
{
    CompilerIndex index;
    Type mt = Type::getMethodType(ca::str::Utf8StringRef::from_cstr("(I)V"));
    runAndVerify(ca::core::Any(mt), "()Ljava/lang/invoke/MethodType;", nullptr);
}

// 非 Handle/MethodType 的 ldc 不动。
TEST_F(LdcPreprocessorTest, LeavesPlainLdcAlone)
{
    CompilerIndex index;
    std::vector<AbstractInsnNode*> insns;
    insns.push_back(new LdcInsnNode(std::string("plain")));
    insns.push_back(new JumpInsnNode(Opcodes::ARETURN, nullptr));
    MethodNode* m = addMethodToIndex(index, "Sample", "run", "()Ljava/lang/String;", insns);

    LdcPreprocessor pre;
    pre.process(index);

    EXPECT_EQ(m->instructions.size(), 2u);
}

}   // namespace
