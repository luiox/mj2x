// vmx64 native E2E harness (Phase 2)：Java source -> javac -> mj2x-cli (vmx64)
// -> xmake 工程（模板 add_files 收编 .asm 经 ml64 汇编 + 注册 C TU）-> DLL
// -> Java System.load + RegisterNatives 绑定 -> 调用结果与 JVM 基线对拍。
//
// 与 vmc4_backend_e2e_test.cpp 的差别：
//   - 产物是 .asm（MASM x64）而非 C：执行路径必须经生成工程（xmake 模板的
//     .asm 汇编步骤 + ml64），不直接驱动 cl。
//   - 驱动真实 CLI 二进制（mtest_set_native_matrix 模式）：一条 --compile 走完
//     门改造 → 临时 xmake 工程 → 模板渲染（@VMX64_ASM_RULE@）→ xmake 配置构建
//     （-p windows --toolchain=msvc -a x64）→ DLL 落位全链路。
//   - 另跑一轮 --generate-xmake（不构建）做工程结构断言：vmx64.asm /
//     vmx64_reg.c / xmake.lua 的注入段；两轮 --emit-c 产物逐字节相同（确定性）。
//
// 本测试验证 Phase 2 交付面：emit-only → 可执行（Java -> JNI 入口 PROC ->
// 加密 handler 链 -> Java 闭环）。
//
// CI 跑法（windows 作业自动全量收编；定向）：
//   xmake run mj2x_unittest --gtest_filter='Vmx64BackendE2E.*'

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "jdk_fixture.hpp"
#include "jdk_finder/jdk_finder.hpp"
#include "test_helper.hpp"

namespace fs = std::filesystem;

using namespace test::jdk;   // runBat / q / fileExists / ExecResult / JdkToolchain / probeJdk

namespace {

// vmx64 E2E 工具链：JDK（javac/java/jni.h）+ MSVC（vcvars64 探测，xmake msvc
// 工具链/ml64 的可用性信号）+ xmake（PATH）+ mj2x-cli 二进制（伴生构建）。
struct Toolchain
{
    bool        available = false;
    std::string reason;
    fs::path    javac;
    fs::path    java;
    fs::path    jdkInclude;
    fs::path    jdkIncludeWin;
    fs::path    cli;
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

    // ml64/vs 可用性探测（照 vmc4 e2e 惯例）：vcvars 定位失败 = 无 MSVC，
    // xmake 无法以 msvc 工具链配置——SKIP 而非 FAIL。
    if (jdk_finder::find_vcvars_all().empty()) {
        tc.reason = "vcvars64.bat not found; MSVC (ml64 via xmake msvc toolchain) is required";
        return tc;
    }

    // xmake（PATH 探测）：--compile 的临时工程构建驱动。
    if (runBat({q("xmake") + " --version"}).exitCode != 0) {
        tc.reason = "xmake not found on PATH; the vmx64 compile path drives an xmake project";
        return tc;
    }

    tc.cli = test::getBuildTargetPath("mj2x-cli");
    if (!fileExists(tc.cli)) {
        tc.reason = "mj2x-cli not built: " + tc.cli.string();
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

std::string joinLines(const std::vector<std::string>& lines)
{
    std::string out;
    for (const auto& line : lines) {
        out += "  ";
        out += line;
        out += "\n";
    }
    return out;
}

std::string readText(const fs::path& path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

/// 受保护的样本类：全部静态、i32/i64 纯算术 + 控制流（Phase 2 最小 opcode 面；
/// 实例方法/字段/浮点 fail-closed 保留字节码，不进 E2E 断言面）。
const char* kSampleSource = R"JAVA(
public class Vmx64Sample {
    static int add(int a, int b) { return a + b; }
    static long lmul(long a, long b) { return a * b; }
    static int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s += i;
        return s;
    }
}
)JAVA";

const char* kDriverSource = R"JAVA(
public class Driver {
    public static void main(String[] args) {
        String dll = System.getenv("MORPHER_DLL");
        if (dll != null) System.load(dll);
        System.out.println(Vmx64Sample.add(19, 23));
        System.out.println(Vmx64Sample.lmul(123456789L, 987654321L));
        System.out.println(Vmx64Sample.sum(100));
        System.out.println(Vmx64Sample.add(-7, 7));
    }
}
)JAVA";

class Vmx64BackendE2E : public ::testing::Test
{
protected:
    static Toolchain tc_;

    static void SetUpTestSuite() { tc_ = probeToolchain(); }

    void SetUp() override
    {
        if (!tc_.available) {
            GTEST_SKIP() << "vmx64 native E2E toolchain unavailable: " << tc_.reason;
        }
    }

    fs::path caseDir(const std::string& name) const
    {
        return test::getOutPath("mj2x/vmx64_e2e/" + name + "/.keep").parent_path();
    }
};

Toolchain Vmx64BackendE2E::tc_;

// ===========================================================================
// 主用例：生成工程结构断言（--generate-xmake，不构建）+ 执行链路
//（--compile 临时工程真构建）+ JVM 基线对拍 + 跨调用确定性。
// ===========================================================================
TEST_F(Vmx64BackendE2E, XmakeProjectExecMatchesJvmBaseline)
{
    std::error_code ec;
    const fs::path  dir = caseDir("sample");
    fs::remove_all(dir, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec) << ec.message();

    // --- Java 源落盘 + 编译 ---
    const fs::path javaFile = dir / "Vmx64Sample.java";
    {
        std::ofstream out(javaFile);
        out << kSampleSource;
    }
    const fs::path driverFile = dir / "Driver.java";
    {
        std::ofstream out(driverFile);
        out << kDriverSource;
    }
    const auto javac = runBat({q(tc_.javac) + " -encoding UTF-8 -d " + q(dir) + " " +
                               q(javaFile) + " " + q(driverFile)});
    ASSERT_EQ(javac.exitCode, 0) << javac.out;

    // --- JVM 基线 ---
    const auto baselineRun = runBat({q(tc_.java) + " -cp " + q(dir) + " Driver"});
    ASSERT_EQ(baselineRun.exitCode, 0) << baselineRun.out;
    const auto baseline = splitLines(baselineRun.out);
    ASSERT_EQ(baseline.size(), 4u) << baselineRun.out;

    const fs::path inClass  = dir / "Vmx64Sample.class";
    const fs::path outClass = dir / "Vmx64Sample_protected.class";

    // --- 第一轮：--generate-xmake 持久工程（不构建），断言工程结构 ---
    const fs::path project = dir / "project";
    {
        const auto generate =
            runBat({q(tc_.cli) + " -i " + q(inClass) + " -o " + q(outClass) +
                    " --backend vmx64 --vmx64-seed 0x5EED --generate-xmake " + q(project)});
        ASSERT_EQ(generate.exitCode, 0) << generate.out;
    }
    ASSERT_TRUE(fileExists(project / "xmake.lua"));
    ASSERT_TRUE(fileExists(project / "src" / "generated" / "vmx64.asm"))
        << "default generated source name must be vmx64.asm for the vmx64 backend";
    ASSERT_TRUE(fileExists(project / "src" / "generated" / "vmx64_reg.c"))
        << "RegisterNatives binding TU must be written into src/generated";

    const std::string xmakeLua = readText(project / "xmake.lua");
    // vmx64 专属段注入：ml64 汇编分流 + 平台硬校验（on_config 的 raise 文案——
    // is_plat/add_files 在共享模板正文已有出现，不能作注入证据；描述作用域
    // 沙箱没有 raise/assert，装配器校验只能在 on_config 脚本作用域）。
    EXPECT_NE(xmakeLua.find("add_files(\"src/generated/*.asm\")"), std::string::npos)
        << xmakeLua;
    EXPECT_NE(xmakeLua.find("mj2x vmx64: generated MASM x64 sources require windows/x64"),
              std::string::npos)
        << xmakeLua;
    // 模板 token 必须全部被消费（漏注入 = 半成品工程）。
    EXPECT_EQ(xmakeLua.find("@VMX64_ASM_RULE@"), std::string::npos) << xmakeLua;
    EXPECT_NE(xmakeLua.find("MJ2X_RT_BACKEND_VMX64"), std::string::npos) << xmakeLua;

    const std::string asmText = readText(project / "src" / "generated" / "vmx64.asm");
    EXPECT_NE(asmText.find(".code"), std::string::npos)
        << "shared handler block must live inside a segment";
    EXPECT_NE(asmText.find("proc frame"), std::string::npos) << asmText;
    EXPECT_NE(asmText.find("public mj2x_vmx64_exec_"), std::string::npos) << asmText;
    EXPECT_NE(asmText.find("dq mj2x_vmx64_h_"), std::string::npos) << asmText;

    const std::string regText = readText(project / "src" / "generated" / "vmx64_reg.c");
    EXPECT_NE(regText.find("#include <jni.h>"), std::string::npos) << regText;
    EXPECT_NE(regText.find("extern jint mj2x_vmx64_exec_"), std::string::npos) << regText;
    EXPECT_NE(regText.find("_mj2x_register_for_class"), std::string::npos) << regText;
    EXPECT_NE(regText.find("JNI_OnLoad"), std::string::npos) << regText;
    EXPECT_NE(regText.find("RegisterNatives"), std::string::npos) << regText;

    // --- 第二轮：--compile（临时 xmake 工程，真构建到 DLL）---
    // --emit-c 显式路径产物留档：与第一轮工程内 .asm 逐字节相同（同 seed
    // 跨进程调用的确定性契约）。
    const fs::path asmArtifact = dir / "vmx64_run2.asm";
    const fs::path dll        = dir / "Vmx64Sample_protected.dll";
    {
        const auto compile = runBat(
            {"set \"JAVA_HOME=" + tc_.java.parent_path().parent_path().string() + "\"",
             q(tc_.cli) + " -i " + q(inClass) + " -o " + q(outClass) +
                 " --backend vmx64 --vmx64-seed 0x5EED --emit-c " + q(asmArtifact) +
                 " --compile"});
        ASSERT_EQ(compile.exitCode, 0) << compile.out;
    }
    ASSERT_TRUE(fileExists(dll)) << "native library must land next to the output class";
    EXPECT_EQ(readText(asmArtifact), asmText)
        << "same seed + same input must be byte-identical across CLI invocations";

    // --- Java 调用保护类（System.load -> JNI_OnLoad -> clinit 注册）---
    // 有界执行护栏（CI 实证：VM 死循环 = java 永不返回 = runBat 永等 = 烧满
    // 6h job timeout 才见分晓）。PowerShell WaitForExit 上限 120s，超时杀进程
    // 退出码 124——已落盘的部分 stdout/stderr 即"挂死发生在哪次调用"的现场。
    fs::copy_file(outClass, inClass, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    const fs::path ps1    = dir / "run_bounded.ps1";
    const fs::path outTxt = dir / "driver_stdout.txt";
    const fs::path errTxt = dir / "driver_stderr.txt";
    {
        std::ofstream out(ps1);
        out << "$env:MORPHER_DLL = $args[0]\n"
            << "$p = Start-Process -FilePath $args[1] "
               "-ArgumentList '-XX:-CreateCoredumpOnCrash','-cp',$args[2],'Driver' "
               "-WorkingDirectory $args[2] -NoNewWindow "
               "-RedirectStandardOutput $args[3] -RedirectStandardError $args[4] -PassThru\n"
            << "if (-not $p.WaitForExit(120000)) {\n"
            << "  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue\n"
            << "  Write-Output 'VM RUN TIMEOUT (120s): native call did not return'\n"
            << "  exit 124\n"
            << "}\n"
            << "exit $p.ExitCode\n";
    }
    const auto protectedRun =
        runBat({std::string("powershell -NoProfile -ExecutionPolicy Bypass -File ") + q(ps1) +
                " " + q(dll) + " " + q(tc_.java) + " " + q(dir) + " " + q(outTxt) + " " +
                q(errTxt)});
    // 失败现场全量转储：hs_err_pid*.log（崩溃寄存器/故障指令——JVM 崩在
    // native 内时它是唯一寄存器级现场）+ 发射 .asm + CLI 构建日志。
    const auto crashDiag = [&]() {
        std::string d = "\n--- driver stdout ---\n";
        d += fileExists(outTxt) ? readText(outTxt) : "<none>";
        d += "\n--- driver stderr ---\n";
        d += fileExists(errTxt) ? readText(errTxt) : "<none>";
        d += "\n--- runner out ---\n" + protectedRun.out;
        std::error_code lec;
        // hs_err 由 java 落在其 CWD（Start-Process 已指到 case 目录）；测试进程
        // 自身 CWD（仓库根）兜底再扫一遍。
        const fs::path hsDirs[2] = {dir, fs::current_path()};
        for (const auto& hsDir : hsDirs) {
            for (fs::directory_iterator it(hsDir, lec), endIt; it != endIt && !lec; ++it) {
                const std::string name = it->path().filename().string();
                if (name.rfind("hs_err_pid", 0) == 0) {
                    d += "\n--- " + name + " (first 120 lines) ---\n";
                    std::vector<std::string> lines =
                        splitLines(readText(it->path()));
                    for (size_t i = 0; i < lines.size() && i < 120; ++i)
                        d += lines[i] + "\n";
                }
            }
        }
        d += "\n--- emitted vmx64.asm ---\n";
        d += fileExists(asmArtifact) ? readText(asmArtifact) : "<missing>";
        return d;
    };
    ASSERT_NE(protectedRun.exitCode, 124)
        << "protected Driver timed out after 120s (dispatch loop suspected)" << crashDiag();
    ASSERT_EQ(protectedRun.exitCode, 0) << "protected Driver crashed" << crashDiag();
    const auto actual =
        splitLines(fileExists(outTxt) ? readText(outTxt) : std::string());

    ASSERT_EQ(baseline.size(), actual.size())
        << "baseline:\n" << joinLines(baseline) << "protected:\n" << joinLines(actual)
        << crashDiag();
    for (size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_EQ(baseline[i], actual[i]) << "line " << i;
    }
}

// ===========================================================================
// 门回归：vmx64 fail-closed 面（错误信息断言留在 key 级别，不锁文案细节）。
// ===========================================================================
TEST_F(Vmx64BackendE2E, GatesRejectUnsupportedSurfaces)
{
    std::error_code ec;
    const fs::path  dir = caseDir("gates");
    fs::remove_all(dir, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path javaFile = dir / "Vmx64Sample.java";
    {
        std::ofstream out(javaFile);
        out << kSampleSource;
    }
    const auto javac = runBat({q(tc_.javac) + " -encoding UTF-8 -d " + q(dir) + " " +
                               q(javaFile)});
    ASSERT_EQ(javac.exitCode, 0) << javac.out;
    const fs::path inClass = dir / "Vmx64Sample.class";

    // --emit-c 非 .asm 扩展名：拒绝（扩展名决定工程内的编译分流）。
    {
        const auto r = runBat({q(tc_.cli) + " -i " + q(inClass) + " -o " +
                               q(dir / "Vmx64Sample_out.class") + " --backend vmx64 --emit-c " +
                               q(dir / "not_asm.c")});
        EXPECT_NE(r.exitCode, 0) << r.out;
    }
    // --generate-cmake：模板无 .asm 汇编步骤，仍 fail-closed。
    {
        const auto r = runBat({q(tc_.cli) + " -i " + q(inClass) + " -o " +
                               q(dir / "Vmx64Sample_out.class") + " --backend vmx64" +
                               " --emit-c " + q(dir / "g.asm") + " --generate-cmake " +
                               q(dir / "cmake_proj")});
        EXPECT_NE(r.exitCode, 0) << r.out;
        EXPECT_FALSE(fileExists(dir / "cmake_proj" / "CMakeLists.txt"));
    }
    // --c-compiler 组合：vmx64 恒走 xmake 驱动，direct 编译器选择无意义。
    {
        const auto r = runBat({q(tc_.cli) + " -i " + q(inClass) + " -o " +
                               q(dir / "Vmx64Sample_out.class") + " --backend vmx64" +
                               " --emit-c " + q(dir / "g.asm") + " --compile --c-compiler msvc"});
        EXPECT_NE(r.exitCode, 0) << r.out;
    }
    // 多目标 --targets：多目标管线只懂 C 源。
    {
        const auto r = runBat({q(tc_.cli) + " -i " + q(inClass) + " -o " +
                               q(dir / "Vmx64Sample_out.class") + " --backend vmx64" +
                               " --emit-c " + q(dir / "g.asm") + " --compile --targets windows-x64"});
        EXPECT_NE(r.exitCode, 0) << r.out;
    }
}

}   // namespace
