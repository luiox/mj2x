// 用户自定义 intrinsic（issue #818）三后端单测。
//
// 验证统一契约：用户通过 UserIntrinsicConfig 注册的 Java 调用点
// （owner.name:desc）在 StackC / RegC / VMC4 里都降级为对用户
// symbol 的直接调用，而不是通用 JNI 派发：
//   - RegC：emitIntrinsicCall 走 UserIntrinsic emitKind，保留 ExceptionCheck；
//   - StackC：emitInvoke 弹出 stack[] 参数直接调 symbol；
//   - VMC4：encodeCall 标记 VmMethodRef.userSymbol，helper 解包 regs 直调。
// passReceiver 扩面（instance intrinsic）：instance 调用点仅匹配显式声明
// passReceiver=true 的条目，receiver 以各后端 REF 值原样直传（参数序
// [env,] receiver, <desc 参数...>）；static 调用点匹配到 passReceiver 条目
// 报配置校验错误 fail-closed；instance 调用点未声明 passReceiver 不匹配
// （G-1 行为保持）。
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
#include "mj2x/backend/vmc4/vm_interpreter_emit.hpp"
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
UserIntrinsic makeUserEntry(bool passContext = false, bool mayThrow = false,
                            bool passReceiver = false)
{
    UserIntrinsic u;
    u.owner        = "com/x/F";
    u.name         = "m";
    u.desc         = "(II)I";
    u.symbol       = "my_m";
    u.passContext  = passContext;
    u.passReceiver = passReceiver;
    u.mayThrow     = mayThrow;
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

/// com/x/F.m(II)I 实例调用（int call(F f, int a, int b) { return f.m(a, b); }）——
/// owner/name/desc 与用户条目同键，用于锁定 instance 调用不进用户 intrinsic 路径。
std::unique_ptr<MethodNode> makeInstanceCallMethod()
{
    return makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "instanceCall", "(Lcom/x/F;II)I",
                      {
                          new VarInsnNode(Opcodes::ALOAD, 1),
                          new VarInsnNode(Opcodes::ILOAD, 2),
                          new VarInsnNode(Opcodes::ILOAD, 3),
                          new MethodInsnNode(
                              Opcodes::INVOKEVIRTUAL, U("com/x/F"), U("m"), U("(II)I"), false),
                          new InsnNode(Opcodes::IRETURN),
                          },
                      3, 4);
}

RegCEmitResult emitRegCInstanceCallWithUsers(const std::vector<UserIntrinsic>& users,
                                             std::string&                     reason)
{
    auto mn = makeInstanceCallMethod();
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*mn);
    SSABuilder               builder;
    auto ssa = builder.build(blocks, "UserIntrinsicRegC", mn->name.to_std_string(),
                             mn->desc.to_std_string(), false, mn->maxLocals, arena, reason);
    if (!ssa) return {};
    RegLowering lowering;
    auto        reg = lowering.lower(*ssa, reason);
    if (!reg) return {};
    RegCIntrinsicRegistry registry(users);
    RegCEmitter           emitter(nullptr, {}, nullptr, 0, nullptr, nullptr, nullptr, &registry);
    return emitter.emitFunction(*reg);
}

TEST(RegCUserIntrinsicTest, InstanceCallSiteNeverEntersUserIntrinsicPath)
{
    std::string reason;
    // 注册同 owner/name/desc 的条目；instance 调用点按契约不降级。回归锁：
    // 修复前 matchUserCall 缺 isStatic 门控，argCount（含 receiver=3） !=
    // 形参数（2）被判 Unsupported，整方法 fail-closed（result.ok == false）。
    auto result = emitRegCInstanceCallWithUsers({makeUserEntry()}, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    // 不调用用户 symbol。
    EXPECT_EQ(result.source.find("my_m("), std::string::npos) << result.source;
    // 走通用 JNI 派发 helper（虚分派 receiver 经 varargs 传入）。
    EXPECT_NE(result.source.find("_mj2x_regc_call_i32("), std::string::npos) << result.source;
}

TEST(RegCUserIntrinsicTest, InstanceCallSiteWithPassReceiverLowersToUserSymbol)
{
    std::string reason;
    // passReceiver=true：instance 调用点降级为用户 symbol，RegIR args[0]（receiver）
    // 作为第一参数直传；恒保留 ExceptionCheck（不做 pure 豁免）。
    auto result =
        emitRegCInstanceCallWithUsers({makeUserEntry(false, false, true)}, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("my_m("), std::string::npos) << result.source;
    // 不经通用 JNI 派发。
    EXPECT_EQ(result.source.find("_mj2x_regc_call_i32("), std::string::npos) << result.source;
    // JVM 语义保持：receiver 为 NULL 抛 NPE（与通用派发 emitCachedCall 一致）。
    EXPECT_NE(result.source.find("_mj2x_regc_throw_null(\"instance method call\")"),
              std::string::npos)
        << result.source;
    // 用户实现可能经 JNI 抛异常：恒保留 pending 异常检查。
    EXPECT_NE(result.source.find("_mj2x_regc_exception_check"), std::string::npos)
        << result.source;
}

TEST(RegCUserIntrinsicTest, InstanceCallSitePassReceiverAndPassContextOrdersEnvFirst)
{
    std::string reason;
    // passContext + passReceiver 组合：env 最前，receiver 次之，desc 参数最后。
    auto result =
        emitRegCInstanceCallWithUsers({makeUserEntry(true, false, true)}, reason);
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_NE(result.source.find("my_m(_mj2x_regc_current_context()->env,"), std::string::npos)
        << result.source;
}

TEST(RegCUserIntrinsicTest, StaticCallSiteWithPassReceiverEntryFailsClosed)
{
    std::string reason;
    // 配置校验：passReceiver 条目匹配到 INVOKESTATIC 调用点（静态方法无
    // receiver）——Unsupported 使整方法 fail-closed，不静默回退通用派发。
    auto result = emitRegCWithUsers({makeUserEntry(false, false, true)}, reason);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.reason.find("passReceiver"), std::string::npos) << result.reason;
}

TEST(RegCUserIntrinsicTest, DeclarationsIncludeReceiverParamForPassReceiverEntries)
{
    // extern 声明契约：passContext 在前、receiver 次之（mj2x_ref）、desc 参数最后。
    UserIntrinsic withEnv = makeUserEntry(true, false, true);
    withEnv.symbol        = "my_m_env";
    RegCIntrinsicRegistry registry({makeUserEntry(false, false, true), withEnv});
    ca::str::Utf8StringBuilder out;
    registry.emitDeclarations(out);
    const std::string text = out.build().to_std_string();
    EXPECT_NE(text.find("extern int32_t my_m(mj2x_ref receiver, int32_t, int32_t);"),
              std::string::npos)
        << text;
    EXPECT_NE(
        text.find("extern int32_t my_m_env(JNIEnv* env, mj2x_ref receiver, int32_t, int32_t);"),
        std::string::npos)
        << text;
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

namespace
{

/// StackC instance 调用点编译（com/x/F.m(II)I 经 INVOKEVIRTUAL 调用）。
BackendResult emitStackCInstance(const std::vector<UserIntrinsic>& users)
{
    StackCBackend backend;
    UserIntrinsicConfig cfg;
    cfg.intrinsics = users;
    backend.setUserIntrinsics(cfg);

    auto mn = makeInstanceCallMethod();
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);
    return backend.compileMethod("com/x/F", cm, blocks);
}

}   // namespace

TEST(StackCUserIntrinsicTest, InstanceCallSiteWithPassReceiverLowersToDirectSymbolCall)
{
    // passReceiver=true：receiver 在参数区之下第一槽（本 fixture 即 s[0].l），
    // 作为第一参数直传；desc 参数随后；结果写回 receiver 槽，sp 净弹 2。
    auto result = emitStackCInstance({makeUserEntry(false, false, true)});
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("= my_m(s[0].l, s[1].i, s[2].i)"), std::string::npos)
        << result.text;
    // JVM 语义保持：调用前 null receiver NPE trap（与通用 JNI 派发一致）。
    EXPECT_NE(result.text.find("NullPointerException"), std::string::npos) << result.text;
    // 不经 JNI 派发。
    EXPECT_EQ(result.text.find("CallIntMethod(env, lc, mid_local"), std::string::npos) << result.text;
}

TEST(StackCUserIntrinsicTest, InstanceCallSiteWithoutPassReceiverStillJniDispatch)
{
    // G-1 行为保持：instance 调用点遇到未声明 passReceiver 的条目不匹配，
    // 回通用 JNI 虚分派（StackC 通用面为 Call<Int>Method 短名 + mid_local 缓存）。
    auto result = emitStackCInstance({makeUserEntry()});
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_EQ(result.text.find("my_m("), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("CallIntMethod(env, lc, mid_local"), std::string::npos)
        << result.text;
}

TEST(StackCUserIntrinsicTest, StaticCallSiteWithPassReceiverEntryFailsClosed)
{
    // 配置校验：passReceiver 条目匹配到静态调用点（静态调用无 receiver）——
    // 整方法 fail-closed 保留 Java 字节码，不静默回退。
    StackCBackend backend;
    UserIntrinsicConfig cfg;
    cfg.intrinsics.push_back(makeUserEntry(false, false, true));
    backend.setUserIntrinsics(cfg);

    auto mn = makeUserCallMethod();
    CompilerMethod cm;
    cm.methodNode       = mn.get();
    cm.markedForCompile = true;

    ca::str::Utf8StringArena arena;
    CodeConverter            cc(arena);
    auto                     blocks = cc.convert(*mn);
    auto result = backend.compileMethod("com/x/F", cm, blocks);
    EXPECT_EQ(result.kind, BackendResult::Kind::NONE);
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

/// VMC4 instance 调用点编码（com/x/F.m(II)I 经 INVOKEVIRTUAL 调用）。
std::unique_ptr<VmProgram> encodeInstanceWithUsers(const std::vector<UserIntrinsic>& users,
                                                   std::string&                      reason)
{
    auto mn = makeInstanceCallMethod();
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

TEST(Vmc4UserIntrinsicTest, InstanceCallSiteCarriesUserSymbolAndReceiver)
{
    std::string reason;
    // passReceiver=true：instance 调用点匹配，VmMethodRef 标记 userSymbol +
    // userPassReceiver，invokeKind 保持 Virtual（receiver 经 regs[recv] 传入）。
    auto prog = encodeInstanceWithUsers({makeUserEntry(false, false, true)}, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->methodRefs.size(), 1u);
    const auto& mref = prog->methodRefs[0];
    EXPECT_EQ(mref.userSymbol, "my_m");
    EXPECT_TRUE(mref.userPassReceiver);
    EXPECT_EQ(mref.invokeKind, VmInvokeKind::Virtual);
}

TEST(Vmc4UserIntrinsicTest, InstanceCallSiteWithoutPassReceiverUnmarked)
{
    std::string reason;
    // G-1 行为保持：instance 调用点遇到未声明 passReceiver 的条目不匹配，
    // 走通用 JNI helper。
    auto prog = encodeInstanceWithUsers({makeUserEntry()}, reason);
    ASSERT_NE(prog, nullptr) << reason;
    ASSERT_EQ(prog->methodRefs.size(), 1u);
    EXPECT_TRUE(prog->methodRefs[0].userSymbol.empty());
    EXPECT_EQ(prog->methodRefs[0].invokeKind, VmInvokeKind::Virtual);
}

TEST(Vmc4UserIntrinsicTest, StaticCallSiteWithPassReceiverEntryEncodeFails)
{
    std::string reason;
    // 配置校验：passReceiver 条目匹配到静态调用点（静态方法无 receiver）——
    // 编码失败，reason 指明 passReceiver 配置错误。
    auto prog = encodeWithUsers({makeUserEntry(false, false, true)}, reason);
    EXPECT_EQ(prog, nullptr);
    EXPECT_NE(reason.find("passReceiver"), std::string::npos) << reason;
}

TEST(Vmc4UserIntrinsicTest, InterpreterHelperPassesReceiverSlot)
{
    std::string reason;
    // 解释器 helper：userPassReceiver 时 receiver 以 regs[recv].ref 直传
    // （env 后第一参数），并保留 null receiver NPE 守卫。
    auto prog = encodeInstanceWithUsers({makeUserEntry(false, false, true)}, reason);
    ASSERT_NE(prog, nullptr) << reason;
    const std::string src = VmInterpreterEmitter::emit(*prog, VmInstance(Vmc4Options{}), "t");
    EXPECT_NE(src.find("regs[recv].ref"), std::string::npos) << src;
    EXPECT_NE(src.find("mj2x_vm_throw_npe(env, \"instance method call\")"), std::string::npos)
        << src;
}

}   // namespace
