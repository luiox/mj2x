#include <gmock/gmock.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "garena/arena.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/core/any.hpp"
#include "libca/log/logger.hpp"
#include "libca/log/logger_registry.hpp"
#include "libca/str/utf8_string.hpp"
#include "mj2x/backend/regc/regc_backend.hpp"
#include "mj2x/backend/regc/regc_emitter.hpp"
#include "mj2x/frontend/code_converter.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

/// @brief 捕获 regc target 日志的后端（#1193 内联可诊性断言面）。
class CapturingRegcLogBackend final : public ca::log::ILogBackend
{
public:
    void log(ca::log::Level level, std::string_view target, std::string_view file, int line,
             const ca::log::OpaqueFormat& message) override
    {
        (void)target;
        (void)file;
        (void)line;
        std::string text;
        message.render_to(text);
        entries.emplace_back(level, std::move(text));
    }

    std::vector<std::pair<ca::log::Level, std::string>> entries;
};

/// @brief 作用域内接管 regc target 的 logger，析构时注销（测试隔离）。
class ScopedRegcLogCapture
{
public:
    ScopedRegcLogCapture()
        : backend_(std::make_shared<CapturingRegcLogBackend>())
    {
        ca::log::LoggerRegistry::register_logger("regc",
                                                 std::make_shared<ca::log::Logger>(backend_));
    }

    ~ScopedRegcLogCapture() { ca::log::LoggerRegistry::unregister_logger("regc"); }

    CapturingRegcLogBackend& backend() const { return *backend_; }

private:
    std::shared_ptr<CapturingRegcLogBackend> backend_;
};

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
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern(samDesc)));
    bsmArgs.emplace_back(Handle(handleTag, arena.intern(owner), arena.intern(fieldName),
                                arena.intern(fieldDesc)));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern(samDesc)));
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
    bsmArgs.emplace_back(jbytecode::core::Type::getObjectType(arena.intern("p/Point")));
    bsmArgs.emplace_back(std::string("x"));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Lp/Point;)I")));
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

// ── issue #958：直派面 null receiver 守卫按块保护分流 ──
//
// JVMS getfield/putfield/invokevirtual 系：receiver NULL 时于当前 pc 抛 NPE，按
// 异常表路由到方法自身 handler。守卫抛出后按块保护状态分流——受保护块落空进
// 指令后异常分发（方法内 catch NPE 必须接得住）；未受保护块保持 goto cleanup
// 直接传播。实例形态的访问体位于守卫 else 分支（受保护路径不得以 NULL receiver
// 递进 JNI）。

/// throw_null 站点与访问体之间是否夹着 goto cleanup（未受保护直传形态判据）。
static bool trapGoesStraightToCleanup(const std::string& source, const char* trapText,
                                      const char* accessMarker)
{
    const auto trapPos   = source.find(trapText);
    const auto accessPos = source.find(accessMarker, trapPos);
    if (trapPos == std::string::npos || accessPos == std::string::npos) return false;
    const auto gotoPos = source.find("goto _mj2x_regc_cleanup", trapPos);
    return gotoPos != std::string::npos && gotoPos < accessPos;
}

// 受保护块（try 区间覆盖 indy 站点）：throw_null 落空进指令后异常分发，
// 不得直接 goto cleanup；访问体进守卫 else 分支。
TEST(RegCBackendTest, IndyDirectFieldSetterNullTrapRoutesToHandlerWhenProtected)
{
    ca::str::Utf8StringArena arena;
    auto* indy = regcTestDirectFieldIndy(
        arena, "set", "(Lp/Rec;I)V", Opcodes::H_PUTFIELD, "p/Rec", "f", "I", "(I)V");
    auto* tryStart = new LabelNode(new Label());
    auto* tryEnd   = new LabelNode(new Label());
    auto* handler  = new LabelNode(new Label());
    auto  method   = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                              "put",
                              "(Lp/Rec;I)V",
                              {
                                  tryStart,
                                  new VarInsnNode(Opcodes::ALOAD, 0),
                                  new VarInsnNode(Opcodes::ILOAD, 1),
                                  indy,
                                  tryEnd,
                                  new InsnNode(Opcodes::RETURN),
                                  handler,
                                  new VarInsnNode(Opcodes::ASTORE, 1),
                                  new InsnNode(Opcodes::RETURN),
                              },
                              3,
                              2);
    method->tryCatchBlocks.push_back(new TryCatchBlockNode(
        tryStart->label, tryEnd->label, handler->label,
        ca::str::Utf8StringRef::from_cstr("java/lang/Exception")));

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    CodeConverter converter(arena);
    auto          blocks = converter.convert(*method);
    auto result = backend.compileMethod("IndyDirect", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    const auto trapPos =
        result.text.find("_mj2x_regc_throw_null(\"indy direct field access\");");
    const auto accessPos =
        result.text.find("SetIntField(_mj2x_regc_env, _mj2x_indy_direct_recv_0,", trapPos);
    ASSERT_NE(trapPos, std::string::npos) << result.text;
    ASSERT_NE(accessPos, std::string::npos) << result.text;
    const auto gotoPos = result.text.find("goto _mj2x_regc_cleanup", trapPos);
    EXPECT_TRUE(gotoPos == std::string::npos || gotoPos > accessPos) << result.text;
    // 指令后异常分发存在：NPE 可路由到方法自身 handler。
    EXPECT_NE(result.text.find("_mj2x_regc_exception_matches("), std::string::npos)
        << result.text;
    expectNoStackAnchors(result.text);
}

// 未受保护块（对照）：守卫抛出后保持 goto cleanup 直接传播（现状语义不变）。
TEST(RegCBackendTest, IndyDirectFieldSetterNullTrapPropagatesDirectlyWhenUnprotected)
{
    ca::str::Utf8StringArena arena;
    auto* indy = regcTestDirectFieldIndy(
        arena, "set", "(Lp/Rec;I)V", Opcodes::H_PUTFIELD, "p/Rec", "f", "I", "(I)V");
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
    EXPECT_TRUE(trapGoesStraightToCleanup(result.text,
                                          "_mj2x_regc_throw_null(\"indy direct field access\");",
                                          "SetIntField(_mj2x_regc_env, _mj2x_indy_direct_recv_0,"))
        << result.text;
    expectNoStackAnchors(result.text);
}

// 受保护块 F2 record 访问器：与 F1 同口径，throw_null 落空进指令后异常分发。
TEST(RegCBackendTest, IndyDirectRecordAccessorNullTrapRoutesToHandlerWhenProtected)
{
    ca::str::Utf8StringArena arena;
    std::vector<ca::core::Any> bsmArgs;
    bsmArgs.emplace_back(jbytecode::core::Type::getObjectType(arena.intern("p/Point")));
    bsmArgs.emplace_back(std::string("x"));
    bsmArgs.emplace_back(jbytecode::core::Type::getMethodType(arena.intern("(Lp/Point;)I")));
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
    auto* tryStart = new LabelNode(new Label());
    auto* tryEnd   = new LabelNode(new Label());
    auto* handler  = new LabelNode(new Label());
    auto  method   = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                              "getX",
                              "(Lp/Point;)I",
                              {
                                  tryStart,
                                  new VarInsnNode(Opcodes::ALOAD, 0),
                                  indy,
                                  tryEnd,
                                  new InsnNode(Opcodes::IRETURN),
                                  handler,
                                  new VarInsnNode(Opcodes::ASTORE, 1),
                                  new InsnNode(Opcodes::ICONST_M1),
                                  new InsnNode(Opcodes::IRETURN),
                              },
                              2,
                              2);
    method->tryCatchBlocks.push_back(new TryCatchBlockNode(
        tryStart->label, tryEnd->label, handler->label,
        ca::str::Utf8StringRef::from_cstr("java/lang/Exception")));

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    CodeConverter converter(arena);
    auto          blocks = converter.convert(*method);
    auto result = backend.compileMethod("IndyDirect", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    const auto trapPos =
        result.text.find("_mj2x_regc_throw_null(\"indy direct accessor call\");");
    const auto accessPos =
        result.text.find("CallIntMethodA(_mj2x_regc_env, _mj2x_indy_direct_recv_0,", trapPos);
    ASSERT_NE(trapPos, std::string::npos) << result.text;
    ASSERT_NE(accessPos, std::string::npos) << result.text;
    const auto gotoPos = result.text.find("goto _mj2x_regc_cleanup", trapPos);
    EXPECT_TRUE(gotoPos == std::string::npos || gotoPos > accessPos) << result.text;
    EXPECT_NE(result.text.find("_mj2x_regc_exception_matches("), std::string::npos)
        << result.text;
    expectNoStackAnchors(result.text);
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

// ── 浮点→整型转换（#1027 D2）────────────────────────────────────
// 旧实现发裸 C cast：NaN/越界是 UB。修复后 f2i/f2l/d2i/d2l 一律发
// mj2x_regc_f2i32/f2i64 CRT helper（JVM 语义：NaN→0、向零截断、超界饱和），
// 与 StackC/VMC4/LLVM 三端对齐；整型间转换（i2l 等）仍走裸 cast（C 定义良好）。

TEST(RegCBackendTest, FloatToIntConvertEmitsJvmSaturatingHelper)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "f2i",
                             "(F)I",
                             {
                                 new VarInsnNode(Opcodes::FLOAD, 0),
                                 new InsnNode(Opcodes::F2I),
                                 new InsnNode(Opcodes::IRETURN),
                             },
                             1,
                             1);

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend   backend;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*method);
    auto          result = backend.compileMethod("Test", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    // 浮点→int 走 CRT helper（double 形参，float 实参显式提升）。
    EXPECT_NE(result.text.find("mj2x_regc_f2i32((double)"), std::string::npos) << result.text;
    // 不得再出现裸 cast 形态的转换赋值（旧 UB 形态）。
    EXPECT_EQ(result.text.find("= (int32_t)("), std::string::npos) << result.text;
    expectNoStackAnchors(result.text);
}

TEST(RegCBackendTest, DoubleToLongConvertEmitsJvmSaturatingHelper)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "d2l",
                             "(D)J",
                             {
                                 new VarInsnNode(Opcodes::DLOAD, 0),
                                 new InsnNode(Opcodes::D2L),
                                 new InsnNode(Opcodes::LRETURN),
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

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("mj2x_regc_f2i64((double)"), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("= (int64_t)("), std::string::npos) << result.text;
    expectNoStackAnchors(result.text);
}

TEST(RegCBackendTest, IntToLongConvertKeepsPlainCast)
{
    auto method = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             "i2l",
                             "(I)J",
                             {
                                 new VarInsnNode(Opcodes::ILOAD, 0),
                                 new InsnNode(Opcodes::I2L),
                                 new InsnNode(Opcodes::LRETURN),
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

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    // 整型加宽转换保持裸 cast（C 定义良好，无 UB 面）。
    EXPECT_NE(result.text.find("= (int64_t)("), std::string::npos) << result.text;
    EXPECT_EQ(result.text.find("mj2x_regc_f2i64("), std::string::npos) << result.text;
    expectNoStackAnchors(result.text);
}

// ---------------------------------------------------------------------------
// #1193：内联可达性与 jar 条目序解耦——beforeCompile 预解析缓存。
// 被调方法先于调用方被「转换清空」（instructions.clear + ACC_NATIVE，即
// injectIntoClass 之后的形态）时：
//   - 无 beforeCompile（旧驱动序）：resolver 现场解析失败 → 不内联（回退
//     通用 JNI 调用，即条目序依赖的静默回退面）；
//   - 有 beforeCompile：缓存命中 → 内联照常发生（生成体无 JNI 调用）。
// ---------------------------------------------------------------------------

namespace {

/// 构造两个类的 index：Callee.add(II)I 静态方法 + Caller.caller(II)I 调它。
/// 返回后 callerMethod 指向 Caller 的 CompilerMethod。
CompilerIndex makeTwoClassIndex(std::unique_ptr<MethodNode>& add,
                                std::unique_ptr<MethodNode>& caller,
                                CompilerMethod*&             callerMethod)
{
    add = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "add", "(II)I",
                     {new VarInsnNode(Opcodes::ILOAD, 0), new VarInsnNode(Opcodes::ILOAD, 1),
                      new InsnNode(Opcodes::IADD), new InsnNode(Opcodes::IRETURN)},
                     2, 2);
    caller = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "caller", "(II)I",
                        {new VarInsnNode(Opcodes::ILOAD, 0), new VarInsnNode(Opcodes::ILOAD, 1),
                         new MethodInsnNode(Opcodes::INVOKESTATIC,
                                            ca::str::Utf8StringRef::from_cstr("Callee"),
                                            ca::str::Utf8StringRef::from_cstr("add"),
                                            ca::str::Utf8StringRef::from_cstr("(II)I"), false),
                         new InsnNode(Opcodes::IRETURN)},
                        2, 2);

    auto makeClassNode = [](const char* name) {
        auto cn                = std::make_unique<jbytecode::tree::ClassNode>();
        cn->name               = ca::str::Utf8StringRef::from_cstr(name);
        cn->version            = Opcodes::V1_8;
        cn->access             = Opcodes::ACC_PUBLIC;
        cn->superName          = ca::str::Utf8StringRef::from_cstr("java/lang/Object");
        return cn;
    };

    CompilerClass calleeCls;
    calleeCls.classNode = makeClassNode("Callee");
    calleeCls.methods.push_back(CompilerMethod{add.get(), true, false, {}});

    CompilerClass callerCls;
    callerCls.classNode = makeClassNode("Caller");
    callerCls.methods.push_back(CompilerMethod{caller.get(), true, false, {}});

    CompilerIndex index;
    index.addClass("Callee", std::move(calleeCls));
    index.addClass("Caller", std::move(callerCls));
    callerMethod = &index.getClass("Caller")->methods[0];
    return index;
}

/// 模拟 callee 已被先转换：injectIntoClass 之后的形态（指令清空 + native）。
void simulateAlreadyConverted(CompilerIndex& index)
{
    auto* cls = index.getClass("Callee");
    auto* mn  = cls->methods[0].methodNode;
    mn->access |= Opcodes::ACC_NATIVE;
    mn->instructions.clear();
}

}   // namespace

TEST(RegCBackendTest, BeforeCompileCacheDecouplesInlineFromConversionOrder)
{
    std::unique_ptr<MethodNode> add;
    std::unique_ptr<MethodNode> caller;
    CompilerMethod*             callerMethod = nullptr;
    CompilerIndex               index        = makeTwoClassIndex(add, caller, callerMethod);

    RegCBackend backend;
    backend.beforeCompile(index);
    // 被调方法先被转换清空（callee-first 条目序的等效形态）。
    simulateAlreadyConverted(index);

    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*caller);
    auto          result = backend.compileMethod("Caller", *callerMethod, blocks, &index);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    // 缓存命中：内联展开，生成体不含通用 JNI 静态调用。
    EXPECT_EQ(result.text.find("CallStaticIntMethodA"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("regc_Caller_caller"), std::string::npos) << result.text;
}

TEST(RegCBackendTest, WithoutBeforeCompileClearedCalleeFallsBackToJniCall)
{
    std::unique_ptr<MethodNode> add;
    std::unique_ptr<MethodNode> caller;
    CompilerMethod*             callerMethod = nullptr;
    CompilerIndex               index        = makeTwoClassIndex(add, caller, callerMethod);

    RegCBackend backend;
    // 无 beforeCompile：被调方法已清空 → resolver 现场解析失败（旧行为：
    // 条目序依赖的静默回退）。
    simulateAlreadyConverted(index);

    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto          blocks = converter.convert(*caller);
    auto          result = backend.compileMethod("Caller", *callerMethod, blocks, &index);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_NE(result.text.find("CallStaticIntMethodA"), std::string::npos) << result.text;
}

// ---------------------------------------------------------------------------
// #1193 评审 P3-1：逐方法 unresolvable WARN 不得被 INFO 分支压制。
// 方法内同时存在成功内联（Callee.add 缓存命中）与 unresolvable 调用点
// （Other.missing 类不在 index）时，旧实现 else-if 串联——WARN 只在
// inlinedCalls==0 时发射，部分内联的方法反而拿不到可诊性输出（可见性
// 不对称）。红相：删 WARN 独立发射 → 本测试转红。
// ---------------------------------------------------------------------------

TEST(RegCBackendTest, UnresolvableWarnNotSuppressedWhenOtherCallsInlined)
{
    auto add = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "add", "(II)I",
                          {new VarInsnNode(Opcodes::ILOAD, 0), new VarInsnNode(Opcodes::ILOAD, 1),
                           new InsnNode(Opcodes::IADD), new InsnNode(Opcodes::IRETURN)},
                          2, 2);
    // caller: static int caller(int a, int b) {
    //             int s = Callee.add(a, b);     // 可解析 → 内联
    //             return s + Other.missing(a);  // 类不在 index → unresolvable
    //         }
    auto caller = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "caller", "(II)I",
                             {new VarInsnNode(Opcodes::ILOAD, 0),
                              new VarInsnNode(Opcodes::ILOAD, 1),
                              new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("Callee"),
                                                 ca::str::Utf8StringRef::from_cstr("add"),
                                                 ca::str::Utf8StringRef::from_cstr("(II)I"), false),
                              new VarInsnNode(Opcodes::ISTORE, 2),
                              new VarInsnNode(Opcodes::ILOAD, 2),
                              new VarInsnNode(Opcodes::ILOAD, 0),
                              new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("Other"),
                                                 ca::str::Utf8StringRef::from_cstr("missing"),
                                                 ca::str::Utf8StringRef::from_cstr("(I)I"), false),
                              new InsnNode(Opcodes::IADD),
                              new InsnNode(Opcodes::IRETURN)},
                             2, 3);

    auto makeClassNode = [](const char* name) {
        auto cn       = std::make_unique<jbytecode::tree::ClassNode>();
        cn->name      = ca::str::Utf8StringRef::from_cstr(name);
        cn->version   = Opcodes::V1_8;
        cn->access    = Opcodes::ACC_PUBLIC;
        cn->superName = ca::str::Utf8StringRef::from_cstr("java/lang/Object");
        return cn;
    };

    CompilerClass calleeCls;
    calleeCls.classNode = makeClassNode("Callee");
    calleeCls.methods.push_back(CompilerMethod{add.get(), true, false, {}});
    CompilerClass callerCls;
    callerCls.classNode = makeClassNode("Caller");
    callerCls.methods.push_back(CompilerMethod{caller.get(), true, false, {}});

    CompilerIndex index;
    index.addClass("Callee", std::move(calleeCls));
    index.addClass("Caller", std::move(callerCls));
    CompilerMethod* callerMethod = &index.getClass("Caller")->methods[0];

    RegCBackend backend;
    backend.beforeCompile(index);

    ScopedRegcLogCapture capture;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto   blocks = converter.convert(*caller);
    auto   result = backend.compileMethod("Caller", *callerMethod, blocks, &index);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;

    bool sawInlineInfo  = false;
    bool sawUnresolvableWarn = false;
    for (const auto& [level, text] : capture.backend().entries) {
        if (level == ca::log::Level::Info && text.find("inline 1 calls in Caller.caller") !=
                                                 std::string::npos) {
            sawInlineInfo = true;
        }
        if (level == ca::log::Level::Warn &&
            text.find("inline unresolvable callees in Caller.caller(II)I") != std::string::npos) {
            sawUnresolvableWarn = true;
        }
    }
    EXPECT_TRUE(sawInlineInfo) << "INFO 行必须保留（inlinedCalls>0 汇总）";
    EXPECT_TRUE(sawUnresolvableWarn)
        << "unresolvable 逐方法 WARN 不得因同方法存在成功内联而被压制";
}

// ---------------------------------------------------------------------------
// #1193 评审 P3-2：resolver 拒因透出。现场路径（缓存外形状）的各拒绝原因
// 经 failReason 进样例：synchronized callee（beforeCompile 准入排除 → 缓存
// 必未命中）与 class not in index（library 或缺失类）。
// ---------------------------------------------------------------------------

TEST(RegCBackendTest, UnresolvableWarnSampleCarriesResolverReason)
{
    auto add = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "add", "(II)I",
                          {new VarInsnNode(Opcodes::ILOAD, 0), new VarInsnNode(Opcodes::ILOAD, 1),
                           new InsnNode(Opcodes::IADD), new InsnNode(Opcodes::IRETURN)},
                          2, 2);
    auto syncHelper = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC |
                                     Opcodes::ACC_SYNCHRONIZED,
                                 "helper", "(I)I",
                                 {new VarInsnNode(Opcodes::ILOAD, 0),
                                  new InsnNode(Opcodes::ICONST_1),
                                  new InsnNode(Opcodes::IADD),
                                  new InsnNode(Opcodes::IRETURN)},
                                 2, 1);
    // caller: static int caller(int a, int b) {
    //             int s = Callee.add(a, b);          // 缓存命中 → 内联
    //             s += SynchCallee.helper(a);        // synchronized → 现场路径拒绝
    //             return s + Other.missing(a);       // 类不在 index → 拒绝
    //         }
    auto caller = makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, "caller", "(II)I",
                             {new VarInsnNode(Opcodes::ILOAD, 0),
                              new VarInsnNode(Opcodes::ILOAD, 1),
                              new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("Callee"),
                                                 ca::str::Utf8StringRef::from_cstr("add"),
                                                 ca::str::Utf8StringRef::from_cstr("(II)I"), false),
                              new VarInsnNode(Opcodes::ISTORE, 2),
                              new VarInsnNode(Opcodes::ILOAD, 2),
                              new VarInsnNode(Opcodes::ILOAD, 0),
                              new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("SynchCallee"),
                                                 ca::str::Utf8StringRef::from_cstr("helper"),
                                                 ca::str::Utf8StringRef::from_cstr("(I)I"), false),
                              new InsnNode(Opcodes::IADD),
                              new VarInsnNode(Opcodes::ISTORE, 2),
                              new VarInsnNode(Opcodes::ILOAD, 2),
                              new VarInsnNode(Opcodes::ILOAD, 0),
                              new MethodInsnNode(Opcodes::INVOKESTATIC,
                                                 ca::str::Utf8StringRef::from_cstr("Other"),
                                                 ca::str::Utf8StringRef::from_cstr("missing"),
                                                 ca::str::Utf8StringRef::from_cstr("(I)I"), false),
                              new InsnNode(Opcodes::IADD),
                              new InsnNode(Opcodes::IRETURN)},
                             2, 3);

    auto makeClassNode = [](const char* name) {
        auto cn       = std::make_unique<jbytecode::tree::ClassNode>();
        cn->name      = ca::str::Utf8StringRef::from_cstr(name);
        cn->version   = Opcodes::V1_8;
        cn->access    = Opcodes::ACC_PUBLIC;
        cn->superName = ca::str::Utf8StringRef::from_cstr("java/lang/Object");
        return cn;
    };

    CompilerClass calleeCls;
    calleeCls.classNode = makeClassNode("Callee");
    calleeCls.methods.push_back(CompilerMethod{add.get(), true, false, {}});
    CompilerClass syncCls;
    syncCls.classNode = makeClassNode("SynchCallee");
    syncCls.methods.push_back(CompilerMethod{syncHelper.get(), true, false, {}});
    CompilerClass callerCls;
    callerCls.classNode = makeClassNode("Caller");
    callerCls.methods.push_back(CompilerMethod{caller.get(), true, false, {}});

    CompilerIndex index;
    index.addClass("Callee", std::move(calleeCls));
    index.addClass("SynchCallee", std::move(syncCls));
    index.addClass("Caller", std::move(callerCls));
    CompilerMethod* callerMethod = &index.getClass("Caller")->methods[0];

    RegCBackend backend;
    backend.beforeCompile(index);

    ScopedRegcLogCapture capture;
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto   blocks = converter.convert(*caller);
    auto   result = backend.compileMethod("Caller", *callerMethod, blocks, &index);
    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;

    std::string warnText;
    for (const auto& [level, text] : capture.backend().entries) {
        if (level == ca::log::Level::Warn &&
            text.find("inline unresolvable callees in Caller.caller(II)I") != std::string::npos) {
            warnText = text;
        }
    }
    ASSERT_NE(warnText, "") << "unresolvable>0 必有逐方法 WARN";
    // 现场路径 synchronized 拒因（SynchCallee 在 index，beforeCompile 准入
    // 排除 → 缓存未命中 → 现场解析命中方法后拒绝）。
    EXPECT_NE(warnText.find("SynchCallee.helper:(I)I (synchronized callee)"), std::string::npos)
        << warnText;
    // 类缺失拒因。
    EXPECT_NE(warnText.find("Other.missing:(I)I (class not in index (library or absent))"),
              std::string::npos)
        << warnText;

    // finalize 整 jar 汇总同样携带拒因（样例集与逐方法行同源）。
    backend.finalize(index);
    std::string summaryText;
    for (const auto& [level, text] : capture.backend().entries) {
        if (level == ca::log::Level::Warn &&
            text.find("inline unresolvable samples (cap") != std::string::npos) {
            summaryText = text;
        }
    }
    ASSERT_NE(summaryText, "") << "finalize 汇总必须输出 unresolvable 样例";
    EXPECT_NE(summaryText.find("SynchCallee.helper:(I)I (synchronized callee)"),
              std::string::npos)
        << summaryText;
}

// ---- computed jumps / 跳转目标间接化（issue #7）----
//
// switch 的 "case key -> 目标块" 映射从代码挪到数据表：key 按站点密钥异或存放，
// 分派改走运行时解析出的块索引。产物里不再有 `case <字面量>: goto block_bX`。
//
// 走 backend 级而非发射器直连：key 表要安放在 TU 头部注册表，直连发射器没有
// 注册表（会按设计降级为字面量 case）。

namespace {

std::unique_ptr<MethodNode> makeSwitchMethod()
{
    // 标签需要 arena 寿命：与 regc_test.cpp 同形状。
    static garena::Arena labelArena;
    auto* case0 = new LabelNode(labelArena.create<Label>());
    auto* case1 = new LabelNode(labelArena.create<Label>());
    auto* def   = new LabelNode(labelArena.create<Label>());
    return makeMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                      "pick",
                      "(I)I",
                      {
                          new VarInsnNode(Opcodes::ILOAD, 0),
                          new TableSwitchInsnNode(0, 1, def->label, {case0->label, case1->label}),
                          case0,
                          new IntInsnNode(Opcodes::BIPUSH, 7),
                          new InsnNode(Opcodes::IRETURN),
                          case1,
                          new IntInsnNode(Opcodes::BIPUSH, 9),
                          new InsnNode(Opcodes::IRETURN),
                          def,
                          new InsnNode(Opcodes::ICONST_M1),
                          new InsnNode(Opcodes::IRETURN),
                      },
                      2,
                      1);
}

}   // namespace

TEST(RegCBackendTest, ComputedJumpsMovesCaseKeysIntoDataTable)
{
    auto method = makeSwitchMethod();

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend backend;
    backend.setComputedJumps(true);
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto blocks = converter.convert(*method);
    auto result = backend.compileMethod("Test", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    // key 表进 TU 头部。
    EXPECT_NE(result.text.find("_mj2x_cj_keys_"), std::string::npos) << result.text;
    // 分派改走运行时解析出的索引（而非原始 switch 值）。
    EXPECT_NE(result.text.find("switch (_mj2x_cj_idx_"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("mj2x_regc_const_key("), std::string::npos) << result.text;
    // 站点密钥只派生一次（static 局部量），不是每次分派调一次。
    EXPECT_NE(result.text.find("static int64_t _mj2x_cj_k_"), std::string::npos) << result.text;
}

TEST(RegCBackendTest, ComputedJumpsOffKeepsLiteralCaseKeys)
{
    auto method = makeSwitchMethod();

    CompilerMethod cm;
    cm.methodNode = method.get();
    CompilerClass cls;
    cls.methods.push_back(cm);

    RegCBackend backend;   // 默认关
    ca::str::Utf8StringArena arena;
    CodeConverter            converter(arena);
    auto blocks = converter.convert(*method);
    auto result = backend.compileMethod("Test", cls.methods[0], blocks);

    ASSERT_EQ(result.kind, BackendResult::Kind::C_SOURCE) << result.text;
    EXPECT_EQ(result.text.find("_mj2x_cj_"), std::string::npos) << result.text;
    EXPECT_NE(result.text.find("case 0"), std::string::npos) << result.text;
}
