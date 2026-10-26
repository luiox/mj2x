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
// 同时跟踪操作数栈峰值并断言 ≤ m->maxStack——本文件用例全为无参方法（形参
// 走局部变量不占栈），预压不干扰峰值口径。这是 issue #1 的判据：LDC 展开链
// 峰值若超出方法 maxStack，stackc 后端 union jvalue s[maxStack] 即写穿。
bool verifyStackShape(MethodNode* m, std::string& reason)
{
    std::vector<std::string> stack;
    const std::string        methodDesc = m->desc.to_std_string();
    // #921 staticArena 退役：无参 getDescriptor() 已删除，检视统一走注入版
    //（函数级局部 arena，仅瞬时转串）。
    ca::str::Utf8StringArena scratch;
    for (const auto&         arg :
         jbytecode::core::Type::getArgumentTypes(ca::str::Utf8StringRef::from_string_view(methodDesc))) {
        // 本测试不用 long/double 形参，category 差异不影响判据。
        stack.push_back(arg.getDescriptor(scratch).to_std_string());
    }
    size_t peak = stack.size();

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
            const auto args = jbytecode::core::Type::getArgumentTypes(
                ca::str::Utf8StringRef::from_string_view(miDesc));
            const auto ret = jbytecode::core::Type::getReturnType(
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
                if (!descCompatible(args[static_cast<size_t>(i)].getDescriptor(scratch).to_std_string(),
                                    actual)) {
                    reason = "arg mismatch at " + mi->name.to_std_string() + ": expected " +
                             args[static_cast<size_t>(i)].getDescriptor(scratch).to_std_string() +
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
            const std::string retDesc = ret.getDescriptor(scratch).to_std_string();
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
                const auto ret = jbytecode::core::Type::getReturnType(
                    ca::str::Utf8StringRef::from_string_view(mDesc));
                if (!descCompatible(ret.getDescriptor(scratch).to_std_string(), stack.back())) {
                    reason = "areturn type mismatch: " + stack.back();
                    return false;
                }
                stack.pop_back();
            }
        }
        if (stack.size() > peak) peak = stack.size();
    }
    if (!stack.empty()) {
        reason = "leftover stack depth " + std::to_string(stack.size());
        return false;
    }
    if (peak > static_cast<size_t>(m->maxStack)) {
        reason = "peak stack depth " + std::to_string(peak) + " exceeds maxStack " +
                 std::to_string(m->maxStack);
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

// Lookup.findXxx 调用点的发射文本形态（find4 钉表用）。
struct LookupCallShape
{
    std::string name;
    std::string desc;
};

// find4 钉表用例：tag → Handle 描述符（field tag 用字段描述符）+ 期望 find
// 名字 + 描述符。
struct FindNameDescCase
{
    int         tag;
    const char* handleDesc;
    const char* name;
    const char* desc;
};

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

        // 展开必须抬 maxStack +4（16 为本 fixture 初值；否则 stackc 后端
        // union jvalue s[maxStack] 开小，LDC 解析内联写穿 s[]——issue #1）。
        EXPECT_EQ(m->maxStack, 16 + 4);

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

// ---- find4 收敛钉表：tag→find* 全表（8 tag + interface + 未知 tag 缺省分支）----
// 上面的逐 tag 用例只断言 find 名字；这里把展开后 Lookup.findXxx 调用点的
// 名字与方法描述符逐字节钉死（#1027 find4：ldc 侧 find 判定切 indyHandleFindFor
// 单源后的发射文本不变性判据）。

// 展开一个「ldc Handle(tag); areturn」方法，返回 Lookup.findXxx 调用点的
// (name, desc)，并顺带做栈形校验（任何压栈顺序/数量错误都会被抓住）。
// handleDesc：Handle 的描述符——field tag 须传字段描述符，method tag 传方法
// 描述符（findGetter 系第 3 参 = 字段类型 Class，来源 h.getDesc()）。
LookupCallShape expandAndCaptureLookupCall(int tag, const char* handleDesc)
{
    CompilerIndex index;
    Handle h = makeHandle(index, tag, "Sample", "m", handleDesc);
    std::vector<AbstractInsnNode*> insns;
    insns.push_back(new LdcInsnNode(h));
    insns.push_back(new JumpInsnNode(Opcodes::ARETURN, nullptr));
    MethodNode* m =
        addMethodToIndex(index, "Sample", "run", "()Ljava/lang/invoke/MethodHandle;", insns);

    LdcPreprocessor pre;
    pre.process(index);

    std::string reason;
    EXPECT_TRUE(verifyStackShape(m, reason)) << "tag=" << tag << ": " << reason;

    LookupCallShape found;
    int hits = 0;
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
        auto* mi = static_cast<MethodInsnNode*>(insn);
        if (mi->owner.to_std_string() != "java/lang/invoke/MethodHandles$Lookup") continue;
        ++hits;
        found.name = mi->name.to_std_string();
        found.desc = mi->desc.to_std_string();
    }
    EXPECT_EQ(hits, 1) << "tag=" << tag << ": expected exactly one Lookup.findXxx call";
    return found;
}

TEST_F(LdcPreprocessorTest, FindNameAndDescFullTable)
{
    const FindNameDescCase cases[] = {
        {Opcodes::H_GETFIELD, "I", "findGetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;"},
        {Opcodes::H_GETSTATIC, "J", "findStaticGetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;"},
        {Opcodes::H_PUTFIELD, "I", "findSetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;"},
        {Opcodes::H_PUTSTATIC, "D", "findStaticSetter",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
         "Ljava/lang/invoke/MethodHandle;"},
        {Opcodes::H_INVOKESTATIC, "(I)V", "findStatic",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;"},
        {Opcodes::H_INVOKESPECIAL, "(I)V", "findSpecial",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
         "Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;"},
        {Opcodes::H_NEWINVOKESPECIAL, "(I)V", "findConstructor",
         "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;"},
        {Opcodes::H_INVOKEVIRTUAL, "(I)V", "findVirtual",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;"},
        {Opcodes::H_INVOKEINTERFACE, "(I)V", "findVirtual",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;"},
        // 未知 tag：缺省分支 → findVirtual 形态（与单源 default 一致）。
        {999, "(I)V", "findVirtual",
         "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
         "Ljava/lang/invoke/MethodHandle;"},
    };
    for (const auto& c : cases) {
        const LookupCallShape got = expandAndCaptureLookupCall(c.tag, c.handleDesc);
        EXPECT_EQ(got.name, c.name) << "tag=" << c.tag;
        EXPECT_STREQ(got.desc.c_str(), c.desc) << "tag=" << c.tag;
    }
}

// ldc MethodType：fromMethodDescriptorString(desc, callerClassLoader)。
TEST_F(LdcPreprocessorTest, ExpandMethodTypeConstant)
{
    CompilerIndex index;
    Type mt = jbytecode::core::Type::getMethodType(ca::str::Utf8StringRef::from_cstr("(I)V"));
    runAndVerify(ca::core::Any(mt), "()Ljava/lang/invoke/MethodType;", nullptr);
}

// issue #1 回归：真实类文件形态下（ldc Handle + invoke* ⇒ max_stack=2），展开链
// 峰值 D+5（D=0 起算即 5）须被抬升后的 maxStack 覆盖。预处理器漏抬时
// verifyStackShape 的峰值断言直接判红——这正是 stackc s[] 写穿的成因。
TEST_F(LdcPreprocessorTest, BumpsMaxStackToCoverExpansionPeak)
{
    CompilerIndex index;
    Handle h = makeHandle(index, Opcodes::H_INVOKESTATIC, "Sample", "add", "(II)I");
    std::vector<AbstractInsnNode*> insns;
    insns.push_back(new LdcInsnNode(ca::core::Any(h)));
    insns.push_back(new JumpInsnNode(Opcodes::ARETURN, nullptr));
    MethodNode* m =
        addMethodToIndex(index, "Sample", "run", "()Ljava/lang/invoke/MethodHandle;", insns);
    m->maxStack = 2;   // 手构样本 LdcMhSample 的真实 max_stack 形态。

    LdcPreprocessor pre;
    pre.process(index);

    EXPECT_EQ(m->maxStack, 2 + 4);
    std::string reason;
    ASSERT_TRUE(verifyStackShape(m, reason)) << reason;
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
    // 无展开不得误抬 maxStack。
    EXPECT_EQ(m->maxStack, 16);
}

}   // namespace
