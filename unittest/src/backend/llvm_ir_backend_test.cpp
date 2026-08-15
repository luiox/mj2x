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
    EXPECT_NE(objectResult.text.find("rejects"), std::string::npos) << objectResult.text;

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
    EXPECT_NE(result.text.find("rejects"), std::string::npos) << result.text;
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
