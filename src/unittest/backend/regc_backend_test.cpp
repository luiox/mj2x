#include <gmock/gmock.h>

#include <memory>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/frontend/code_converter.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

std::unique_ptr<MethodNode> makeMethod(
    int access,
    const char* name,
    const char* desc,
    std::initializer_list<AbstractInsnNode*> insns,
    int maxStack,
    int maxLocals) {
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto mn = std::make_unique<MethodNode>(
        access,
        ca::str::ZUtf8StringRef::from_static(name),
        ca::str::ZUtf8StringRef::from_static(desc),
        ca::str::ZUtf8StringRef::from_static(""),
        noExcepts);
    mn->maxStack = maxStack;
    mn->maxLocals = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

void expectNoStackAnchors(const std::string& source) {
    std::string reason;
    EXPECT_TRUE(validateRegCNoStackAnchors(source, reason)) << reason << "\n" << source;
    EXPECT_EQ(source.find("stack["), std::string::npos) << source;
    EXPECT_EQ(source.find("union jvalue"), std::string::npos) << source;
    EXPECT_EQ(source.find(" sp "), std::string::npos) << source;
}

} // namespace

TEST(RegCBackendTest, EmitsJniWrapperAndMarksStaticMethodNative) {
    auto method = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "add", "(II)I",
        {
            new VarInsnNode(Opcodes::ILOAD, 0),
            new VarInsnNode(Opcodes::ILOAD, 1),
            new InsnNode(Opcodes::IADD),
            new InsnNode(Opcodes::IRETURN),
        },
        2, 2);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend backend;
    CodeConverter converter;
    auto blocks = converter.convert(*method);
    auto result = backend.compileMethod("Test", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_NE(result.text.find("JNIEXPORT jint JNICALL"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("Java_Test_add__II(JNIEnv* env, jclass clazz"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("regc_Test_add__II_I((int32_t)(p0), (int32_t)(p1));"),
              std::string::npos) << result.text;
    expectNoStackAnchors(result.text);

    backend.injectIntoClass(cls, cls.methods[0], result);
    EXPECT_TRUE(method->access & Opcodes::ACC_NATIVE);
    EXPECT_EQ(method->instructions.size(), 0);
    EXPECT_TRUE(cls.modified);
}

TEST(RegCBackendTest, EmitsInstanceWrapperWithReceiverRegister) {
    auto method = makeMethod(
        Opcodes::ACC_PUBLIC,
        "bump", "(I)I",
        {
            new VarInsnNode(Opcodes::ILOAD, 1),
            new InsnNode(Opcodes::ICONST_1),
            new InsnNode(Opcodes::IADD),
            new InsnNode(Opcodes::IRETURN),
        },
        2, 2);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend backend;
    CodeConverter converter;
    auto blocks = converter.convert(*method);
    auto result = backend.compileMethod("pkg/Box", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_NE(result.text.find("Java_pkg_Box_bump__I(JNIEnv* env, jobject thisObject"),
              std::string::npos) << result.text;
    EXPECT_NE(result.text.find("regc_pkg_Box_bump__I_I((mj2x_ref)(thisObject), (int32_t)(p0));"),
              std::string::npos) << result.text;
    expectNoStackAnchors(result.text);
}

TEST(RegCBackendTest, FailedLoweringDoesNotMarkMethodNative) {
    auto method = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "bad", "()I",
        {
            new InsnNode(Opcodes::IRETURN),
        },
        0, 0);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend backend;
    CodeConverter converter;
    auto blocks = converter.convert(*method);
    auto result = backend.compileMethod("Bad", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::NONE);
    EXPECT_NE(result.text.find("RegC lowering failed"), std::string::npos) << result.text;

    backend.injectIntoClass(cls, cls.methods[0], result);
    EXPECT_FALSE(method->access & Opcodes::ACC_NATIVE);
    EXPECT_GT(method->instructions.size(), 0);
    EXPECT_FALSE(cls.modified);
}
