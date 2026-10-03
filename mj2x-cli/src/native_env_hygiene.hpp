#pragma once

/// @file native_env_hygiene.hpp
/// @brief mj2x-cli 内部头：子进程环境形状卫生函数簇（#1023）。
/// @details 自 native_compiler.cpp / native_project_builder.cpp 的匿名
/// namespace 与手动前置声明纯搬移而来（行为零改动，函数体逐字节保持），
/// 转为 inline 使合成 env 断言 gtest（mj2x_unittest 的
/// native_env_hygiene_test.cpp）无需链接 mj2x-cli 即可直接调用。

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#    include <strings.h>
#else
// GetEnvironmentStringsW/WideCharToMultiByte：currentProcessEnvironment 的
// 父进程环境枚举用（仿 native_project_builder.cpp 的守卫口径）。
#    include <string.h>
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#endif

#include "libca/log/log_macros.hpp"
#include "libca/process/subprocess.hpp"

namespace mj2x::cli {

/// 捕获/合并两侧共用的环境键值对列表形状。
using ProcessEnvironment = std::vector<std::pair<std::string, std::string>>;

/// 环境键 case-insensitive 比较（Windows 环境变量键大小写不敏感口径）。
inline bool environmentKeyEquals(const std::string& left, const char* right)
{
#ifdef _WIN32
    return _stricmp(left.c_str(), right) == 0;
#else
    return strcasecmp(left.c_str(), right) == 0;
#endif
}

/// 父进程当前环境块快照（UTF-8 键值对）。
/// @details Windows 侧用 GetEnvironmentStringsW + CP_UTF8 转换——libca 子进程
/// 环境要求 UTF-8（无效序列注入会被整体拒绝，spawn 失败），故不用 ANSI 版；
/// 转换按替换语义（标志 0）而非 WC_ERR_INVALID_CHARS：极端脏值宁可单条劣化
/// 也不整条丢弃（丢 PATH 会让子进程彻底失明）。键含 '=' 的 "=C:" 型伪变量
/// 整条跳过——Command::env 键禁止含 '='，注入会让 spawn 直接失败。
inline std::vector<std::pair<std::string, std::string>> currentProcessEnvironment()
{
    std::vector<std::pair<std::string, std::string>> result;
#ifdef _WIN32
    wchar_t* inherited = ::GetEnvironmentStringsW();
    if (inherited == nullptr) return result;
    for (const wchar_t* entry = inherited; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        if (*entry == L'=') continue;
        const wchar_t* separator = std::wcschr(entry, L'=');
        if (separator == nullptr) continue;
        const auto keyLength   = static_cast<std::size_t>(separator - entry);
        const auto valueLength = std::wcslen(separator + 1);
        if (keyLength == 0) continue;
        const auto toUtf8 = [](const wchar_t* value, std::size_t length) {
            if (length == 0) return std::string{};
            const auto size = ::WideCharToMultiByte(
                CP_UTF8, 0, value, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
            if (size <= 0) return std::string{};
            std::string converted(static_cast<std::size_t>(size), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length),
                                  converted.data(), size, nullptr, nullptr);
            return converted;
        };
        result.emplace_back(toUtf8(entry, keyLength), toUtf8(separator + 1, valueLength));
    }
    ::FreeEnvironmentStringsW(inherited);
#else
    // 非 Windows：loadMsvcEnvironment 恒产空 environment（case 1 整块继承），
    // mergeProcessEnvironment 对空 captured 直接返回，本枚举不可达；留空即可
    // （macOS 的 environ 枚举需 _NSGetEnviron，无谓引入平台差异）。
#endif
    return result;
}

/// PATH/INCLUDE/LIB 路径族键判定（case-insensitive）。
inline bool isPathFamilyKey(const std::string& key)
{
    return environmentKeyEquals(key, "PATH") || environmentKeyEquals(key, "INCLUDE") ||
           environmentKeyEquals(key, "LIB");
}

/// 路径族变量值内目录去重（splitenv→unique→joinenv，保序取首个）。
/// @details 重复目录不改变查找语义，只无谓拉长 env 块、放大子进程 env 块
/// 构造的边界敏感性（A1 报告 §5.2：xmake pinned tbox 的填充 off-by-one 与
/// 块内容形状相关）。Windows 下分隔符 ';' 且比较大小写不敏感（与文件系统
/// 口径一致）；非 Windows 分隔符 ':'、精确比较——该路径实际不可达
/// （captured 恒空，mergeProcessEnvironment 早退），仅为编译完备。
inline std::string dedupePathValue(const std::string& value)
{
#ifdef _WIN32
    const char separator = ';';
    const auto same    = [](const std::string& left, const std::string& right) {
        return _stricmp(left.c_str(), right.c_str()) == 0;
    };
#else
    const char separator = ':';
    const auto same    = [](const std::string& left, const std::string& right) {
        return left == right;
    };
#endif
    std::vector<std::string> unique;
    std::istringstream       stream(value);
    std::string              entry;
    while (std::getline(stream, entry, separator)) {
        if (entry.empty()) continue;
        const auto duplicate =
            std::any_of(unique.begin(), unique.end(),
                        [&](const std::string& known) { return same(known, entry); });
        if (duplicate) continue;
        unique.push_back(entry);
    }
    std::string joined;
    for (const auto& item : unique) {
        if (!joined.empty()) joined += separator;
        joined += item;
    }
    return joined;
}

/// 合并后 env 块字节预算（按 "KEY=VALUE" + '\0' 逐项累计）。计数为 UTF-8
/// 字节近似（Windows 实际环境块是 UTF-16），且未计块尾终止 NUL 的 1 字节
/// ——常数级偏差，不影响判级用途。超限只记 WARN：
/// 现阶段收缩手段仅去重/剔除非法条目（不损失语义）；若仍超限需要删条目，
/// 删除策略须先评审再实现（A1 报告 P0-A：统计性去险，非语义损失删除）。
constexpr std::size_t kMergedEnvironmentBytesLimit = 16 * 1024;

/// @brief 把捕获的 MSVC 环境（loadMsvcEnvironment 产物）以「父进程环境为底、
///        捕获变量覆盖其上」的方式注入子进程命令。
/// @details 调用点显式构造完整环境块：父进程当前环境铺底，vcvars 捕获值按
/// case-insensitive 同名胜出（PATH/INCLUDE/LIB 必须是 vcvars 版本，否则丢
/// cl.exe）。Command::env 的语义是「覆盖集」，子进程环境块的继承行为属
/// libca 内部实现细节（现行为是继承底+覆盖），调用方不依赖该细节。captured
/// 为空（loadMsvcEnvironment case 1：INCLUDE 已设）时不动 env_——空 env_ 即
/// 整块继承，行为保持不变。注入前做形状卫生（A1 报告 P0-A）：路径族值
/// （PATH/INCLUDE/LIB）目录去重（保序取首个）、剔除空值条目与含 '=' 的键
/// ——vcvars `set` 文本解析产物没有 currentProcessEnvironment 的伪变量
/// 防御，含 '=' 的键会让 spawn 整体失败；captured 来源的空值覆盖（VAR=）
/// 同样被剔除——该键不注入，子进程保留父进程原值。显式全量注入契约不变。
inline void mergeProcessEnvironment(ca::process::Command&        command,
                                    const ProcessEnvironment& captured)
{
    if (captured.empty()) return;
    std::size_t totalBytes = 0;
    const auto  inject     = [&](const std::string& key, const std::string& value) {
        if (key.empty() || value.empty() || key.find('=') != std::string::npos) return;
        const std::string deduped = isPathFamilyKey(key) ? dedupePathValue(value) : value;
        totalBytes += key.size() + deduped.size() + 2;
        command.env(key, deduped);
    };
    for (const auto& [key, value] : currentProcessEnvironment()) {
        const auto overridden = std::any_of(
            captured.begin(), captured.end(),
            [&](const auto& item) { return environmentKeyEquals(item.first, key.c_str()); });
        if (overridden) continue;
        inject(key, value);
    }
    for (const auto& [key, value] : captured) inject(key, value);
    if (totalBytes > kMergedEnvironmentBytesLimit) {
        CA_LOGT_WARN("compile",
                     "merged child environment block is {} bytes (limit {}); "
                     "path-family dedup applied, no entries dropped.\n",
                     totalBytes, kMergedEnvironmentBytesLimit);
    }
}

#if defined(_WIN32)
/// 子进程异常退出判定（Windows NTSTATUS 失败码，如堆损坏 0xC0000374 →
/// -1073740940）：libca 带符号化后是量级极大的负 int；常规错误退出
/// （xmake exit(-1) 等）是小幅负值。<= -1000 即可区分两类，-1/-2 不触发重试。
inline bool isAbnormalChildExit(int code)
{
    return code <= -1000;
}
#endif

}   // namespace mj2x::cli
