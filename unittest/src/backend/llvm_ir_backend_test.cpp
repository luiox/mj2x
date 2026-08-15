#include <gmock/gmock.h>

#include <memory>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/llvmir/llvm_ir_backend.hpp"
#include "mj2x/frontend/code_converter.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

std::unique_ptr<MethodNode> makeMethod(int access, const char* name, const char* desc,
                                       std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                                       int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto method = std::make_unique<MethodNode>(access,
                                               ca::str::ZUtf8StringRef::from_static(name),
                                               ca::str::ZUtf8StringRef::from_static(desc),
                                               ca::str::ZUtf8StringRef::from_static(""),
                                               noExceptions);
    method->maxStack  = maxStack;
    method->maxLocals = maxLocals;
    for (auto* instruction : insns) method->instructions.add(instruction);
    return method;
}

}   // namespace

TEST(LlvmIrBackendTest, EmitsScalarJniWrapperAndMarksMethodNative)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "add",
                             "(II)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2,
                             2);

    CompilerMethod compilerMethod;
    compilerMethod.methodNode = method.get();
    CompilerClass compilerClass;
    compilerClass.methods.push_back(compilerMethod);

    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto blocks = converter.convert(*method);
    auto result = backend.compileMethod("Test", compilerClass.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("define internal i32 @mj2x_llvm_Java_Test_add__II(ptr %env, i32 %p0, i32 %p1)"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("define dllexport i32 @Java_Test_add__II(ptr %env, ptr %clazz, i32 %p0, i32 %p1)"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("add i32"), std::string::npos) << result.text;

    backend.injectIntoClass(compilerClass, compilerClass.methods[0], result);
    EXPECT_TRUE(method->access & Opcodes::ACC_NATIVE);
    EXPECT_EQ(method->instructions.size(), 0);
    EXPECT_TRUE(compilerClass.modified);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto artifact = backend.generatedSourceArtifact();
    EXPECT_EQ(artifact.language, NativeSourceLanguage::LlvmIr);
    EXPECT_EQ(artifact.suggestedFileName, "native.ll");
    EXPECT_EQ(artifact.content, backend.generatedSource());
}

TEST(LlvmIrBackendTest, KeepsUnsupportedMethodBytecode)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC,
                             "instanceMethod",
                             "()I",
                             {
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1,
                             1);

    CompilerMethod compilerMethod;
    compilerMethod.methodNode = method.get();
    CompilerClass compilerClass;
    compilerClass.methods.push_back(compilerMethod);

    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto blocks = converter.convert(*method);
    auto result = backend.compileMethod("Test", compilerClass.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::NONE);
    EXPECT_NE(result.text.find("static methods only"), std::string::npos) << result.text;

    backend.injectIntoClass(compilerClass, compilerClass.methods[0], result);
    EXPECT_FALSE(method->access & Opcodes::ACC_NATIVE);
    EXPECT_GT(method->instructions.size(), 0);
    EXPECT_FALSE(compilerClass.modified);
}

TEST(LlvmIrBackendTest, KeepsEffectfulAndThrowingMethodsBytecode)
{
    auto objectMethod = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                   "allocates",
                                   "()I",
                                   {
                                       new TypeInsnNode(
                                           Opcodes::NEW,
                                           ca::str::ZUtf8StringRef::from_static("java/lang/Object")),
                                       new InsnNode(Opcodes::DUP),
                                       new MethodInsnNode(
                                           Opcodes::INVOKESPECIAL,
                                           ca::str::ZUtf8StringRef::from_static("java/lang/Object"),
                                           ca::str::ZUtf8StringRef::from_static("<init>"),
                                           ca::str::ZUtf8StringRef::from_static("()V"),
                                           false),
                                       new InsnNode(Opcodes::POP),
                                       new InsnNode(Opcodes::ICONST_1),
                                       new InsnNode(Opcodes::IRETURN),
                                   },
                                   2,
                                   0);

    CompilerMethod objectCompilerMethod;
    objectCompilerMethod.methodNode = objectMethod.get();
    CompilerClass compilerClass;
    compilerClass.methods.push_back(objectCompilerMethod);

    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            objectConverter(arena);
    auto objectResult = backend.compileMethod(
        "Test", compilerClass.methods[0], objectConverter.convert(*objectMethod));

    ASSERT_EQ(objectResult.kind, BackendResult::Kind::NONE);
    // NEW 已放行；invokespecial <init> 在实例调用实现前 fail-closed。
    EXPECT_NE(objectResult.text.find("static calls require a compiler index"),
              std::string::npos)
        << objectResult.text;

    backend.injectIntoClass(compilerClass, compilerClass.methods[0], objectResult);
    EXPECT_FALSE(objectMethod->access & Opcodes::ACC_NATIVE);
    EXPECT_GT(objectMethod->instructions.size(), 0);
    EXPECT_FALSE(compilerClass.modified);
}

/// @brief 编译单个静态方法并返回 BackendResult（字节码 → LLVM IR 切片）。
BackendResult compileSingle(const char* name, const char* desc,
                            std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                            int maxLocals)
{
    auto        method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns,
                                    maxStack, maxLocals);
    CompilerClass clazz;
    clazz.methods.push_back(CompilerMethod{method.get(), true, false, {}});
    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*method);
    return backend.compileMethod("Test", clazz.methods[0], blocks);
}

TEST(LlvmIrBackendTest, EmitsShiftOperationsWithJavaModuloSemantics)
{
    // Java 移位量取低 5/6 位；LLVM shl/ashr/lshr 对移位量自动取模，语义一致。
    auto compile = [](int opcode, const char* expect) {
        auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 "shift",
                                 "(II)I",
                                 {
                                     new VarInsnNode(Opcodes::ILOAD, 0),
                                     new VarInsnNode(Opcodes::ILOAD, 1),
                                     new InsnNode(opcode),
                                     new InsnNode(Opcodes::IRETURN),
                                 },
                                 2,
                                 2);
        CompilerClass clazz;
        clazz.methods.push_back(CompilerMethod{method.get(), true, false, {}});
        LlvmIrBackend backend;
        ca::str::Utf8StringArena arena;
        CodeConverter            converter(arena);
        auto                     blocks = converter.convert(*method);
        auto result = backend.compileMethod("Test", clazz.methods[0], blocks);
        ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
        EXPECT_NE(result.text.find(expect), std::string::npos) << result.text;
    };
    compile(Opcodes::ISHL, "shl i32");
    compile(Opcodes::ISHR, "ashr i32");
    compile(Opcodes::IUSHR, "lshr i32");

    // long 移位（i64）。
    auto lmethod = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                              "lshift",
                              "(JJ)J",
                              {
                                  new VarInsnNode(Opcodes::LLOAD, 0),
                                  new VarInsnNode(Opcodes::LLOAD, 2),
                                  new InsnNode(Opcodes::LSHL),
                                  new InsnNode(Opcodes::LRETURN),
                              },
                              4,
                              4);
    CompilerClass clazz;
    clazz.methods.push_back(CompilerMethod{lmethod.get(), true, false, {}});
    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*lmethod);
    auto result = backend.compileMethod("Test", clazz.methods[0], blocks);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("shl i64"), std::string::npos) << result.text;
}

namespace {

/// @brief 构造带两个静态方法（add / caller 调 add）的 CompilerIndex。
CompilerIndex makeCallerIndex(std::unique_ptr<MethodNode>& add,
                              std::unique_ptr<MethodNode>& caller)
{
    add    = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "add", "(II)I",
                        {new VarInsnNode(Opcodes::ILOAD, 0), new VarInsnNode(Opcodes::ILOAD, 1),
                         new InsnNode(Opcodes::IADD), new InsnNode(Opcodes::IRETURN)},
                        2, 2);
    caller = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "caller", "(II)I",
                        {new VarInsnNode(Opcodes::ILOAD, 0), new VarInsnNode(Opcodes::ILOAD, 1),
                         new MethodInsnNode(Opcodes::INVOKESTATIC,
                                            ca::str::ZUtf8StringRef::from_static("Test"),
                                            ca::str::ZUtf8StringRef::from_static("add"),
                                            ca::str::ZUtf8StringRef::from_static("(II)I"),
                                            false),
                         new InsnNode(Opcodes::IRETURN)},
                        2, 2);
    CompilerIndex index;
    CompilerClass clazz;
    clazz.methods.push_back(CompilerMethod{add.get(), true, false, {}});
    clazz.methods.push_back(CompilerMethod{caller.get(), true, false, {}});
    index.addClass("Test", std::move(clazz));
    return index;
}

}   // namespace

TEST(LlvmIrBackendTest, EmitsIntraClassStaticCall)
{
    std::unique_ptr<MethodNode> add;
    std::unique_ptr<MethodNode> caller;
    CompilerIndex               index = makeCallerIndex(add, caller);

    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*caller);
    auto result = backend.compileMethod("Test", index.getClass("Test")->methods[1], blocks, &index);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("define internal i32 @mj2x_llvm_Java_Test_caller__II(ptr %env, i32 %p0, i32 %p1)"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("call i32 @mj2x_llvm_Java_Test_add__II(ptr %env, i32 %v0, i32 %v1)"),
              std::string::npos)
        << result.text;
}

TEST(LlvmIrBackendTest, KeepsBytecodeWhenCallTargetIsNotConvertible)
{
    // caller 调 target，target 含对象分配 → target 不可转换 → caller fail-closed。
    std::unique_ptr<MethodNode> target;
    std::unique_ptr<MethodNode> caller;
    target = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "target", "()I",
                        {new TypeInsnNode(Opcodes::NEW,
                                          ca::str::ZUtf8StringRef::from_static("java/lang/Object")),
                         new InsnNode(Opcodes::DUP),
                         new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                            ca::str::ZUtf8StringRef::from_static("java/lang/Object"),
                                            ca::str::ZUtf8StringRef::from_static("<init>"),
                                            ca::str::ZUtf8StringRef::from_static("()V"),
                                            false),
                         new InsnNode(Opcodes::POP), new InsnNode(Opcodes::ICONST_1),
                         new InsnNode(Opcodes::IRETURN)},
                        2, 0);
    caller = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "caller", "()I",
                        {new MethodInsnNode(Opcodes::INVOKESTATIC,
                                            ca::str::ZUtf8StringRef::from_static("Test"),
                                            ca::str::ZUtf8StringRef::from_static("target"),
                                            ca::str::ZUtf8StringRef::from_static("()I"),
                                            false),
                         new InsnNode(Opcodes::IRETURN)},
                        1, 0);
    CompilerIndex index;
    CompilerClass clazz;
    clazz.methods.push_back(CompilerMethod{target.get(), true, false, {}});
    clazz.methods.push_back(CompilerMethod{caller.get(), true, false, {}});
    index.addClass("Test", std::move(clazz));

    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*caller);
    auto result = backend.compileMethod("Test", index.getClass("Test")->methods[1], blocks, &index);

    ASSERT_EQ(result.kind, BackendResult::Kind::NONE);
    // target 含 NEW/DUP/INVOKESPECIAL：NEW 已放行，invokespecial 在实例调用
    // 实现前 fail-closed，调用方保持字节码。
    EXPECT_NE(result.text.find("only supports static method calls"), std::string::npos)
        << result.text;
}

TEST(LlvmIrBackendTest, KeepsBytecodeForMissingClassAndIndexlessCalls)
{
    // 跨类调用：目标类不在编译索引 → fail-closed。
    auto crossClass = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "cross", "()I",
        {new MethodInsnNode(Opcodes::INVOKESTATIC,
                            ca::str::ZUtf8StringRef::from_static("Other"),
                            ca::str::ZUtf8StringRef::from_static("add"),
                            ca::str::ZUtf8StringRef::from_static("()I"),
                            false),
         new InsnNode(Opcodes::IRETURN)},
        1, 0);

    CompilerIndex index;
    CompilerClass clazz;
    clazz.methods.push_back(CompilerMethod{crossClass.get(), true, false, {}});
    index.addClass("Test", std::move(clazz));

    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);

    // 无 index（单元测试直连路径）时的静态调用 → fail-closed。
    auto indexless = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "indexless", "()I",
        {new MethodInsnNode(Opcodes::INVOKESTATIC,
                            ca::str::ZUtf8StringRef::from_static("Test"),
                            ca::str::ZUtf8StringRef::from_static("add"),
                            ca::str::ZUtf8StringRef::from_static("()I"),
                            false),
         new InsnNode(Opcodes::IRETURN)},
        1, 0);
    CompilerClass indexlessClazz;
    indexlessClazz.methods.push_back(CompilerMethod{indexless.get(), true, false, {}});
    LlvmIrBackend indexlessBackend;
    auto          indexlessBlocks = converter.convert(*indexless);
    auto          indexlessResult =
        indexlessBackend.compileMethod("Test", indexlessClazz.methods[0], indexlessBlocks, nullptr);
    ASSERT_EQ(indexlessResult.kind, BackendResult::Kind::NONE);
    EXPECT_NE(indexlessResult.text.find("compiler index"), std::string::npos)
        << indexlessResult.text;

    // 跨类（带 index，目标类缺失）→ fail-closed。
    auto crossResult = backend.compileMethod("Test", index.getClass("Test")->methods[0],
                                             converter.convert(*crossClass), &index);
    EXPECT_EQ(crossResult.kind, BackendResult::Kind::NONE);
    EXPECT_NE(crossResult.text.find("not in the compilation index"), std::string::npos)
        << crossResult.text;
}

TEST(LlvmIrBackendTest, EmitsRecursiveStaticCall)
{
    // 自递归调用：符号确定性生成，直接递归可转换（发射形态验证）。
    auto recursive = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "recursive", "()I",
        {new MethodInsnNode(Opcodes::INVOKESTATIC,
                            ca::str::ZUtf8StringRef::from_static("Test"),
                            ca::str::ZUtf8StringRef::from_static("recursive"),
                            ca::str::ZUtf8StringRef::from_static("()I"),
                            false),
         new InsnNode(Opcodes::IRETURN)},
        1, 0);

    CompilerIndex index;
    CompilerClass clazz;
    clazz.methods.push_back(CompilerMethod{recursive.get(), true, false, {}});
    index.addClass("Test", std::move(clazz));

    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto blocks = converter.convert(*recursive);
    auto result = backend.compileMethod("Test", index.getClass("Test")->methods[0], blocks, &index);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("call i32 @mj2x_llvm_Java_Test_recursive(ptr %env)"),
              std::string::npos)
        << result.text;
}

/// @brief 编译单个静态方法（无 index；不涉及调用时可用）。
BackendResult compileScalar(const char* name, const char* desc,
                            std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                            int maxLocals)
{
    auto          method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, name, desc, insns,
                                      maxStack, maxLocals);
    CompilerClass clazz;
    clazz.methods.push_back(CompilerMethod{method.get(), true, false, {}});
    LlvmIrBackend backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto blocks = converter.convert(*method);
    return backend.compileMethod("Test", clazz.methods[0], blocks);
}

TEST(LlvmIrBackendTest, EmitsIntegerDivisionWithZeroCheck)
{
    // (int a, int b) -> a / b：udiv（位模式等价、无 INT_MIN/-1 溢出 poison）
    // + 除零检查 → 共享 throw-arithmetic helper。
    auto result = compileScalar("divide", "(II)I",
                                {new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1), new InsnNode(Opcodes::IDIV),
                                 new InsnNode(Opcodes::IRETURN)},
                                2, 2);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("icmp eq i32"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("udiv i32"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("call void @mj2x_llvm_throw_arith(ptr %env)"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("ret i32 0"), std::string::npos) << result.text;
}

TEST(LlvmIrBackendTest, EmitsIntegerRemainderWithZeroCheck)
{
    // (long a, long b) -> a % b：urem + 除零检查。
    auto result = compileScalar("remainder", "(JJ)J",
                                {new VarInsnNode(Opcodes::LLOAD, 0),
                                 new VarInsnNode(Opcodes::LLOAD, 2), new InsnNode(Opcodes::LREM),
                                 new InsnNode(Opcodes::LRETURN)},
                                4, 4);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("icmp eq i64"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("urem i64"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("ret i64 0"), std::string::npos) << result.text;
}

TEST(LlvmIrBackendTest, EmitsSaturatingFloatToIntConversion)
{
    // f2i：llvm.fptosi.sat（NaN→0、溢出饱和 = Java 语义）。
    auto f2i = compileScalar("f2i", "(F)I",
                             {new VarInsnNode(Opcodes::FLOAD, 0), new InsnNode(Opcodes::F2I),
                              new InsnNode(Opcodes::IRETURN)},
                             1, 1);
    ASSERT_EQ(f2i.kind, BackendResult::Kind::C_SOURCE) << f2i.text;
    EXPECT_NE(f2i.text.find("call i32 @llvm.fptosi.sat.i32.f32(float"), std::string::npos)
        << f2i.text;

    // d2l：llvm.fptosi.sat.i64.f64。
    auto d2l = compileScalar("d2l", "(D)J",
                             {new VarInsnNode(Opcodes::DLOAD, 0), new InsnNode(Opcodes::D2L),
                              new InsnNode(Opcodes::LRETURN)},
                             2, 2);
    ASSERT_EQ(d2l.kind, BackendResult::Kind::C_SOURCE) << d2l.text;
    EXPECT_NE(d2l.text.find("call i64 @llvm.fptosi.sat.i64.f64(double"), std::string::npos)
        << d2l.text;
}

TEST(LlvmIrBackendTest, EmitsNarrowSignatureBoundaryConversions)
{
    // (byte, short, char, boolean) -> short：
    // wrapper 收 jbyte/jshort/jchar/jboolean（i8/i16），提升到 i32 计算位宽
    // （byte/short 符号扩展、char/boolean 零扩展），返回 trunc 到 i16。
    auto result = compileScalar("narrow",
                                "(BSZC)S",
                                {new VarInsnNode(Opcodes::ILOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new VarInsnNode(Opcodes::ILOAD, 2),
                                 new VarInsnNode(Opcodes::ILOAD, 3),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN)},
                                4, 4);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    // wrapper 边界：窄参数类型 + 提升 + trunc 返回。
    EXPECT_NE(result.text.find("define dllexport i16 @Java_Test_narrow__BSZC(ptr %env, ptr %clazz, i8 %p0, i16 %p1, i8 %p2, i16 %p3)"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("%w0 = sext i8 %p0 to i32"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("%w1 = sext i16 %p1 to i32"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("%w2 = zext i8 %p2 to i32"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("%w3 = zext i16 %p3 to i32"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("call i32 @mj2x_llvm_Java_Test_narrow__BSZC(ptr %env, i32 %w0, i32 %w1, i32 %w2, i32 %w3)"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("%t = trunc i32 %result to i16"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("ret i16 %t"), std::string::npos) << result.text;
}

TEST(LlvmIrBackendTest, EmitsSpecialFloatLiterals)
{
    // NaN / ±Inf / -0.0 常量以 LLVM hex 字面量发射（十进制无法表示）。
    auto nan = compileScalar("nanf", "()F",
                             {new LdcInsnNode(std::numeric_limits<float>::quiet_NaN()),
                              new InsnNode(Opcodes::FRETURN)},
                             1, 0);
    ASSERT_EQ(nan.kind, BackendResult::Kind::C_SOURCE) << nan.text;
    EXPECT_NE(nan.text.find("0x7FC00000"), std::string::npos) << nan.text;

    auto pinf = compileScalar("pinf", "()D",
                              {new LdcInsnNode(std::numeric_limits<double>::infinity()),
                               new InsnNode(Opcodes::DRETURN)},
                              2, 0);
    ASSERT_EQ(pinf.kind, BackendResult::Kind::C_SOURCE) << pinf.text;
    EXPECT_NE(pinf.text.find("0x7FF0000000000000"), std::string::npos) << pinf.text;

    auto ninf = compileScalar("ninf", "()D",
                              {new LdcInsnNode(-std::numeric_limits<double>::infinity()),
                               new InsnNode(Opcodes::DRETURN)},
                              2, 0);
    ASSERT_EQ(ninf.kind, BackendResult::Kind::C_SOURCE) << ninf.text;
    EXPECT_NE(ninf.text.find("0xFFF0000000000000"), std::string::npos) << ninf.text;

    auto nzero = compileScalar("nzero", "()D",
                               {new LdcInsnNode(-0.0), new InsnNode(Opcodes::DRETURN)}, 2, 0);
    ASSERT_EQ(nzero.kind, BackendResult::Kind::C_SOURCE) << nzero.text;
    EXPECT_NE(nzero.text.find("0x8000000000000000"), std::string::npos) << nzero.text;
}

TEST(LlvmIrBackendTest, EmitsStaticFieldAccess)
{
    // putstatic + getstatic（i32）：JNI 字段 helper 发射 + 全局字符串。
    auto result = compileScalar("field", "()I",
                                {new InsnNode(Opcodes::ICONST_5),
                                 new FieldInsnNode(Opcodes::PUTSTATIC,
                                                   ca::str::Utf8StringRef::from_cstr("Test"),
                                                   ca::str::Utf8StringRef::from_cstr("sink"),
                                                   ca::str::Utf8StringRef::from_cstr("I")),
                                 new FieldInsnNode(Opcodes::GETSTATIC,
                                                   ca::str::Utf8StringRef::from_cstr("Test"),
                                                   ca::str::Utf8StringRef::from_cstr("sink"),
                                                   ca::str::Utf8StringRef::from_cstr("I")),
                                 new InsnNode(Opcodes::IRETURN)},
                                1, 0);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("call void @mj2x_llvm_field_store_int(ptr %env, ptr null, ptr @_mj2x_llvm_str_0, ptr @_mj2x_llvm_str_1, ptr @_mj2x_llvm_str_2, i32 1, i32"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("call i32 @mj2x_llvm_field_load_int(ptr %env, ptr null, ptr @_mj2x_llvm_str_0, ptr @_mj2x_llvm_str_1, ptr @_mj2x_llvm_str_2, i32 1)"),
              std::string::npos)
        << result.text;
}

TEST(LlvmIrBackendTest, EmitsReferenceBoundary)
{
    // 引用参数/返回（wrapper ptr 边界）。
    auto result = compileScalar("id", "(Ljava/lang/String;)Ljava/lang/String;",
                                {new VarInsnNode(Opcodes::ALOAD, 0), new InsnNode(Opcodes::ARETURN)},
                                1, 1);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("define internal ptr @mj2x_llvm_Java_Test_id__Ljava_lang_String_2(ptr %env, ptr %p0)"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("define dllexport ptr @Java_Test_id__Ljava_lang_String_2(ptr %env, ptr %clazz, ptr %p0)"),
              std::string::npos)
        << result.text;
}

TEST(LlvmIrBackendTest, EmitsObjectAllocationAndTypeChecks)
{
    // NEW（不调 <init> 的畸形形态，仅验证分配发射）+ checkcast + instanceof。
    auto alloc = compileScalar("make", "()Ljava/lang/Object;",
                               {new TypeInsnNode(Opcodes::NEW,
                                                 ca::str::Utf8StringRef::from_cstr("java/lang/Object")),
                                new InsnNode(Opcodes::ARETURN)},
                               1, 0);
    ASSERT_EQ(alloc.kind, BackendResult::Kind::C_SOURCE) << alloc.text;
    EXPECT_NE(alloc.text.find("call ptr @mj2x_llvm_new_object(ptr %env, ptr @_mj2x_llvm_str_0)"),
              std::string::npos)
        << alloc.text;
    EXPECT_NE(alloc.text.find("ret ptr"), std::string::npos) << alloc.text;

    // checkcast：(Object) → (String)。
    auto cast = compileScalar("cast", "(Ljava/lang/Object;)Ljava/lang/String;",
                              {new VarInsnNode(Opcodes::ALOAD, 0),
                               new TypeInsnNode(Opcodes::CHECKCAST,
                                                ca::str::Utf8StringRef::from_cstr("java/lang/String")),
                               new InsnNode(Opcodes::ARETURN)},
                              1, 1);
    ASSERT_EQ(cast.kind, BackendResult::Kind::C_SOURCE) << cast.text;
    EXPECT_NE(cast.text.find("call ptr @mj2x_llvm_check_cast(ptr %env, %v0, ptr @_mj2x_llvm_str_0)"),
              std::string::npos)
        << cast.text;

    // instanceof：obj instanceof String。
    auto instance = compileScalar("is", "(Ljava/lang/Object;)Z",
                                  {new VarInsnNode(Opcodes::ALOAD, 0),
                                   new TypeInsnNode(Opcodes::INSTANCEOF,
                                                    ca::str::Utf8StringRef::from_cstr("java/lang/String")),
                                   new InsnNode(Opcodes::IRETURN)},
                                  1, 1);
    ASSERT_EQ(instance.kind, BackendResult::Kind::C_SOURCE) << instance.text;
    EXPECT_NE(instance.text.find("call i32 @mj2x_llvm_instance_of(ptr %env, %v0, ptr @_mj2x_llvm_str_0)"),
              std::string::npos)
        << instance.text;
}

TEST(LlvmIrBackendTest, EmitsArrayAccessAndLength)
{
    // IALOAD：数组 + 索引 → array_load_int helper。
    auto load = compileScalar("at", "([II)I",
                              {new VarInsnNode(Opcodes::ALOAD, 0), new VarInsnNode(Opcodes::ILOAD, 1),
                               new InsnNode(Opcodes::IALOAD), new InsnNode(Opcodes::IRETURN)},
                              2, 2);
    ASSERT_EQ(load.kind, BackendResult::Kind::C_SOURCE) << load.text;
    EXPECT_NE(load.text.find("call i32 @mj2x_llvm_array_load_int(ptr %env, ptr %v0, i32 %v1)"),
              std::string::npos)
        << load.text;

    // IASTORE：数组 + 索引 + 值 → array_store_int helper。
    auto store = compileScalar("put", "([III)V",
                               {new VarInsnNode(Opcodes::ALOAD, 0), new VarInsnNode(Opcodes::ILOAD, 1),
                                new VarInsnNode(Opcodes::ILOAD, 2), new InsnNode(Opcodes::IASTORE),
                                new InsnNode(Opcodes::RETURN)},
                               3, 3);
    ASSERT_EQ(store.kind, BackendResult::Kind::C_SOURCE) << store.text;
    EXPECT_NE(store.text.find("call void @mj2x_llvm_array_store_int(ptr %env, ptr %v0, i32 %v1, i32 %v2)"),
              std::string::npos)
        << store.text;

    // ARRAYLENGTH → array_length helper。
    auto len = compileScalar("n", "([I)I",
                             {new VarInsnNode(Opcodes::ALOAD, 0), new InsnNode(Opcodes::ARRAYLENGTH),
                              new InsnNode(Opcodes::IRETURN)},
                             1, 1);
    ASSERT_EQ(len.kind, BackendResult::Kind::C_SOURCE) << len.text;
    EXPECT_NE(len.text.find("call i32 @mj2x_llvm_array_length(ptr %env, ptr %v0)"),
              std::string::npos)
        << len.text;

    // AASTORE（引用数组）：element type REF → array_store_ref helper。
    auto refStore = compileScalar("putRef", "([Ljava/lang/String;ILjava/lang/String;)V",
                                  {new VarInsnNode(Opcodes::ALOAD, 0),
                                   new VarInsnNode(Opcodes::ILOAD, 1),
                                   new VarInsnNode(Opcodes::ALOAD, 2),
                                   new InsnNode(Opcodes::AASTORE), new InsnNode(Opcodes::RETURN)},
                                  3, 3);
    ASSERT_EQ(refStore.kind, BackendResult::Kind::C_SOURCE) << refStore.text;
    EXPECT_NE(refStore.text.find("call void @mj2x_llvm_array_store_ref(ptr %env, ptr %v0, i32 %v1, ptr %v2)"),
              std::string::npos)
        << refStore.text;

    // BALOAD（byte 组件）：array_load_byte helper（boolean[] 特判在内）。
    auto byteLoad = compileScalar("b", "([BI)B",
                                  {new VarInsnNode(Opcodes::ALOAD, 0),
                                   new VarInsnNode(Opcodes::ILOAD, 1),
                                   new InsnNode(Opcodes::BALOAD), new InsnNode(Opcodes::IRETURN)},
                                  2, 2);
    ASSERT_EQ(byteLoad.kind, BackendResult::Kind::C_SOURCE) << byteLoad.text;
    EXPECT_NE(byteLoad.text.find("call i32 @mj2x_llvm_array_load_byte(ptr %env, ptr %v0, i32 %v1)"),
              std::string::npos)
        << byteLoad.text;
}
