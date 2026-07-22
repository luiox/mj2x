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
    CodeConverter converter;
    auto blocks = converter.convert(*method);
    auto result = backend.compileMethod("Test", compilerClass.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("define internal i32 @mj2x_llvm_Java_Test_add__II(i32 %p0, i32 %p1)"),
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
    CodeConverter converter;
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
    auto divideMethod = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                   "divides",
                                   "(II)I",
                                   {
                                       new VarInsnNode(Opcodes::ILOAD, 0),
                                       new VarInsnNode(Opcodes::ILOAD, 1),
                                       new InsnNode(Opcodes::IDIV),
                                       new InsnNode(Opcodes::IRETURN),
                                   },
                                   2,
                                   2);

    CompilerMethod objectCompilerMethod;
    objectCompilerMethod.methodNode = objectMethod.get();
    CompilerMethod divideCompilerMethod;
    divideCompilerMethod.methodNode = divideMethod.get();
    CompilerClass compilerClass;
    compilerClass.methods.push_back(objectCompilerMethod);
    compilerClass.methods.push_back(divideCompilerMethod);

    LlvmIrBackend backend;
    CodeConverter objectConverter;
    CodeConverter divideConverter;
    auto objectResult = backend.compileMethod(
        "Test", compilerClass.methods[0], objectConverter.convert(*objectMethod));
    auto divideResult = backend.compileMethod(
        "Test", compilerClass.methods[1], divideConverter.convert(*divideMethod));

    ASSERT_EQ(objectResult.kind, BackendResult::Kind::NONE);
    EXPECT_NE(objectResult.text.find("rejects"), std::string::npos) << objectResult.text;
    ASSERT_EQ(divideResult.kind, BackendResult::Kind::NONE);
    EXPECT_NE(divideResult.text.find("observable effects"), std::string::npos)
        << divideResult.text;

    backend.injectIntoClass(compilerClass, compilerClass.methods[0], objectResult);
    backend.injectIntoClass(compilerClass, compilerClass.methods[1], divideResult);
    EXPECT_FALSE(objectMethod->access & Opcodes::ACC_NATIVE);
    EXPECT_FALSE(divideMethod->access & Opcodes::ACC_NATIVE);
    EXPECT_GT(objectMethod->instructions.size(), 0);
    EXPECT_GT(divideMethod->instructions.size(), 0);
    EXPECT_FALSE(compilerClass.modified);
}
