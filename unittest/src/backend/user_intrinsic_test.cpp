// 用户自定义 intrinsic（issue #818）三后端单测。
//
// 验证统一契约：用户通过 UserIntrinsicConfig 注册的 Java 调用点
// （owner.name:desc，静态）在 StackC / RegC / VMC4 里都降级为对用户
// symbol 的直接调用，而不是通用 JNI 派发：
//   - RegC：emitIntrinsicCall 走 UserIntrinsic emitKind，保留 ExceptionCheck；
//   - StackC：emitInvoke 弹出 stack[] 参数直接调 symbol；
//   - VMC4：encodeCall 标记 VmMethodRef.userSymbol，helper 解包 regs 直调。
// 本文件只验证生成代码形态（不执行）；执行级验证在 E2E 套件。

#include <gmock/gmock.h>

#include <memory>
#include <string>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"
#include "mj2x/backend/stackc/stackc_backend.hpp"
#include "mj2x/backend/vmc4/vm_encoder.hpp"
#include "mj2x/backend/vmc4/vmc4_instance.hpp"
#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/core/user_intrinsics.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/mir/register_ir/reg_dce.hpp"
#include "mj2x/mir/register_ir/reg_lowering.hpp"
#include "mj2x/mir/ssa/ssa_builder.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

ca::str::Utf8StringRef U(const char* s)
{
    return ca::str::Utf8StringRef::from_cstr(s);
}

std::unique_ptr<MethodNode> makeMethod(int access, const char* name, const char* desc,
                                       std::initializer_list<AbstractInsnNode*> insns,
                                       int maxStack, int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(access, U(name), U(desc), U(""), noExcepts);
    mn->maxStack  = maxStack;
    mn->maxLocals = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

/// com/x/F.m(II)I 静态调用（int userCall(int a, int b) { return F.m(a, b); }）。
std::unique_ptr<MethodNode> makeUserCallMethod()
{
    return makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "userCall", "(II)I",
                      {
                          new VarInsnNode(Opcodes::ILOAD, 0),
                          new VarInsnNode(Opcodes::ILOAD, 1),
                          new MethodInsnNode(
                              Opcodes::INVOKESTATIC, U("com/x/F"), U("m"), U("(II)I"), false),
                          new InsnNode(Opcodes::IRETURN),
                      },
                      2, 2);
}

/// 一条测试用用户 intrinsic：com/x/F.m(II)I -> my_m。
UserIntrinsic makeUserEntry(bool passContext = false, bool mayThrow = false)
{
    UserIntrinsic u;
    u.owner       = "com/x/F";
    u.name        = "m";
    u.desc        = "(II)I";
    u.symbol      = "my_m";
    u.passContext = passContext;
    u.mayThrow    = mayThrow;
    return u;
}

// ===========================================================================
// RegC
// ===========================================================================

RegCEmitResult emitRegCWithUsers(const std::vector<UserIntrinsic>& users, std::string& reason)
{
    auto mn = makeUserCallMethod();
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "UserIntrinsicRegC", mn->name.to_std_string(),
                             mn->desc.to_std_string(), true, mn->maxLocals, arena, reason);
    if (!ssa) return {};
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return {};
    RegCIntrinsicRegistry registry(users);
    RegCEmitter           emitter(nullptr, {}, nullptr, 0, nullptr, nullptr, nullptr, &registry);
    return emitter.emitFunction(*reg);
}

TEST(RegCUserIntrinsicTest, StaticCallSiteLowersToUserSymbol)
{
    std::string reason;
    auto        result = emitRegCWithUsers({makeUserEntry()}, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    // 直接调用用户 symbol，参数来自虚拟寄存器。
    EXPECT_NE(result.source.find("my_m("), std::string::npos) << result.source;
    // 不经过通用 JNI 调用 helper。
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32(\"com/x/F\""), std::string::npos)
        << result.source;
    // UserIntrinsic 恒保留 pending 异常检查（不做 pure 豁免）。
    EXPECT_NE(result.source.find("_mj2x_regc_exception_check"), std::string::npos)
        << result.source;
}

TEST(RegCUserIntrinsicTest, PassContextPrefixesEnv)
{
    std::string reason;
    auto        result = emitRegCWithUsers({makeUserEntry(true, false)}, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    // passContext：第一参数传当前上下文 env。
    EXPECT_NE(result.source.find("my_m(_mj2x_regc_current_context()->env"),
              std::string::npos)
        << result.source;
}

TEST(RegCUserIntrinsicTest, UnmatchedCallSiteStillUsesGenericDispatch)
{
    std::string reason;
    // 注册一个不匹配的条目（别的 owner），com/x/F.m 应回退通用 JNI helper。
    UserIntrinsic other = makeUserEntry();
    other.owner         = "other/pkg/K";
    auto        result  = emitRegCWithUsers({other}, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_EQ(result.source.find("my_m("), std::string::npos) << result.source;
    // 回退通用 JNI 派发（cached methodID + CallStaticIntMethodA）。
    EXPECT_NE(result.source.find("CallStaticIntMethodA"), std::string::npos) << result.source;
}

// ===========================================================================
// StackC
// ===========================================================================

TEST(StackCUserIntrinsicTest, StaticCallSiteLowersToDirectSymbolCall)
{
    StackCBackend backend;
    UserIntrinsicConfig cfg;
    cfg.intrinsics.push_back(makeUserEntry());
    backend.setUserIntrinsics(cfg);

    auto mn = makeUserCallMethod();
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);
    auto result = backend.compileMethod("com/x/F", cm, blocks);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;

    // 直接调用 symbol，参数从静态化后的栈槽弹出（arg0 在前），结果写回接收者槽。
    // 注：StackC 的 emitNodeWithExceptionDispatch 会把 stack[sp - N] 静态化为 s[N]。
    EXPECT_NE(result.text.find("= my_m(s[0].i, s[1].i)"), std::string::npos) << result.text;
    // 不经过 JNI 派发。
    EXPECT_EQ(result.text.find("CallStaticIntMethod"), std::string::npos) << result.text;
}

TEST(StackCUserIntrinsicTest, PassContextAndMayThrow)
{
    StackCBackend backend;
    UserIntrinsicConfig cfg;
    cfg.intrinsics.push_back(makeUserEntry(true, true));
    backend.setUserIntrinsics(cfg);

    auto mn = makeUserCallMethod();
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);
    auto result = backend.compileMethod("com/x/F", cm, blocks);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;

    // passContext：env 作为第一参数。
    EXPECT_NE(result.text.find("= my_m(env, s[0].i, s[1].i)"), std::string::npos)
        << result.text;
    // mayThrow：调用后保留 pending 异常检查。
    EXPECT_NE(result.text.find("ExceptionCheck(env)"), std::string::npos) << result.text;
}

TEST(StackCUserIntrinsicTest, UnmatchedCallSiteStillUsesJniDispatch)
{
    StackCBackend backend;
    UserIntrinsicConfig cfg;
    UserIntrinsic other = makeUserEntry();
    other.owner         = "other/pkg/K";
    cfg.intrinsics.push_back(other);
    backend.setUserIntrinsics(cfg);

    auto mn = makeUserCallMethod();
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);
    auto result = backend.compileMethod("com/x/F", cm, blocks);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;

    EXPECT_EQ(result.text.find("my_m("), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("CallStaticIntMethod"), std::string::npos) << result.text;
}

// ===========================================================================
// RegC backend 级全链路（诊断 cli 路径与 emitter 级一致）
// ===========================================================================

TEST(RegCUserIntrinsicTest, BackendCompileMethodCarriesUserIntrinsics)
{
    RegCBackend backend;
    UserIntrinsicConfig cfg;
    cfg.intrinsics.push_back(makeUserEntry());
    backend.setUserIntrinsics(cfg);
    backend.setOutputPath("unused.c");

    auto mn = makeUserCallMethod();
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);
    auto result = backend.compileMethod("com/x/F", cm, blocks);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("my_m("), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("CallStaticIntMethodA"), std::string::npos) << result.text;
}

// ===========================================================================
// VMC4
// ===========================================================================

std::unique_ptr<VmProgram> encodeWithUsers(const std::vector<UserIntrinsic>& users,
                                           std::string& reason)
{
    auto mn = makeUserCallMethod();
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "UserIntrinsicVmc4", mn->name.to_std_string(),
                             mn->desc.to_std_string(), true, mn->maxLocals, arena, reason);
    if (!ssa) return nullptr;
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return nullptr;
    eliminateDeadRegInstructions(*reg);
    Vmc4Options opts;
    VmInstance  instance(opts);
    return VmEncoder::encode(*reg, instance, reason, &users);
}

TEST(Vmc4UserIntrinsicTest, CallSiteRefCarriesUserSymbol)
{
    std::string reason;
    auto        prog = encodeWithUsers({makeUserEntry()}, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->methodRefs.size(), 1u);
    const auto& mref = prog->methodRefs[0];
    EXPECT_EQ(mref.userSymbol, "my_m");
    EXPECT_FALSE(mref.userPassContext);
    EXPECT_EQ(mref.invokeKind, VmInvokeKind::Static);
}

TEST(Vmc4UserIntrinsicTest, PassContextMarked)
{
    std::string reason;
    auto        prog = encodeWithUsers({makeUserEntry(true, false)}, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->methodRefs.size(), 1u);
    EXPECT_EQ(prog->methodRefs[0].userSymbol, "my_m");
    EXPECT_TRUE(prog->methodRefs[0].userPassContext);
}

TEST(Vmc4UserIntrinsicTest, UnmatchedCallSiteCarriesEmptyUserSymbol)
{
    std::string reason;
    UserIntrinsic other = makeUserEntry();
    other.owner         = "other/pkg/K";
    auto        prog    = encodeWithUsers({other}, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->methodRefs.size(), 1u);
    EXPECT_TRUE(prog->methodRefs[0].userSymbol.empty());
}

}   // namespace
