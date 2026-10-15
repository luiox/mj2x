// native_process_timeout_test.cpp
//
// native 编译外部进程超时策略的断言（P1-4，对标 jvmtp compiler.timeout）。
// 被测函数在 mj2x-cli/src/native_process_timeout.hpp（inline 定义，与
// native_env_hygiene.hpp #1023 同模式）；本测试经跨模块 include 直接实例化，
// 不链接 mj2x-cli（unittest 与 CLI 无符号关系，见 unittest/xmake.lua 的
// mj2x-cli-companion 注记）。
//
// 超时路径走行为面（真子进程）：
//   - 卡死进程树必须被整树收割后收尸：Windows 上 Child::kill() 只终止直接
//     子进程，孙进程（cmd 的 ping / sh 的 sleep）继承的管道写端不关，
//     wait_with_output 的 EOF 等待会把"超时"变成"无限期挂起"。
//     KillsAndReapsSleepingProcessTree 用 std::async + wait_for 把这一
//     "挂起"回归转成确定性失败（future 故意泄漏：阻塞线程不得拖住测试
//     进程退出）。
//   - 快路径 / 限时内自然退出：输出汇总与退出码原样返回。

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "libca/core/status.hpp"
#include "mj2x-cli/src/native_process_timeout.hpp"

namespace {

using mj2x::cli::isDeadlineExceeded;
using mj2x::cli::outputOptionsWithTimeout;
using mj2x::cli::ProcessTimeout;
using mj2x::cli::processTimeoutFromSeconds;
using mj2x::cli::waitChildWithTimeout;

ca::core::StatusResult<ca::process::Child> spawnPiped(const std::string& program,
                                                      const std::vector<std::string>& args)
{
    ca::process::Command command(program);
    command.args(args);
    command.stdout(ca::process::Stdio::piped()).stderr(ca::process::Stdio::piped());
    return command.spawn();
}

/// 卡死 30s 的子进程：shell 再拉一个孙进程（Windows: cmd → ping；
/// POSIX: sh → sleep），孙进程继承 stdout/stderr 管道写端——整树收割的
/// 必要条件（只杀 shell 则 EOF 永不到达）。
std::pair<std::string, std::vector<std::string>> sleepingChildCommand()
{
#ifdef _WIN32
    const char* comspec = std::getenv("ComSpec");
    return {comspec != nullptr && *comspec != '\0' ? comspec : "cmd.exe",
            {"/d", "/c", "ping -n 31 127.0.0.1"}};
#else
    return {"sh", {"-c", "sleep 30 && echo done"}};
#endif
}

/// 立即回显标记后退出 0 的子进程。
std::pair<std::string, std::vector<std::string>> fastChildCommand()
{
#ifdef _WIN32
    const char* comspec = std::getenv("ComSpec");
    return {comspec != nullptr && *comspec != '\0' ? comspec : "cmd.exe",
            {"/d", "/c", "echo native-process-timeout-ok"}};
#else
    return {"sh", {"-c", "echo native-process-timeout-ok"}};
#endif
}

// ---- 纯函数面 ----

TEST(NativeProcessTimeout, FromSecondsNormalizesNonPositiveToUnlimited)
{
    EXPECT_EQ(processTimeoutFromSeconds(0), ProcessTimeout::zero());
    EXPECT_EQ(processTimeoutFromSeconds(-5), ProcessTimeout::zero());
    EXPECT_EQ(processTimeoutFromSeconds(5), ProcessTimeout(5000));
    EXPECT_EQ(processTimeoutFromSeconds(900), ProcessTimeout(900000));
}

TEST(NativeProcessTimeout, OutputOptionsCarryTimeoutAndKillOnTimeout)
{
    const auto options = outputOptionsWithTimeout(ProcessTimeout(1500));
    EXPECT_EQ(options.timeout, ProcessTimeout(1500));
    EXPECT_TRUE(options.kill_on_timeout);
    // 零值 = libca 的无超时语义（Command::output 重载判别）。
    EXPECT_EQ(outputOptionsWithTimeout(ProcessTimeout::zero()).timeout, ProcessTimeout::zero());
}

TEST(NativeProcessTimeout, IsDeadlineExceededDiscriminatesLibcaStatus)
{
    EXPECT_TRUE(isDeadlineExceeded(ca::core::Status::error(ca::core::StatusCode::DEADLINE_EXCEEDED)));
    EXPECT_FALSE(isDeadlineExceeded(ca::core::Status::ok()));
    EXPECT_FALSE(isDeadlineExceeded(ca::core::Status::error(ca::core::StatusCode::INTERNAL)));
}

// ---- 行为面：真子进程 ----

TEST(NativeProcessTimeout, KillsAndReapsSleepingProcessTree)
{
    auto [program, args] = sleepingChildCommand();
    auto spawned         = spawnPiped(program, args);
    ASSERT_TRUE(spawned.is_ok()) << spawned.unwrap_err().to_string();
    auto child = std::move(spawned).unwrap();

    ca::process::Output output;
    // 金丝雀：缺整树收割时收尸等待无限期挂起。async + wait_for 把挂住转成
    // 确定性失败；ready 前故意不 get（线程可能仍阻塞，future 泄漏即可，
    // 不得让 ~future 的 join 拖住测试进程退出）。
    auto* leased = new std::future<bool>(std::async(std::launch::async, [&child, &output] {
        return waitChildWithTimeout(child, "sleeper", ProcessTimeout(400), output);
    }));
    const auto started = std::chrono::steady_clock::now();
    if (leased->wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
        ADD_FAILURE() << "waitChildWithTimeout did not return within 20s: "
                      << "the sleeping process tree was not reaped (EOF never arrived)";
        return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_FALSE(leased->get());
    EXPECT_LT(elapsed, std::chrono::seconds(10)) << "timeout kill+reap took too long";

    auto waited = child.try_wait();
    ASSERT_TRUE(waited.is_ok());
    EXPECT_TRUE(waited.unwrap().has_value()) << "child must be reaped after timeout";
}

TEST(NativeProcessTimeout, CollectsFastChildOutput)
{
    auto [program, args] = fastChildCommand();
    auto spawned         = spawnPiped(program, args);
    ASSERT_TRUE(spawned.is_ok()) << spawned.unwrap_err().to_string();
    auto child = std::move(spawned).unwrap();

    ca::process::Output output;
    EXPECT_TRUE(waitChildWithTimeout(child, "fast", ProcessTimeout(5000), output));
    EXPECT_NE(output.stdout_data.find("native-process-timeout-ok"), std::string::npos);
    EXPECT_TRUE(output.status.success());
}

TEST(NativeProcessTimeout, ZeroTimeoutMeansUnlimited)
{
    auto [program, args] = fastChildCommand();
    auto spawned         = spawnPiped(program, args);
    ASSERT_TRUE(spawned.is_ok()) << spawned.unwrap_err().to_string();
    auto child = std::move(spawned).unwrap();

    ca::process::Output output;
    EXPECT_TRUE(waitChildWithTimeout(child, "fast", ProcessTimeout::zero(), output));
    EXPECT_TRUE(output.status.success());
}

// 子进程在截止前自然退出：helper 必须立即返回 true 并交出完整输出，
// 而不是把"截止未到"当成"等到截止"。用 echo 子进程（本机实测 cmd+echo
// 冷启 ~3s / 热启 <1s；不用 ping 造"慢子进程"——Windows ping 单发延迟
// 因机器/网络形态波动可达数秒，elapsed 断言会环境性翻车）。10s 截止 +
// 6s elapsed 界：实现若退化为"等满超时才返回"，elapsed 顶到 10s 被捕获。
TEST(NativeProcessTimeout, ReturnsWhenChildFinishesBeforeDeadline)
{
    auto [program, args] = fastChildCommand();
    auto spawned         = spawnPiped(program, args);
    ASSERT_TRUE(spawned.is_ok()) << spawned.unwrap_err().to_string();
    auto child = std::move(spawned).unwrap();

    ca::process::Output output;
    const auto          started = std::chrono::steady_clock::now();
    EXPECT_TRUE(waitChildWithTimeout(child, "fast-before-deadline", ProcessTimeout(10000),
                                     output));
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(6));
    EXPECT_NE(output.stdout_data.find("native-process-timeout-ok"), std::string::npos);
    EXPECT_TRUE(output.status.success());
}

}   // namespace
