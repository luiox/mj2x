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

size_t countSubstring(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
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

TEST(RegCBackendTest, InternsCachesAcrossGeneratedTranslationUnit) {
    auto first = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "first", "(Ljava/lang/Object;)I",
        {
            new TypeInsnNode(Opcodes::NEW,
                             ca::str::ZUtf8StringRef::from_static("Foo")),
            new InsnNode(Opcodes::POP),
            new InsnNode(Opcodes::ICONST_1),
            new TypeInsnNode(Opcodes::ANEWARRAY,
                             ca::str::ZUtf8StringRef::from_static("Foo")),
            new InsnNode(Opcodes::POP),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new TypeInsnNode(Opcodes::CHECKCAST,
                             ca::str::ZUtf8StringRef::from_static("java/lang/CharSequence")),
            new InsnNode(Opcodes::POP),
            new FieldInsnNode(Opcodes::GETSTATIC,
                              ca::str::ZUtf8StringRef::from_static("Foo"),
                              ca::str::ZUtf8StringRef::from_static("sink"),
                              ca::str::ZUtf8StringRef::from_static("I")),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               ca::str::ZUtf8StringRef::from_static("Foo"),
                               ca::str::ZUtf8StringRef::from_static("mix"),
                               ca::str::ZUtf8StringRef::from_static("(I)I"),
                               false),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new FieldInsnNode(Opcodes::GETFIELD,
                              ca::str::ZUtf8StringRef::from_static("Foo"),
                              ca::str::ZUtf8StringRef::from_static("value"),
                              ca::str::ZUtf8StringRef::from_static("I")),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               ca::str::ZUtf8StringRef::from_static("Foo"),
                               ca::str::ZUtf8StringRef::from_static("bump"),
                               ca::str::ZUtf8StringRef::from_static("()I"),
                               false),
            new InsnNode(Opcodes::IADD),
            new InsnNode(Opcodes::IRETURN),
        },
        2, 1);
    auto second = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "second", "(Ljava/lang/Object;)I",
        {
            new TypeInsnNode(Opcodes::NEW,
                             ca::str::ZUtf8StringRef::from_static("Foo")),
            new InsnNode(Opcodes::POP),
            new InsnNode(Opcodes::ICONST_1),
            new TypeInsnNode(Opcodes::ANEWARRAY,
                             ca::str::ZUtf8StringRef::from_static("Foo")),
            new InsnNode(Opcodes::POP),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new TypeInsnNode(Opcodes::CHECKCAST,
                             ca::str::ZUtf8StringRef::from_static("java/lang/CharSequence")),
            new InsnNode(Opcodes::POP),
            new FieldInsnNode(Opcodes::GETSTATIC,
                              ca::str::ZUtf8StringRef::from_static("Foo"),
                              ca::str::ZUtf8StringRef::from_static("sink"),
                              ca::str::ZUtf8StringRef::from_static("I")),
            new MethodInsnNode(Opcodes::INVOKESTATIC,
                               ca::str::ZUtf8StringRef::from_static("Foo"),
                               ca::str::ZUtf8StringRef::from_static("mix"),
                               ca::str::ZUtf8StringRef::from_static("(I)I"),
                               false),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new FieldInsnNode(Opcodes::GETFIELD,
                              ca::str::ZUtf8StringRef::from_static("Foo"),
                              ca::str::ZUtf8StringRef::from_static("value"),
                              ca::str::ZUtf8StringRef::from_static("I")),
            new InsnNode(Opcodes::IADD),
            new VarInsnNode(Opcodes::ALOAD, 0),
            new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                               ca::str::ZUtf8StringRef::from_static("Foo"),
                               ca::str::ZUtf8StringRef::from_static("bump"),
                               ca::str::ZUtf8StringRef::from_static("()I"),
                               false),
            new InsnNode(Opcodes::IADD),
            new InsnNode(Opcodes::IRETURN),
        },
        2, 1);

    CompilerMethod firstMethod;
    firstMethod.methodNode = first.get();
    CompilerMethod secondMethod;
    secondMethod.methodNode = second.get();
    CompilerClass cls;
    cls.methods.push_back(firstMethod);
    cls.methods.push_back(secondMethod);

    RegCBackend   backend;
    CodeConverter firstConverter;
    CodeConverter secondConverter;
    auto          firstBlocks  = firstConverter.convert(*first);
    auto          secondBlocks = secondConverter.convert(*second);
    auto          firstResult  = backend.compileMethod("CacheOwner", cls.methods[0], firstBlocks);
    auto          secondResult = backend.compileMethod("CacheOwner", cls.methods[1], secondBlocks);

    ASSERT_EQ(firstResult.kind, BackendResult::Kind::C_SOURCE) << firstResult.text;
    ASSERT_EQ(secondResult.kind, BackendResult::Kind::C_SOURCE) << secondResult.text;

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();

    EXPECT_EQ(countSubstring(source, "{\"Foo\", \"sink\", \"I\", NULL, NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"Foo\", \"value\", \"I\", NULL, NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"Foo\", \"mix\", \"(I)I\", NULL, NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"Foo\", \"bump\", \"()I\", NULL, NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source,
                             "{\"java/lang/Object\", \"toString\", \"()Ljava/lang/String;\", NULL, NULL}"),
              1u)
        << source;
    EXPECT_EQ(countSubstring(source,
                             "{\"java/lang/String\", \"valueOf\", \"(F)Ljava/lang/String;\", NULL, NULL}"),
              1u)
        << source;
    EXPECT_EQ(countSubstring(source,
                             "{\"java/lang/String\", \"valueOf\", \"(D)Ljava/lang/String;\", NULL, NULL}"),
              1u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"Foo\", NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/CharSequence\", NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/String\", NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/NullPointerException\", NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/ClassCastException\", NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/OutOfMemoryError\", NULL}"), 1u)
        << source;
    EXPECT_EQ(countSubstring(source,
                             "{\"java/lang/StringIndexOutOfBoundsException\", NULL}"),
              1u)
        << source;
    EXPECT_EQ(countSubstring(source,
                             "{\"java/lang/UnsupportedOperationException\", NULL}"),
              1u)
        << source;
    EXPECT_EQ(source.find("FindClass(_mj2x_regc_env, \"java/lang/NullPointerException\")"),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find("FindClass(_mj2x_regc_env, \"java/lang/ClassCastException\")"),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find("FindClass(_mj2x_regc_env, \"java/lang/OutOfMemoryError\")"),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find(
                  "FindClass(_mj2x_regc_env, \"java/lang/StringIndexOutOfBoundsException\")"),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find(
                  "FindClass(_mj2x_regc_env, \"java/lang/UnsupportedOperationException\")"),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find("_mj2x_intrinsic_string_class_cache"), std::string::npos)
        << source;
    EXPECT_NE(source.find("_mj2x_regc_field_cache_tu_"), std::string::npos) << source;
    EXPECT_NE(source.find("_mj2x_regc_instance_field_cache_tu_"), std::string::npos) << source;
    EXPECT_NE(source.find("_mj2x_regc_cache_tu_"), std::string::npos) << source;
    EXPECT_NE(source.find("_mj2x_regc_instance_cache_tu_"), std::string::npos) << source;
    EXPECT_NE(source.find("_mj2x_regc_class_cache_tu_"), std::string::npos) << source;
    EXPECT_NE(source.find("_mj2x_regc_alloc_object_cached(&_mj2x_regc_class_cache_tu_"),
              std::string::npos)
        << source;
    EXPECT_NE(source.find("_mj2x_regc_alloc_ref_array_cached(&_mj2x_regc_class_cache_tu_"),
              std::string::npos)
        << source;
    expectNoStackAnchors(source);
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
