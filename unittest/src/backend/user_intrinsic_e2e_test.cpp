// 用户自定义 intrinsic（issue #818）三后端 E2E：CLI 全链路。
//
// 流程（与产品使用路径一致）：
//   javac → mj2x-cli -i -o --emit-c --backend <b> --intrinsic-config cfg.json
//           --intrinsic-src impl.c --compile → java 运行对比。
//
// 语义契约：UserIntr.run(a,b) 调用 F.m(a,b)（Java 实现返回 a+b）。
// 配置把 F.m(II)I 降级为 my_m（返回 a*1000+b）。三后端都必须输出
// run=2003（证明调用点被用户 symbol 接管，而非 Java/JNI 通用派发）。
//
// 本文件是 #820 测试套件的三后端矩阵种子：每个后端一个执行级用例。

#include <gmock/gmock.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;   // runBat / q / fileExists / ExecResult / JdkToolchain / probeJdk

namespace {

// ---------------------------------------------------------------------------
// 工具链：javac + java + mj2x-cli + vcvars64（与 stackc_backend_e2e_test 一致）。
// ---------------------------------------------------------------------------
struct Toolchain
{
    bool        available = false;
    std::string reason;
    fs::path    javac;
    fs::path    java;
    fs::path    mj2xCli;
    fs::path    jdkInclude;
    fs::path    jdkIncludeWin;
    fs::path    vcvars64;
};

Toolchain probeToolchain()
{
    Toolchain tc;
    ProbeOptions opts;
    opts.requireJniHeader = true;
    auto jdk = probeJdk(opts);
    if (!jdk.available) {
        tc.reason = jdk.reason;
        return tc;
    }
    tc.javac         = jdk.javac;
    tc.java          = jdk.java;
    tc.jdkInclude    = jdk.jdkInclude;
    tc.jdkIncludeWin = jdk.jdkIncludeWin;

    tc.mj2xCli = test::getBuildTargetPath("mj2x-cli");
    if (!fileExists(tc.mj2xCli)) {
        tc.reason = "mj2x-cli.exe 未构建（请先 xmake build mj2x-cli）。期望路径: " +
                    tc.mj2xCli.string();
        return tc;
    }

    tc.vcvars64 = jdk_finder::findVcvarsAll();
    if (tc.vcvars64.empty()) {
        tc.reason = "未找到 VS vcvars64.bat（需要 MSVC 编译生成的 .c）";
        return tc;
    }

    tc.available = true;
    return tc;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string              current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        }
        else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

// ---------------------------------------------------------------------------
// Fixture：每个后端参数化跑同一个 round-trip。
// ---------------------------------------------------------------------------
class UserIntrinsicE2E : public ::testing::TestWithParam<const char*>
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) {
            GTEST_SKIP() << "E2E 工具链不可用，跳过: " << tc_.reason;
        }
    }

    fs::path caseDir(const std::string& name) const
    {
        return test::getOutPath("mj2x/user_intrinsic_e2e/" + name + "/.keep").parent_path();
    }

    void writeFile(const fs::path& path, const std::string& content) const
    {
        std::ofstream out(path);
        ASSERT_TRUE(out) << path;
        out << content;
    }

    void runCase(const char* backendTag)
    {
        const std::string backend(backendTag);
        fs::path          dir = caseDir("user_intr_" + backend);
        std::error_code   ec;
        fs::create_directories(dir, ec);
        ASSERT_FALSE(ec) << ec.message();

        // 1. Java 源：F.m 的 Java 实现返回 a+b；UserIntr.run 调用它。
        writeFile(dir / "F.java",
                  "public class F { public static int m(int a, int b) { return a + b; } }\n");
        writeFile(dir / "UserIntr.java",
                  "public class UserIntr {\n"
                  "  public static int run(int a, int b) { return F.m(a, b); }\n"
                  "}\n");
        // 2. 用户 native 实现：a*1000+b（可观测地区别于 Java 实现）。
        writeFile(dir / "my_m.c",
                  "#include <stdint.h>\n"
                  "int32_t my_m(int32_t a, int32_t b) { return a * 1000 + b; }\n");
        // 3. intrinsic 配置 JSON（sources 用绝对路径，编译时按 cwd 解析）。
        const std::string myM = (dir / "my_m.c").string();
        std::string       escaped;
        for (char c : myM) escaped += (c == '\\') ? "\\\\" : std::string(1, c);
        writeFile(dir / "cfg.json",
                  "{\"intrinsics\":[{\"owner\":\"F\",\"name\":\"m\",\"desc\":\"(II)I\","
                  "\"symbol\":\"my_m\",\"passContext\":false,\"mayThrow\":false}],"
                  "\"sources\":[\"" + escaped + "\"]}\n");

        // 4. javac 编译。
        auto javac = runBat({q(tc_.javac) + " -d " + q(dir) + " " + q(dir / "F.java") + " " +
                             q(dir / "UserIntr.java")});
        ASSERT_EQ(javac.exitCode, 0) << javac.out;

        // 5. mj2x-cli：改写 + 生成 C + 编译 DLL（--compile 直接模式，用户源自动并入）。
        auto cli = runBat({q(tc_.mj2xCli) + " -i " + q(dir / "UserIntr.class") +
                           " -o " + q(dir / "UserIntr_out.class") +
                           " --emit-c " + q(dir / "native.c") +
                           " --backend " + backend +
                           " --intrinsic-config " + q(dir / "cfg.json") +
                           " --compile --c-compiler msvc"});
        ASSERT_EQ(cli.exitCode, 0) << "mj2x-cli 失败:\n" << cli.out;
        ASSERT_TRUE(fileExists(dir / "UserIntr_out.dll")) << "未生成 DLL:\n" << cli.out;
        // 生成 C 必须含用户 symbol 调用（结构断言：调用点被降级）。
        {
            std::ifstream in(dir / "native.c");
            std::ostringstream oss;
            oss << in.rdbuf();
            EXPECT_NE(oss.str().find("my_m("), std::string::npos)
                << "generated C must call user symbol my_m\n"
                << oss.str();
        }

        // 6. 用改写类替换原类 + Driver 加载 DLL 运行。
        fs::copy_file(dir / "UserIntr_out.class", dir / "UserIntr.class",
                      fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();
        writeFile(dir / "Driver.java",
                  "public class Driver {\n"
                  "  public static void main(String[] args) throws Exception {\n"
                  "    String dll = System.getenv(\"MORPHER_DLL\");\n"
                  "    if (dll != null) System.load(dll);\n"
                  "    System.out.println(\"run=\" + UserIntr.run(2, 3));\n"
                  "  }\n"
                  "}\n");
        auto driverCompile = runBat({q(tc_.javac) + " -cp " + q(dir) + " -d " + q(dir) + " " +
                                     q(dir / "Driver.java")});
        ASSERT_EQ(driverCompile.exitCode, 0) << driverCompile.out;

        auto run = runBat({
            "set \"MORPHER_DLL=" + (dir / "UserIntr_out.dll").string() + "\"",
            q(tc_.java) + " -cp " + q(dir) + " Driver",
        });
        ASSERT_EQ(run.exitCode, 0) << "java 运行失败:\n" << run.out;
        const auto lines = splitLines(run.out);
        ASSERT_EQ(lines.size(), 1u) << run.out;
        // my_m(2,3) = 2*1000+3 = 2003（Java 实现是 5；降级必须生效）。
        EXPECT_EQ(lines[0], "run=2003") << run.out;
    }
};

Toolchain UserIntrinsicE2E::tc_;

}   // namespace

TEST_P(UserIntrinsicE2E, StaticCallSiteLowersToUserSymbolAtRuntime)
{
    runCase(GetParam());
}

INSTANTIATE_TEST_SUITE_P(BackendMatrix, UserIntrinsicE2E,
                         ::testing::Values("stackc", "regc", "vmc4"),
                         [](const testing::TestParamInfo<const char*>& info) {
                             return std::string(info.param);
                         });
