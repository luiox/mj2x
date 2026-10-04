// lambda 合成类宿主包 exec E2E（#1182 回归门）。
//
// #887 §5 复现形态：RegC 转换产物中 lambda 捕获类经 FindClass 缓存解析失败 →
// lambda 包装的反射调用全 NPE。根因（#1182 卡内实验裁决）：合成类固定落
// mj2x/lambda 包，实现包私有 SAM 的合成类在 defineClass 阶段抛 IllegalAccessError
// （异常挂起）→ FindClass 返回 NULL → 生成码 NULL 守卫把 op 落 NULL。
//
// 判据闭环：
//   1) 源样本（包私有 SAM 捕获/无捕获/嵌套/反射包装 lambda）在 JVM 基线全绿；
//   2) mj2x-cli --backend regc --regc-direct-calls --generate-xmake --compile 后
//      java -jar 运行输出与基线逐字一致（修复前此处全 NPE）；
//   3) 产物形状钉：包私有 SAM 的合成类落 demo1182/ 包、public SAM 合成类维持
//      mj2x/lambda/、嵌入 DLL 在场（#887 假阴性防护条款）。
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;

namespace {

// 输出判定统一用 test::jdk::normalizeOutput（jdk_fixture 提供，去 \r\n）。

const char* kSampleMain = R"(package demo1182;

import java.lang.reflect.Method;

public class Main {

    // 包私有函数式接口：合成 lambda 类不得落固定 mj2x/lambda 包（#1182 根因面）。
    interface Op { int apply(int x); }

    static int work(Op op, int v) { return op.apply(v); }

    public static class SecretBox {
        private int hiddenMix(int v) { return v * 3 + 7; }
        private static int revealToken(int v) { return v ^ 0x5a; }
    }

    static void check(String name, boolean cond) {
        if (!cond) { throw new AssertionError(name + " FAILED"); }
        System.out.println("ok " + name);
    }

    public static void main(String[] args) throws Exception {
        SecretBox box = new SecretBox();

        Op nocap = x -> x + 1;                       // 无捕获
        check("nocap", work(nocap, 10) == 11);

        int bias = 100;
        Op cap = x -> box.hiddenMix(x) + bias;       // 捕获实例+局部
        check("cap", work(cap, 5) == 122);

        java.util.function.IntFunction<java.util.function.IntUnaryOperator> mk =
                a -> (x -> x * a + bias);            // 嵌套 lambda（两代合成类，public SAM）
        check("nested", mk.apply(2).applyAsInt(3) == 106);

        Method mHidden = SecretBox.class.getDeclaredMethod("hiddenMix", int.class);
        mHidden.setAccessible(true);
        Op refl = x -> {                             // 反射包装（lambda 体里 Method.invoke）
            try { return (int) mHidden.invoke(box, x); }
            catch (ReflectiveOperationException e) { throw new RuntimeException(e); }
        };
        check("refl", work(refl, 4) == 19);

        Method mReveal = SecretBox.class.getDeclaredMethod("revealToken", int.class);
        mReveal.setAccessible(true);
        Op refl2 = x -> {
            try { return (int) mReveal.invoke(null, x); }
            catch (ReflectiveOperationException e) { throw new RuntimeException(e); }
        };
        check("refl2", work(refl2, 0) == 0x5a);

        Method apply = Op.class.getMethod("apply", int.class);
        Object opObj = (Object) cap;                 // 反射打 lambda 包装实例本身
        check("refl-op", ((Integer) apply.invoke(opObj, 7)).intValue() == 128);

        System.out.println("ALL_OK");
    }
}
)";

}   // namespace

class LambdaSynthPackageE2E : public ::testing::Test
{
protected:
    struct Toolchain
    {
        bool         available = false;
        std::string  reason;
        JdkToolchain jdk;
        fs::path     cli;
        fs::path     jarTool;
    };

    static Toolchain tc_;

    static void SetUpTestSuite()
    {
        ProbeOptions opts;
        opts.requireJniHeader = true;   // regc --compile 的 native 构建需要 jni.h
        tc_.jdk = probeJdk(opts);
        if (!tc_.jdk.available) {
            tc_.reason = tc_.jdk.reason;
            return;
        }
#if defined(_WIN32)
        tc_.jarTool = tc_.jdk.javaHome / "bin" / "jar.exe";
#else
        tc_.jarTool = tc_.jdk.javaHome / "bin" / "jar";
#endif
        if (!fileExists(tc_.jarTool)) {
            tc_.reason = "jar tool 不存在: " + tc_.jarTool.string();
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

LambdaSynthPackageE2E::Toolchain LambdaSynthPackageE2E::tc_;

TEST_F(LambdaSynthPackageE2E, PackagePrivateSamLambdaSurvivesRegcNative)
{
    const fs::path dir = test::getOutPath("mj2x/integration/lambda_synth/.keep").parent_path();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // ---- 1) 源样本 + 输入 jar ----
    const fs::path srcDir = dir / "src" / "demo1182";
    fs::create_directories(srcDir, ec);
    {
        std::ofstream out(srcDir / "Main.java", std::ios::binary);
        out << kSampleMain;
        ASSERT_TRUE(out.good());
    }
    const fs::path classesDir = dir / "classes";
    const auto compile = runBat({q(tc_.jdk.javac) + " -encoding UTF-8 -d " + q(classesDir) + " " +
                                 q(srcDir / "Main.java")});
    ASSERT_EQ(compile.exitCode, 0) << compile.out;

    const fs::path inJar = dir / "lambda-synth-input.jar";
    const auto jarCreate =
        runBat({q(tc_.jarTool) + " --create --file=" + q(inJar) +
                " --main-class demo1182.Main -C " + q(classesDir) + " ."});
    ASSERT_EQ(jarCreate.exitCode, 0) << jarCreate.out;

    // ---- 2) 基线 JVM 行为（样本本身正确）----
    const auto baseline =
        runBat({q(tc_.jdk.java) + " -cp " + q(classesDir) + " demo1182.Main"});
    ASSERT_EQ(baseline.exitCode, 0) << baseline.out;
    const std::string expected =
        "ok nocap\n"
        "ok cap\n"
        "ok nested\n"
        "ok refl\n"
        "ok refl2\n"
        "ok refl-op\n"
        "ALL_OK\n";
    ASSERT_EQ(normalizeOutput(baseline.out), expected) << baseline.out;

    // ---- 3) RegC 转换 + native 编译 + DLL 嵌入（#887 §5 复现命令形态）----
    const fs::path outJar = dir / "lambda-synth-regc.jar";
    const fs::path proj   = dir / "native_project";
    const auto transform = runBat({q(tc_.cli) + " -i " + q(inJar) + " -o " + q(outJar) +
                                   " --backend regc --regc-direct-calls --generate-xmake " +
                                   q(proj) + " --compile"});
    ASSERT_EQ(transform.exitCode, 0) << transform.out;
    ASSERT_TRUE(fileExists(outJar)) << outJar.string();

    // 产物形状钉：嵌入 DLL 在场（#887 假阴性防护：三者齐备才算转换后产物）。
    std::ifstream      jarIn(outJar, std::ios::binary);
    const std::string  jarBytes((std::istreambuf_iterator<char>(jarIn)),
                                std::istreambuf_iterator<char>());
    ASSERT_NE(jarBytes.find("META-INF/mj2x/native/"), std::string::npos)
        << "转换产物缺嵌入 DLL";
    // 宿主包钉：包私有 SAM 合成类落 SAM 同包；public SAM 合成类维持 mj2x/lambda/。
    ASSERT_NE(jarBytes.find("demo1182/Lambda$0.class"), std::string::npos)
        << "包私有 SAM 合成类未落 SAM 同包";
    ASSERT_NE(jarBytes.find("mj2x/lambda/Lambda$"), std::string::npos)
        << "public SAM 合成类应维持 mj2x/lambda 前缀";

    // ---- 4) 转换后行为一致（修复前：全 NPE，op 为 NULL）----
    const auto protectedRun = runBat({q(tc_.jdk.java) + " -jar " + q(outJar)});
    ASSERT_EQ(protectedRun.exitCode, 0) << protectedRun.out;
    EXPECT_EQ(normalizeOutput(protectedRun.out), expected) << protectedRun.out;
    EXPECT_EQ(protectedRun.out.find("Exception"), std::string::npos) << protectedRun.out;
}
