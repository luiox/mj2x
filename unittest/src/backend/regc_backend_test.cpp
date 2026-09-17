#include <gmock/gmock.h>

#include <memory>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "libca/core/any.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/frontend/code_converter.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

std::unique_ptr<MethodNode> makeMethod(int access, const char* name, const char* desc,
                                       std::initializer_list<AbstractInsnNode*> insns, int maxStack,
                                       int maxLocals)
{
    std::vector<ca::str::Utf8StringRef> noExcepts;
    auto                                mn = std::make_unique<MethodNode>(access,
                                           ca::str::ZUtf8StringRef::from_static(name),
                                           ca::str::ZUtf8StringRef::from_static(desc),
                                           ca::str::ZUtf8StringRef::from_static(""),
                                           noExcepts);
    mn->maxStack                           = maxStack;
    mn->maxLocals                          = maxLocals;
    for (auto* insn : insns) mn->instructions.add(insn);
    return mn;
}

void expectNoStackAnchors(const std::string& source)
{
    std::string reason;
    EXPECT_TRUE(validateRegCNoStackAnchors(source, reason)) << reason << "\n" << source;
    EXPECT_EQ(source.find("stack["), std::string::npos) << source;
    EXPECT_EQ(source.find("union jvalue"), std::string::npos) << source;
    EXPECT_EQ(source.find(" sp "), std::string::npos) << source;
}

size_t countSubstring(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

}   // namespace

TEST(RegCBackendTest, EmitsJniWrapperAndMarksStaticMethodNative)
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

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*method);
    auto          result = backend.compileMethod("Test", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE);
    // RegC 用 RegisterNatives 动态绑定：native 方法是不透明 `static ... JNICALL __mj2x_c_<hash>`
    // （无 JNIEXPORT、无 Java_<mangled> 符号）。签名仍匹配 JNI 调用约定。
    EXPECT_NE(result.text.find("static jint JNICALL\n__mj2x_c_"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("(JNIEnv* env, jclass clazz"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("regc_Test_add__II_I((int32_t)(p0), (int32_t)(p1));"),
              std::string::npos)
        << result.text;
    expectNoStackAnchors(result.text);

    backend.injectIntoClass(cls, cls.methods[0], result);
    EXPECT_TRUE(method->access & Opcodes::ACC_NATIVE);
    EXPECT_EQ(method->instructions.size(), 0);
    EXPECT_TRUE(cls.modified);
}

// 反侦测检查点注入（--runtime-protection）：
//   - 默认不注入（零产物变化）
//   - 开启后 helper 入口含 mj2x_rt_checkpoint_run + FAKE 默认值走 cleanup
TEST(RegCBackendTest, RuntimeProtectionInjection)
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

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto                     blocks = converter.convert(*method);

    // 默认：不注入。
    RegCBackend plain;
    auto        plainResult = plain.compileMethod("Test", cls.methods[0], blocks);
    ASSERT_EQ(plainResult.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_EQ(plainResult.text.find("mj2x_rt_checkpoint_run"), std::string::npos)
        << plainResult.text;

    // 开启 FAKE：helper 入口注入检查点，命中返回默认值走 cleanup。
    RegCBackend injected;
    injected.setRuntimeProtection(codegen::RuntimeProtectionAction::Fake, 16, 500);
    auto injectedResult = injected.compileMethod("Test", cls.methods[0], blocks);
    ASSERT_EQ(injectedResult.kind, BackendResult::Kind::C_SOURCE);
    EXPECT_NE(injectedResult.text.find("mj2x_rt_checkpoint_run"), std::string::npos)
        << injectedResult.text;
    EXPECT_NE(injectedResult.text.find("MJ2X_RT_PROTECT_FAKE"), std::string::npos)
        << injectedResult.text;
    EXPECT_NE(injectedResult.text.find("static mj2x_rt_checkpoint mj2x_rt_cp_"),
              std::string::npos)
        << injectedResult.text;
    EXPECT_NE(injectedResult.text.find("goto _mj2x_regc_cleanup;"), std::string::npos)
        << injectedResult.text;
}

TEST(RegCBackendTest, EmitsInstanceWrapperWithReceiverRegister)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC,
                             "bump",
                             "(I)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2,
                             2);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*method);
    auto          result = backend.compileMethod("pkg/Box", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE);
    // 实例方法：不透明 static 函数，receiver 作为第一个参数（jobject thisObject）。
    EXPECT_NE(result.text.find("static jint JNICALL\n__mj2x_c_"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("(JNIEnv* env, jobject thisObject"), std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("regc_pkg_Box_bump__I_I((mj2x_ref)(thisObject), (int32_t)(p0));"),
              std::string::npos)
        << result.text;
    expectNoStackAnchors(result.text);
}

TEST(RegCBackendTest, InstanceTrampolineSkipsGetObjectClassWhenOwnerClassUnused)
{
    // 纯标量实例方法：生成体不引用 _mj2x_regc_owner_class（当前只有 indy 自定义
    // bootstrap helper 调用点引用），trampoline 跳过 GetObjectClass——每条
    // Java→native 入口省 1 次 JNI；env 设置与 context 初始化保持不变。
    auto method = makeMethod(Opcodes::ACC_PUBLIC,
                             "bump",
                             "(I)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 new InsnNode(Opcodes::ICONST_1),
                                 new InsnNode(Opcodes::IADD),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2,
                             2);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*method);
    auto          result = backend.compileMethod("pkg/Box", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_EQ(result.text.find("GetObjectClass"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("_mj2x_regc_env = env;"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("mj2x_regc_init_context(&_mj2x_regc_ctx, env,"),
              std::string::npos)
        << result.text;
    expectNoStackAnchors(result.text);
}

TEST(RegCBackendTest, EscapedWrapperValueOfEmitsCachedBoxHelper)
{
    // static int escapeBox(int i) { Integer v = Integer.valueOf(i); return v.hashCode(); }
    // hashCode 未知调用使装箱值逃逸 native state；逃逸链走 runtime 小值缓存装箱
    // （Integer [-128,127] 命中 0 JNI，返回对象与 JDK valueOf 恒等），不再走
    // 通用 invokestatic JNI 派发。
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "escapeBox",
                             "(I)I",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                    ca::str::ZUtf8StringRef::from_static("java/lang/Integer"),
                                                    ca::str::ZUtf8StringRef::from_static("valueOf"),
                                                    ca::str::ZUtf8StringRef::from_static("(I)Ljava/lang/Integer;"),
                                                    false),
                                 new VarInsnNode(Opcodes::ASTORE, 1),
                                 new VarInsnNode(Opcodes::ALOAD, 1),
                                 new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                                    ca::str::ZUtf8StringRef::from_static("java/lang/Integer"),
                                                    ca::str::ZUtf8StringRef::from_static("hashCode"),
                                                    ca::str::ZUtf8StringRef::from_static("()I"),
                                                    false),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2,
                             2);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*method);
    auto          result = backend.compileMethod("pkg/Box", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("_mj2x_regc_box_i32("), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("_mj2x_regc_call_ref(\"java/lang/Integer\", \"valueOf\""),
              std::string::npos)
        << result.text;
    expectNoStackAnchors(result.text);
}

TEST(RegCBackendTest, InternsCachesAcrossGeneratedTranslationUnit)
{
    auto first = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "first",
        "(Ljava/lang/Object;)I",
        {
            new TypeInsnNode(Opcodes::NEW, ca::str::ZUtf8StringRef::from_static("Foo")),
            new InsnNode(Opcodes::POP),
            new InsnNode(Opcodes::ICONST_1),
            new TypeInsnNode(Opcodes::ANEWARRAY, ca::str::ZUtf8StringRef::from_static("Foo")),
            new InsnNode(Opcodes::POP),
            new InsnNode(Opcodes::ICONST_1),
            new InsnNode(Opcodes::ICONST_1),
            new MultiANewArrayInsnNode(ca::str::Utf8StringRef::from_cstr("[[Ljava/lang/String;"),
                                       2),
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
        2,
        1);
    auto second = makeMethod(
        Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        "second",
        "(Ljava/lang/Object;)I",
        {
            new TypeInsnNode(Opcodes::NEW, ca::str::ZUtf8StringRef::from_static("Foo")),
            new InsnNode(Opcodes::POP),
            new InsnNode(Opcodes::ICONST_1),
            new TypeInsnNode(Opcodes::ANEWARRAY, ca::str::ZUtf8StringRef::from_static("Foo")),
            new InsnNode(Opcodes::POP),
            new InsnNode(Opcodes::ICONST_1),
            new InsnNode(Opcodes::ICONST_1),
            new MultiANewArrayInsnNode(ca::str::Utf8StringRef::from_cstr("[[Ljava/lang/String;"),
                                       2),
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
        2,
        1);

    CompilerMethod firstMethod;
    firstMethod.methodNode = first.get();
    CompilerMethod secondMethod;
    secondMethod.methodNode = second.get();
    CompilerClass cls;
    cls.methods.push_back(firstMethod);
    cls.methods.push_back(secondMethod);

    RegCBackend   backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            firstConverter(arena);
    CodeConverter            secondConverter(arena);
    auto          firstBlocks  = firstConverter.convert(*first);
    auto          secondBlocks = secondConverter.convert(*second);
    auto          firstResult  = backend.compileMethod("CacheOwner", cls.methods[0], firstBlocks);
    auto          secondResult = backend.compileMethod("CacheOwner", cls.methods[1], secondBlocks);

    ASSERT_EQ(firstResult.kind, BackendResult::Kind::C_SOURCE) << firstResult.text;
    ASSERT_EQ(secondResult.kind, BackendResult::Kind::C_SOURCE) << secondResult.text;

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();

    EXPECT_EQ(countSubstring(source, "{\"Foo\", \"sink\", \"I\", NULL, NULL}"), 1u) << source;
    EXPECT_EQ(countSubstring(source, "{\"Foo\", \"value\", \"I\", NULL, NULL}"), 1u) << source;
    EXPECT_EQ(countSubstring(source, "{\"Foo\", \"mix\", \"(I)I\", NULL, NULL}"), 1u) << source;
    EXPECT_EQ(countSubstring(source, "{\"Foo\", \"bump\", \"()I\", NULL, NULL, NULL, NULL}"), 1u)
        << source;
    EXPECT_EQ(
        countSubstring(
            source, "{\"java/lang/Object\", \"toString\", \"()Ljava/lang/String;\", NULL, NULL, "
                    "NULL, NULL}"),
        0u)
        << source;
    EXPECT_EQ(
        countSubstring(
            source, "{\"java/lang/String\", \"valueOf\", \"(F)Ljava/lang/String;\", NULL, NULL}"),
        0u)
        << source;
    EXPECT_EQ(
        countSubstring(
            source, "{\"java/lang/String\", \"valueOf\", \"(D)Ljava/lang/String;\", NULL, NULL}"),
        0u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"Foo\", NULL}"), 1u) << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/CharSequence\", NULL}"), 1u) << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/String\", NULL}"), 1u) << source;
    EXPECT_EQ(countSubstring(source, "{\"[Ljava/lang/String;\", NULL}"), 1u) << source;
    // #894 B 类真下沉:角色单例缓存(toString/valueOf/异常类等)收敛进
    // mj2x_regc_runtime.c 的固定实例,生成 TU 计数归零。
    EXPECT_EQ(countSubstring(source, "{\"java/lang/NullPointerException\", NULL}"), 0u) << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/ClassCastException\", NULL}"), 0u) << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/OutOfMemoryError\", NULL}"), 0u) << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/StringIndexOutOfBoundsException\", NULL}"), 0u)
        << source;
    EXPECT_EQ(countSubstring(source, "{\"java/lang/UnsupportedOperationException\", NULL}"), 0u)
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
    EXPECT_EQ(
        source.find("FindClass(_mj2x_regc_env, \"java/lang/StringIndexOutOfBoundsException\")"),
        std::string::npos)
        << source;
    EXPECT_EQ(source.find("FindClass(_mj2x_regc_env, \"java/lang/UnsupportedOperationException\")"),
              std::string::npos)
        << source;
    EXPECT_EQ(source.find("_mj2x_intrinsic_string_class_cache"), std::string::npos) << source;
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
    EXPECT_NE(source.find("_mj2x_regc_alloc_multi_array_cached(\"[[Ljava/lang/String;\""),
              std::string::npos)
        << source;
    EXPECT_EQ(
        source.find(
            "static void _mj2x_intrinsic_string_builder_init_empty(mj2x_regc_local_string_builder* "
            "state) {"),
        std::string::npos)
        << source;
    expectNoStackAnchors(source);
}

TEST(RegCBackendTest, ReportsC99SourceArtifact)
{
    RegCBackend backend;
    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);

    const auto artifact = backend.generatedSourceArtifact();
    EXPECT_EQ(artifact.language, NativeSourceLanguage::C99);
    EXPECT_EQ(artifact.suggestedFileName, "native.c");
    EXPECT_EQ(artifact.content, backend.generatedSource());
}

TEST(RegCBackendTest, FailedLoweringDoesNotMarkMethodNative)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "bad",
                             "()I",
                             {
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             0,
                             0);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*method);
    auto          result = backend.compileMethod("Bad", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::NONE);
    // fail-closed：lowering 失败时不生成 native 绑定（kind=NONE，result.text 空），
    // 字节码保留由 injectIntoClass 负责。失败原因已通过 printf 诊断日志输出。

    backend.injectIntoClass(cls, cls.methods[0], result);
    EXPECT_FALSE(method->access & Opcodes::ACC_NATIVE);
    EXPECT_GT(method->instructions.size(), 0);
    EXPECT_FALSE(cls.modified);
}

// ---- indy 原生派发（设计文档 §6 P1-P3，波次二）----
//
// F1/F2 发射级断言：直派桩形态（单分支守卫 + 直接字段/访问器 JNI + 缓存表引用）、
// 回退分支保留既有 helper 调用、RuntimeHelper 站点发射文本与现状逐字节一致。
// 站点判定发生在管线层（SSA 登记 sites 时 classifyIndySiteForm），发射端零判定。

namespace {

// 构造一个 LMF metafactory bsm（与 classify 测试同形）。
Handle regcTestLmfBsm(ca::str::Utf8StringArena& arena)
{
    return Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/invoke/LambdaMetafactory"),
                  arena.intern("metafactory"),
                  arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;"
                               "Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)"
                               "Ljava/lang/invoke/CallSite;"));
}

// 构造 F1 直派形态站点（indy 描述符本身即字段操作）。
InvokeDynamicInsnNode* regcTestDirectFieldIndy(ca::str::Utf8StringArena& arena, const char* name,
                                               const char* desc, int handleTag, const char* owner,
                                               const char* fieldName, const char* fieldDesc,
                                               const char* samDesc)
{
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getMethodType(arena.intern(samDesc)));
    bsmArgs.emplace_back(Handle(handleTag, arena.intern(owner), arena.intern(fieldName),
                                arena.intern(fieldDesc)));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern(samDesc)));
    return new InvokeDynamicInsnNode(ca::str::ZUtf8StringRef::from_static(name),
                                     ca::str::ZUtf8StringRef::from_static(desc), regcTestLmfBsm(arena),
                                     std::move(bsmArgs));
}

}   // namespace

// F1 读（GETSTATIC）：守卫条件 + GetStaticIntField 直派 + 回退分支保留 helper 调用；
// finalize 后 TU 级 static 缓存表 + 解析函数（GetStaticFieldID + READY 状态位）。
TEST(RegCBackendTest, IndyDirectFieldGetterEmitsGuardedDirectAccess)
{
    ca::str::Utf8StringArena arena;
    auto*                    indy =
        regcTestDirectFieldIndy(arena, "read", "()I", Opcodes::H_GETSTATIC, "p/Rec", "SV", "I", "()I");
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "read",
                             "()I",
                             {
                                 indy,
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             0,
                             1);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    CodeConverter converter(arena);
    auto          blocks = converter.convert(*method);
    auto result = backend.compileMethod("IndyDirect", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    // 直派快路：单分支守卫 + 直接 GetStaticIntField（无装箱/数组/invokeWithArguments）。
    EXPECT_NE(result.text.find("if (_mj2x_indy_direct_state_0 == 1 || "
                               "_mj2x_indy_direct_resolve_0())"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("GetStaticIntField(_mj2x_regc_env, _mj2x_indy_direct_cls_0, "
                               "_mj2x_indy_direct_fid_0)"),
              std::string::npos)
        << result.text;
    // 回退分支：既有阶段3 helper 调用保留（§4 单调降级，语义与现状一致）。
    EXPECT_NE(result.text.find("jobject _mj2x_indy_target = _mj2x_regc_indy_bsm_0("
                               "_mj2x_regc_owner_class);"),
              std::string::npos)
        << result.text;
    expectNoStackAnchors(result.text);

    // finalize：TU 级 static 缓存表 + 惰性解析函数（build-time 判定 READY，无 CRACK 态）。
    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("static jclass _mj2x_indy_direct_cls_0;"), std::string::npos) << source;
    EXPECT_NE(source.find("static jfieldID _mj2x_indy_direct_fid_0;"), std::string::npos) << source;
    EXPECT_NE(source.find("static uint8_t _mj2x_indy_direct_state_0;"), std::string::npos) << source;
    EXPECT_NE(source.find("static int _mj2x_indy_direct_resolve_0(void)"), std::string::npos)
        << source;
    EXPECT_NE(source.find("GetStaticFieldID(_mj2x_regc_env, cls, \"SV\", \"I\")"), std::string::npos)
        << source;
    expectNoStackAnchors(source);
}

// F1 写（PUTFIELD，实例字段）：receiver NPE trap + SetIntField 直派 + GetFieldID 缓存解析。
TEST(RegCBackendTest, IndyDirectFieldSetterEmitsNullTrapAndStore)
{
    ca::str::Utf8StringArena arena;
    auto*                    indy = regcTestDirectFieldIndy(arena, "set", "(Lp/Rec;I)V",
                                                            Opcodes::H_PUTFIELD, "p/Rec", "f", "I",
                                                            "(I)V");
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "put",
                             "(Lp/Rec;I)V",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 new VarInsnNode(Opcodes::ILOAD, 1),
                                 indy,
                                 new InsnNode(Opcodes::RETURN),
                             },
                             3,
                             2);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    CodeConverter converter(arena);
    auto          blocks = converter.convert(*method);
    auto result = backend.compileMethod("IndyDirect", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    // null receiver 显式 NPE trap（invokeWithArguments 路径的 NPE 语义复现）。
    EXPECT_NE(result.text.find("_mj2x_regc_throw_null(\"indy direct field access\");"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("SetIntField(_mj2x_regc_env, _mj2x_indy_direct_recv_0, "
                               "_mj2x_indy_direct_fid_0,"),
              std::string::npos)
        << result.text;
    expectNoStackAnchors(result.text);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    // 实例字段：GetFieldID（非 GetStaticFieldID）。
    EXPECT_NE(source.find("GetFieldID(_mj2x_regc_env, cls, \"f\", \"I\")"), std::string::npos)
        << source;
}

// F2 record 访问器直派：守卫 + receiver NPE trap + CallIntMethodA 访问器直调 +
// GetMethodID 缓存解析。
TEST(RegCBackendTest, IndyDirectRecordAccessorEmitsGuardedCall)
{
    ca::str::Utf8StringArena arena;
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(Type::getObjectType(arena.intern("p/Point")));
    bsmArgs.emplace_back(std::string("x"));
    bsmArgs.emplace_back(Type::getMethodType(arena.intern("(Lp/Point;)I")));
    bsmArgs.emplace_back(Handle(Opcodes::H_INVOKEVIRTUAL, arena.intern("p/Point"),
                                arena.intern("x"), arena.intern("()I")));
    auto* indy = new InvokeDynamicInsnNode(
        ca::str::ZUtf8StringRef::from_static("x"),
        ca::str::ZUtf8StringRef::from_static("(Lp/Point;)I"),
        Handle(Opcodes::H_INVOKESTATIC, arena.intern("java/lang/runtime/ObjectMethods"),
               arena.intern("bootstrap"),
               arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                            "Ljava/lang/invoke/MethodType;Ljava/lang/Class;Ljava/lang/String;"
                            "Ljava/lang/invoke/MethodType;[Ljava/lang/Object;)Ljava/lang/Object;")),
        std::move(bsmArgs));
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "getX",
                             "(Lp/Point;)I",
                             {
                                 new VarInsnNode(Opcodes::ALOAD, 0),
                                 indy,
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             2,
                             1);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    CodeConverter converter(arena);
    auto          blocks = converter.convert(*method);
    auto result = backend.compileMethod("IndyDirect", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("if (_mj2x_indy_direct_state_0 == 1 || "
                               "_mj2x_indy_direct_resolve_0())"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("_mj2x_regc_throw_null(\"indy direct accessor call\");"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("CallIntMethodA(_mj2x_regc_env, _mj2x_indy_direct_recv_0, "
                               "_mj2x_indy_direct_mid_0, NULL)"),
              std::string::npos)
        << result.text;
    expectNoStackAnchors(result.text);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    EXPECT_NE(source.find("static jmethodID _mj2x_indy_direct_mid_0;"), std::string::npos) << source;
    EXPECT_NE(source.find("GetMethodID(_mj2x_regc_env, cls, \"x\", \"()I\")"), std::string::npos)
        << source;
}

// RuntimeHelper 站点（自定义 bootstrap）零回归锚：发射文本与现状逐字节一致
// （参数数组装箱 + helper 取 target + invokeWithArguments + 拆箱），且不产生任何
// 直派缓存表（build-time 判定失败的站点根本不发直派桩）。
TEST(RegCBackendTest, IndyRuntimeHelperSiteKeepsLegacyHelperText)
{
    ca::str::Utf8StringArena arena;
    auto*                    indy = new InvokeDynamicInsnNode(
        ca::str::ZUtf8StringRef::from_static("call"),
        ca::str::ZUtf8StringRef::from_static("()I"),
        Handle(Opcodes::H_INVOKESTATIC, arena.intern("some/lib/Hidden"), arena.intern("link"),
               arena.intern("(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                            "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;")),
        std::vector<ca::core::Any>{});
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "call",
                             "()I",
                             {
                                 indy,
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1,
                             0);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    CodeConverter converter(arena);
    auto          blocks = converter.convert(*method);
    auto result = backend.compileMethod("IndyHelper", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    // 现状逐字节锚点（与波次二之前的 emitInvokeDynamic 主体一致）。
    EXPECT_NE(result.text.find("jobjectArray _mj2x_indy_args = (jobjectArray)(*_mj2x_regc_env)"
                               "->NewObjectArray(_mj2x_regc_env, 0, _mj2x_indy_obj_cls, NULL);"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("jobject _mj2x_indy_target = _mj2x_regc_indy_bsm_0("
                               "_mj2x_regc_owner_class);"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("jmethodID _mj2x_indy_invoke_mid = (*_mj2x_regc_env)->GetMethodID("
                               "_mj2x_regc_env, _mj2x_indy_mh_cls, \"invokeWithArguments\", "
                               "\"([Ljava/lang/Object;)Ljava/lang/Object;\");"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("jobject _mj2x_indy_result = (jobject)(*_mj2x_regc_env)"
                               "->CallObjectMethod(_mj2x_regc_env, _mj2x_indy_target, "
                               "_mj2x_indy_invoke_mid, _mj2x_indy_args);"),
              std::string::npos)
        << result.text;
    EXPECT_NE(result.text.find("_mj2x_regc_unbox_i32(_mj2x_indy_result)"), std::string::npos)
        << result.text;
    // 无直派桩、无直派缓存表。
    EXPECT_EQ(result.text.find("_mj2x_indy_direct_"), std::string::npos) << result.text;
    expectNoStackAnchors(result.text);

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    EXPECT_EQ(backend.generatedSource().find("_mj2x_indy_direct_"), std::string::npos)
        << backend.generatedSource();
}

// --strings-encrypt 组合：直派缓存表解析函数的 owner/name/desc 字面量走密文缓冲
// （与 emitIndyBsmArg 同一处理），不内联明文；JNI_OnLoad 批量解密注入不变。
TEST(RegCBackendTest, IndyDirectCacheLiteralsEncryptWithStringsEncrypt)
{
    ca::str::Utf8StringArena arena;
    auto*                    indy =
        regcTestDirectFieldIndy(arena, "read", "()I", Opcodes::H_GETSTATIC, "p/Rec", "SV", "I", "()I");
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "read",
                             "()I",
                             {
                                 indy,
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             0,
                             1);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend backend;
    backend.setStringsEncrypt(true);
    CodeConverter converter(arena);
    auto          blocks = converter.convert(*method);
    auto result = backend.compileMethod("IndyDirectEnc", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    // 直派快路本身无任何字面量（全部句柄经缓存表引用）。
    EXPECT_EQ(result.text.find("\"p/Rec\""), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("\"SV\""), std::string::npos) << result.text;

    CompilerIndex emptyIndex;
    backend.finalize(emptyIndex);
    const auto& source = backend.generatedSource();
    // 解析函数字面量改走密文缓冲；区域（区间断言）内不得有明文 owner/name/desc
    // （回退分支的既有 bootstrap 函数会明文重建 impl 句柄——波次一加密切片边界，
    // 非直派缓存表面）。
    const auto resolveBegin = source.find("static int _mj2x_indy_direct_resolve_0(void) {");
    ASSERT_NE(resolveBegin, std::string::npos) << source;
    const auto resolveEnd = source.find("\n}\n", resolveBegin);
    ASSERT_NE(resolveEnd, std::string::npos) << source;
    const std::string resolveBody = source.substr(resolveBegin, resolveEnd - resolveBegin);
    EXPECT_NE(resolveBody.find("_mj2x_str_"), std::string::npos) << resolveBody;
    EXPECT_EQ(resolveBody.find("\"SV\""), std::string::npos) << resolveBody;
    EXPECT_EQ(resolveBody.find("\"p/Rec\""), std::string::npos) << resolveBody;
    EXPECT_EQ(resolveBody.find("\"I\""), std::string::npos) << resolveBody;
    // JNI_OnLoad 批量解密注入不变。
    EXPECT_NE(source.find("_mj2x_regc_decrypt_bytes((uint8_t*)_mj2x_str_"), std::string::npos)
        << source;
}
