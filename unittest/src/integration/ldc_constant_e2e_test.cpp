// ldc MethodHandle/MethodType 常量的 exec E2E（X1 回归门）。
//
// javac 不会从 Java 源产出 ldc Handle/MethodType，所以样本类用 libjbytecode
// tree API 手工构造（这也是保护器产物里 ldc 常量的真实来源）。判据闭环：
//   1) 手构类在 JVM（-Xverify:all）上的输出 == 硬编码期望（先证明样本本身对）；
//   2) mj2x-cli（默认注册 LdcPreprocessor，全方法翻译）处理后，加载/运行输出
//      与基线逐字一致 —— 历史缺陷形态是处理后直接 VerifyError。
// 覆盖全部展开家族：findStatic/findVirtual/findConstructor/findGetter/MethodType。
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "test_helper.hpp"
#include "jbytecode/jbytecode.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;
using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

ca::str::Utf8StringRef ref(ca::str::Utf8StringArena& arena, const char* s)
{
    return arena.intern(s);
}

/// 发射 invokeWithArguments 的 varargs 数组装载：new Object[n] 依次填入
/// locals[slot0..slotN-1]。invokeWithArguments 是普通 varargs 方法，字节码
/// 描述符固定为 ([Ljava/lang/Object;)Ljava/lang/Object;（散参形态不存在）。
void emitVarargsSlots(MethodNode* m, ca::str::Utf8StringArena& arena,
                      std::initializer_list<int> slots)
{
    m->visitIntInsn(Opcodes::BIPUSH, static_cast<int>(slots.size()));
    m->visitTypeInsn(Opcodes::ANEWARRAY, ref(arena, "java/lang/Object"));
    int idx = 0;
    for (int slot : slots) {
        m->visitInsn(Opcodes::DUP);
        m->visitIntInsn(Opcodes::BIPUSH, idx);
        m->visitVarInsn(Opcodes::ALOAD, slot);
        m->visitInsn(Opcodes::AASTORE);
        ++idx;
    }
}

/// runBat 输出带 \r\n，判据用 \n（与矩阵测试 normalizeOutput 一致）。
std::string normalizeOutput(std::string value)
{
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    return value;
}

/// 手构 LdcMhSample.class：一个字段、构造器、greet/shout 普通方法 +
/// 五个 ldc 常量方法（static/virtual/ctor/getter handle、MethodType）。
std::vector<ca::u8> buildLdcMhSample(ca::str::Utf8StringArena& arena)
{
    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref(arena, "LdcMhSample");
    cn.superName = ref(arena, "java/lang/Object");
    cn.fields.push_back(new FieldNode(Opcodes::ACC_PRIVATE,
                                      ref(arena, "tag"),
                                      ref(arena, "Ljava/lang/String;"),
                                      ref(arena, ""),
                                      ca::core::Any{}));

    // <init>(String t) { super(); this.tag = t; }
    {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC, ref(arena, "<init>"),
                                 ref(arena, "(Ljava/lang/String;)V"), ref(arena, ""), {});
        m->visitCode();
        m->visitVarInsn(Opcodes::ALOAD, 0);
        m->visitMethodInsn(Opcodes::INVOKESPECIAL, ref(arena, "java/lang/Object"),
                           ref(arena, "<init>"), ref(arena, "()V"), false);
        m->visitVarInsn(Opcodes::ALOAD, 0);
        m->visitVarInsn(Opcodes::ALOAD, 1);
        m->visitFieldInsn(Opcodes::PUTFIELD, ref(arena, "LdcMhSample"), ref(arena, "tag"),
                          ref(arena, "Ljava/lang/String;"));
        m->visitInsn(Opcodes::RETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        cn.methods.push_back(m);
    }

    // static String greet(String s) { return "hello " + s; }
    {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 ref(arena, "greet"),
                                 ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"),
                                 ref(arena, ""), {});
        m->visitCode();
        m->visitTypeInsn(Opcodes::NEW, ref(arena, "java/lang/StringBuilder"));
        m->visitInsn(Opcodes::DUP);
        m->visitMethodInsn(Opcodes::INVOKESPECIAL, ref(arena, "java/lang/StringBuilder"),
                           ref(arena, "<init>"), ref(arena, "()V"), false);
        m->visitLdcInsn(ca::core::Any(std::string("hello ")));
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/StringBuilder"),
                           ref(arena, "append"),
                           ref(arena, "(Ljava/lang/Object;)Ljava/lang/StringBuilder;"), false);
        m->visitVarInsn(Opcodes::ALOAD, 0);
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/StringBuilder"),
                           ref(arena, "append"),
                           ref(arena, "(Ljava/lang/Object;)Ljava/lang/StringBuilder;"), false);
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/StringBuilder"),
                           ref(arena, "toString"), ref(arena, "()Ljava/lang/String;"), false);
        m->visitInsn(Opcodes::ARETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        cn.methods.push_back(m);
    }

    // String shout(String s) { return s + ":" + tag; }
    {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC, ref(arena, "shout"),
                                 ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"),
                                 ref(arena, ""), {});
        m->visitCode();
        m->visitTypeInsn(Opcodes::NEW, ref(arena, "java/lang/StringBuilder"));
        m->visitInsn(Opcodes::DUP);
        m->visitMethodInsn(Opcodes::INVOKESPECIAL, ref(arena, "java/lang/StringBuilder"),
                           ref(arena, "<init>"), ref(arena, "()V"), false);
        m->visitVarInsn(Opcodes::ALOAD, 1);
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/StringBuilder"),
                           ref(arena, "append"),
                           ref(arena, "(Ljava/lang/Object;)Ljava/lang/StringBuilder;"), false);
        m->visitLdcInsn(ca::core::Any(std::string(":")));
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/StringBuilder"),
                           ref(arena, "append"),
                           ref(arena, "(Ljava/lang/Object;)Ljava/lang/StringBuilder;"), false);
        m->visitVarInsn(Opcodes::ALOAD, 0);
        m->visitFieldInsn(Opcodes::GETFIELD, ref(arena, "LdcMhSample"), ref(arena, "tag"),
                          ref(arena, "Ljava/lang/String;"));
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/StringBuilder"),
                           ref(arena, "append"),
                           ref(arena, "(Ljava/lang/Object;)Ljava/lang/StringBuilder;"), false);
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/StringBuilder"),
                           ref(arena, "toString"), ref(arena, "()Ljava/lang/String;"), false);
        m->visitInsn(Opcodes::ARETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        cn.methods.push_back(m);
    }

    // ldc Handle(H_INVOKESTATIC, greet) → invokeWithArguments(s)。
    {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 ref(arena, "useStaticHandle"),
                                 ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"),
                                 ref(arena, ""), {});
        m->visitCode();
        m->visitLdcInsn(ca::core::Any(Handle(Opcodes::H_INVOKESTATIC, ref(arena, "LdcMhSample"),
                                             ref(arena, "greet"),
                                             ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"))));
        emitVarargsSlots(m, arena, {0});
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/invoke/MethodHandle"),
                           ref(arena, "invokeWithArguments"),
                           ref(arena, "([Ljava/lang/Object;)Ljava/lang/Object;"), false);
        m->visitTypeInsn(Opcodes::CHECKCAST, ref(arena, "java/lang/String"));
        m->visitInsn(Opcodes::ARETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        cn.methods.push_back(m);
    }

    // ldc Handle(H_INVOKEVIRTUAL, shout) → invokeWithArguments(new LdcMhSample("v"), s)。
    {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 ref(arena, "useVirtualHandle"),
                                 ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"),
                                 ref(arena, ""), {});
        m->visitCode();
        m->visitTypeInsn(Opcodes::NEW, ref(arena, "LdcMhSample"));
        m->visitInsn(Opcodes::DUP);
        m->visitLdcInsn(ca::core::Any(std::string("v")));
        m->visitMethodInsn(Opcodes::INVOKESPECIAL, ref(arena, "LdcMhSample"),
                           ref(arena, "<init>"), ref(arena, "(Ljava/lang/String;)V"), false);
        m->visitVarInsn(Opcodes::ASTORE, 1);
        m->visitLdcInsn(ca::core::Any(Handle(Opcodes::H_INVOKEVIRTUAL, ref(arena, "LdcMhSample"),
                                             ref(arena, "shout"),
                                             ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"))));
        emitVarargsSlots(m, arena, {1, 0});
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/invoke/MethodHandle"),
                           ref(arena, "invokeWithArguments"),
                           ref(arena, "([Ljava/lang/Object;)Ljava/lang/Object;"), false);
        m->visitTypeInsn(Opcodes::CHECKCAST, ref(arena, "java/lang/String"));
        m->visitInsn(Opcodes::ARETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        cn.methods.push_back(m);
    }

    // ldc Handle(H_NEWINVOKESPECIAL, <init>) → invokeWithArguments(s) 得实例，再 shout("ctor")。
    {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 ref(arena, "useCtorHandle"),
                                 ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"),
                                 ref(arena, ""), {});
        m->visitCode();
        m->visitLdcInsn(ca::core::Any(Handle(Opcodes::H_NEWINVOKESPECIAL, ref(arena, "LdcMhSample"),
                                             ref(arena, "<init>"),
                                             ref(arena, "(Ljava/lang/String;)V"))));
        emitVarargsSlots(m, arena, {0});
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/invoke/MethodHandle"),
                           ref(arena, "invokeWithArguments"),
                           ref(arena, "([Ljava/lang/Object;)Ljava/lang/Object;"), false);
        m->visitTypeInsn(Opcodes::CHECKCAST, ref(arena, "LdcMhSample"));
        m->visitLdcInsn(ca::core::Any(std::string("ctor")));
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "LdcMhSample"),
                           ref(arena, "shout"),
                           ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"), false);
        m->visitInsn(Opcodes::ARETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        cn.methods.push_back(m);
    }

    // ldc Handle(H_GETFIELD, tag) → invokeWithArguments(new LdcMhSample(s))。
    {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 ref(arena, "useGetterHandle"),
                                 ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"),
                                 ref(arena, ""), {});
        m->visitCode();
        m->visitTypeInsn(Opcodes::NEW, ref(arena, "LdcMhSample"));
        m->visitInsn(Opcodes::DUP);
        m->visitVarInsn(Opcodes::ALOAD, 0);
        m->visitMethodInsn(Opcodes::INVOKESPECIAL, ref(arena, "LdcMhSample"),
                           ref(arena, "<init>"), ref(arena, "(Ljava/lang/String;)V"), false);
        m->visitVarInsn(Opcodes::ASTORE, 1);
        m->visitLdcInsn(ca::core::Any(Handle(Opcodes::H_GETFIELD, ref(arena, "LdcMhSample"),
                                             ref(arena, "tag"),
                                             ref(arena, "Ljava/lang/String;"))));
        emitVarargsSlots(m, arena, {1});
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/invoke/MethodHandle"),
                           ref(arena, "invokeWithArguments"),
                           ref(arena, "([Ljava/lang/Object;)Ljava/lang/Object;"), false);
        m->visitTypeInsn(Opcodes::CHECKCAST, ref(arena, "java/lang/String"));
        m->visitInsn(Opcodes::ARETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        cn.methods.push_back(m);
    }

    // ldc MethodType("(I)V") → toMethodDescriptorString()。
    {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 ref(arena, "useMethodType"),
                                 ref(arena, "(Ljava/lang/String;)Ljava/lang/String;"),
                                 ref(arena, ""), {});
        m->visitCode();
        m->visitLdcInsn(ca::core::Any(
            Type::getMethodType(ca::str::Utf8StringRef::from_cstr("(I)V"))));
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref(arena, "java/lang/invoke/MethodType"),
                           ref(arena, "toMethodDescriptorString"),
                           ref(arena, "()Ljava/lang/String;"), false);
        m->visitInsn(Opcodes::ARETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        cn.methods.push_back(m);
    }

    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

class LdcConstantE2E : public ::testing::Test
{
protected:
    struct Toolchain
    {
        bool         available = false;
        std::string  reason;
        JdkToolchain jdk;
        fs::path     cli;
    };

    static Toolchain tc_;

    static void SetUpTestSuite()
    {
        ProbeOptions opts;
        opts.requireJniHeader = true;
        tc_.jdk = probeJdk(opts);
        if (!tc_.jdk.available) {
            tc_.reason = tc_.jdk.reason;
            return;
        }
        tc_.cli = test::getBuildTargetPath("mj2x-cli");
        if (!fileExists(tc_.cli)) {
            tc_.reason = "mj2x-cli not built: " + tc_.cli.string();
            return;
        }
        tc_.available = true;
    }

    void SetUp() override
    {
        if (!tc_.available) GTEST_SKIP() << "E2E toolchain unavailable: " << tc_.reason;
    }
};

LdcConstantE2E::Toolchain LdcConstantE2E::tc_;

TEST_F(LdcConstantE2E, LdcHandleAndMethodTypeSurviveTranslation)
{
    const fs::path dir = test::getOutPath("mj2x/integration/ldc_constant/.keep").parent_path();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    ca::str::Utf8StringArena arena;
    const auto               bytes = buildLdcMhSample(arena);
    {
        std::ofstream out(dir / "LdcMhSample.class", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(out.good());
    }

    {
        std::ofstream out(dir / "Driver.java");
        out << "public class Driver {\n"
               "  public static void main(String[] args) throws Throwable {\n"
               "    String dll = System.getenv(\"MORPHER_DLL\");\n"
               "    if (dll != null) System.load(dll);\n"
               "    System.out.println(LdcMhSample.useStaticHandle(\"world\"));\n"
               "    System.out.println(LdcMhSample.useVirtualHandle(\"s\"));\n"
               "    System.out.println(LdcMhSample.useCtorHandle(\"c\"));\n"
               "    System.out.println(LdcMhSample.useGetterHandle(\"g\"));\n"
               "    System.out.println(LdcMhSample.useMethodType(\"x\"));\n"
               "  }\n"
               "}\n";
    }
    const auto compile = runBat({q(tc_.jdk.javac) + " -encoding UTF-8 -cp " + q(dir) + " -d " +
                                 q(dir) + " " + q(dir / "Driver.java")});
    ASSERT_EQ(compile.exitCode, 0) << compile.out;

    // 1) 手构样本本身在 JVM 上行为正确（-Xverify:all 也校验 ldc 常量形态）。
    const auto baselineRun =
        runBat({q(tc_.jdk.java) + " -Xverify:all -cp " + q(dir) + " Driver"});
    ASSERT_EQ(baselineRun.exitCode, 0) << baselineRun.out;
    const std::string expected =
        "hello world\n"
        "s:v\n"
        "ctor:c\n"
        "g\n"
        "(I)V\n";
    ASSERT_EQ(normalizeOutput(baselineRun.out), expected) << baselineRun.out;

    // 2) mj2x-cli 全方法翻译（默认 LdcPreprocessor）后行为一致。
    const fs::path inClass  = dir / "LdcMhSample.class";
    const fs::path outClass = dir / "LdcMhSample_protected.class";
    const fs::path cFile    = dir / "ldc.c";
    const auto     transform = runBat({q(tc_.cli) + " -i " + q(inClass) + " -o " + q(outClass) +
                                   " --backend stackc --emit-c " + q(cFile) + " --compile"});
    ASSERT_EQ(transform.exitCode, 0) << transform.out;
    ASSERT_TRUE(fileExists(outClass)) << outClass.string();

    // --compile 的 native 库产物名由 CLI nativeLibraryPath 决定：输出 stem +
    // 平台扩展名（Windows .dll / macOS .dylib / 其余 .so）。
#if defined(_WIN32)
    const fs::path nativeLib = dir / "LdcMhSample_protected.dll";
#elif defined(__APPLE__)
    const fs::path nativeLib = dir / "LdcMhSample_protected.dylib";
#else
    const fs::path nativeLib = dir / "LdcMhSample_protected.so";
#endif
    ASSERT_TRUE(fileExists(nativeLib)) << nativeLib.string();
    fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    // MORPHER_DLL 环境变量按平台注入：cmd 用 set，POSIX sh 用 export。
#ifdef _WIN32
    const std::string envLine = "set \"MORPHER_DLL=" + nativeLib.string() + "\"";
#else
    const std::string envLine = "export MORPHER_DLL=" + nativeLib.string();
#endif
    const auto nativeRun = runBat({envLine,
                                   q(tc_.jdk.java) + " -Xverify:all -cp " + q(dir) + " Driver"});
    ASSERT_EQ(nativeRun.exitCode, 0) << nativeRun.out;
    EXPECT_EQ(normalizeOutput(nativeRun.out), expected) << nativeRun.out;
}

}   // namespace
