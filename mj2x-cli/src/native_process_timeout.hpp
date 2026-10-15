#pragma once

/// @file native_process_timeout.hpp
/// @brief mj2x-cli 内部头：native 编译外部进程墙钟超时（P1-4，对标 jvmtp
///        `compiler.timeout`，秒，0=∞；见 dev_plan §K 批次 1）。
/// @details 自 native_compiler.cpp / native_project_builder.cpp 的外呼等待点
///        抽出（native_env_hygiene.hpp #1023 同模式）：inline 定义使合成断言
///        gtest（mj2x_unittest 的 native_process_timeout_test.cpp）无需链接
///        mj2x-cli 即可直接实例化。libca 0.0.15 subprocess 已具备超时基元
///        （Command::output(OutputOptions) / Child::wait_with_output_for /
///        StatusCode::DEADLINE_EXCEEDED），本头只做 mj2x 侧的策略与文案单源。
///
/// 超时语义（每次外呼独立计时，与 jvmtp 一致）：
///   - `Command::output(OutputOptions)` 站点（编译器/link/vcvars 探针/pack
///     hook）：超时即杀直接子进程（libca kill_on_timeout 默认 true）。
///   - stdin 供料的 pipe 站点（C/LLVM IR/llc）与 xmake 站点：走
///     waitChildWithTimeout——先整树收割再收尸。Windows 上 Child::kill() 只
///     终止直接子进程，孙进程（xmake 拉起的 cl.exe、cmd 拉起的 ping 等）
///     继承的管道写端不关，收尸等待将永远拿不到 EOF，故用 taskkill /T /F
///     按进程树收割；Linux 上 kill 已针对整个进程组（libca 契约）。
///
/// include 顺序约束（同 native_env_hygiene.hpp）：本头 include
/// libca/process/subprocess.hpp，其会 #undef Windows UCRT 的 stdin/stdout/
/// stderr 宏；调用方若需先把全局流固化成 kStdoutStream 之类局部常量，必须
/// 在本头之前完成。**传递引入同样致命**：native_compiler.hpp 含本头，故
/// 任何 include native_compiler.hpp / native_project_generator.hpp 的 TU
/// （native_compiler.cpp / native_project_builder.cpp）也必须把 <cstdio>
/// 与固化块置于首个 include 之前——固化写在项目头之后会直接 C2065。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "libca/core/status.hpp"
#include "libca/log/log_macros.hpp"
#include "libca/process/subprocess.hpp"

namespace mj2x::cli {

/// native 编译外部进程超时值；零表示不限时（等同 libca 的无超时重载）。
using ProcessTimeout = std::chrono::milliseconds;

/// 秒 → 超时值（CLI `--native-timeout` 入口；0 或负值归一为不限时）。
inline ProcessTimeout processTimeoutFromSeconds(int seconds) noexcept
{
    if (seconds <= 0) return ProcessTimeout::zero();
    return std::chrono::duration_cast<ProcessTimeout>(std::chrono::seconds(seconds));
}

/// 超时值的可读文案：整数秒不带小数点（900 → "900s"、1500 → "1.5s"）；
/// 零值显示 "unlimited"。日志与 E2E 断言共用，避免两处口径漂移。
inline std::string timeoutSummary(ProcessTimeout timeout)
{
    if (timeout <= ProcessTimeout::zero()) return "unlimited";
    const double seconds = std::chrono::duration<double>(timeout).count();
    if (seconds == static_cast<double>(static_cast<long long>(seconds)))
        return std::to_string(static_cast<long long>(seconds)) + "s";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1fs", seconds);
    return buffer;
}

/// 记录一次外呼超时（libca 站点已杀直接子进程；pipe/xmake 站点由
/// killProcessTree 收割进程树）。name 如 "C compiler" / "clang" / "llc" /
/// "xmake configure"。
inline void logProcessTimeout(const char* name, ProcessTimeout timeout)
{
    CA_LOGT_ERROR("compile", "{} timed out after {} and was killed.\n", name,
                  timeoutSummary(timeout).c_str());
}

/// `Command::output` 的超时选项：超时即杀（kill_on_timeout 默认 true），
/// 返回 DEADLINE_EXCEEDED。
inline ca::process::OutputOptions outputOptionsWithTimeout(ProcessTimeout timeout) noexcept
{
    ca::process::OutputOptions options;
    options.timeout           = timeout;
    options.kill_on_timeout   = true;
    return options;
}

/// Status 是否为 libca 的超时错误（外呼站点判别"超时被杀"与"启动/等待失败"）。
inline bool isDeadlineExceeded(const ca::core::Status& status) noexcept
{
    return status.code() == ca::core::StatusCode::DEADLINE_EXCEEDED;
}

/// 整树终止子进程并保证直接子进程被回收（幂等，可在收尸前后调用）。
/// @details Windows：先 `taskkill /T /F /PID <id>` 收割进程树（孙进程继承的
///          管道写端随树死一起关，收尸等待才能拿到 EOF），再 Child::kill()
///          兜底（taskkill 缺失/失败时至少终止直接子进程）。taskkill 输出进
///          null 设备，不污染 CLI 日志面；其自身失败不影响主路径。
///          Linux：Child::kill() 已针对整个进程组（libca 契约）。
inline void killProcessTree(ca::process::Child& child)
{
#ifdef _WIN32
    ca::process::Command taskkill("taskkill");
    taskkill.arg("/T").arg("/F").arg("/PID").arg(std::to_string(child.id()));
    taskkill.stdout(ca::process::Stdio::null()).stderr(ca::process::Stdio::null());
    (void)taskkill.output();
#endif
    child.kill();
}

/// 带超时地等待 stdin 供料的子进程并汇总输出。
///
/// @param child    已 spawn、stdin 已交由独立写入线程的 Child。
/// @param name     进程名（错误文案用，如 "C compiler" / "clang" / "llc"）。
/// @param timeout  墙钟超时；零 = 不限时（等同 wait_with_output）。
/// @param output   出口参数：拿到输出（含超时前已排空的部分）时填充；
///                  "未能拿到输出"（等待出错、收尸失败）时保持默认空值。
/// @return true = 已拿到输出（子进程退出码在 output.status，非零退出不是
///         本层错误）；false = 等待出错或超时后被收割收尸失败，错误已由
///         本函数记入 "compile" 日志 target，调用方直接返回非 0 即可。
///
/// 超时路径的 libca 契约：`wait_with_output_for` 超时后已排空的数据留在
/// Child；killProcessTree 关闭子进程侧（整树）管道写端，stdin 写入线程的
/// 阻塞 write 随之失败退出；随后 `wait_with_output()` 续接累积数据并连同
/// 退出状态一并返回，不丢诊断输出。调用方应无条件 writeProcessOutput(output)
/// 再判断返回值——超时前编译器吐出的部分告警对定位卡死源码有价值。
inline bool waitChildWithTimeout(ca::process::Child& child, const char* name,
                                 ProcessTimeout timeout, ca::process::Output& output)
{
    if (timeout <= ProcessTimeout::zero()) {
        auto completed = child.wait_with_output();
        if (completed.is_err()) {
            CA_LOGT_ERROR("compile", "cannot wait for {}: {}\n", name,
                          completed.unwrap_err().to_string().c_str());
            return false;
        }
        output = std::move(completed).unwrap();
        return true;
    }

    auto completed = child.wait_with_output_for(timeout);
    if (completed.is_err()) {
        const ca::core::Status status = completed.unwrap_err();
        if (!isDeadlineExceeded(status)) {
            CA_LOGT_ERROR("compile", "cannot wait for {}: {}\n", name,
                          status.to_string().c_str());
            return false;
        }
        CA_LOGT_ERROR("compile", "{} timed out after {}; killing process tree\n", name,
                      timeoutSummary(timeout).c_str());
        // 先收割再收尸：孙进程持有的管道写端不关，wait_with_output 的 EOF
        // 等待会把"超时"变成"无限期挂起"（见文件头说明）。
        killProcessTree(child);
        auto killed = child.wait_with_output();
        if (killed.is_err()) {
            CA_LOGT_ERROR("compile", "cannot reap {} after timeout: {}\n", name,
                          killed.unwrap_err().to_string().c_str());
            return false;
        }
        output = std::move(killed).unwrap();
        return false;
    }
    output = std::move(completed).unwrap();
    return true;
}

}   // namespace mj2x::cli
