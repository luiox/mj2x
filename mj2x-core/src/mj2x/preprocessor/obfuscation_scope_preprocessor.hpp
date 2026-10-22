#pragma once

// obfuscation_scope_preprocessor.hpp
//
// 混淆作用域（issue #8）：按类级 glob 判定"这个类受不受混淆变换影响"。
//
// jvmtp 对标：混淆开关在真实工程里不能一开就作用于全部编译输入——性能、产物
// 体积、可调试性都要求把作用域收窄到少数核心类。
//
// 判定结果落到 `CompilerClass::inObfuscationScope`（方案 (b)，见 issue #8）：
// pass 与 backend 只问"这个类在不在作用域内"，与 RegIR 完全解耦，StackC /
// VMC4 也能复用同一标记，不必每个后端各实现一遍。
//
// 模式语义复用 native_class_filter_preprocessor 的 `classPassesFilter`
// （`*` 不跨段、`**` 跨段、`?` 单字符；exclude 优先于 include），不新造一套。

#include "mj2x/preprocessor/native_class_filter_preprocessor.hpp"
#include "mj2x/preprocessor/preprocessor.hpp"

#include "libca/log/log_macros.hpp"

#include <string>
#include <vector>

namespace mj2x {

/// @brief 给每个类打"是否在混淆作用域内"的标记。
///
/// @details
/// 默认全部在作用域内（不给任何模式时零行为变化）。作用域外的类仍照常编译成
/// native，只是跳过 RegIR 变换——产物与未开混淆时逐字节一致。
///
/// 只打标记、不改方法体：与 NativeClassFilterPreprocessor（决定是否进入 native
/// 编译集）正交——一个是"编译不编译"，一个是"编译出来的要不要混淆"。
class ObfuscationScopePreprocessor : public Preprocessor
{
public:
    ObfuscationScopePreprocessor(std::vector<std::string> include,
                                 std::vector<std::string> exclude)
        : include_(std::move(include))
        , exclude_(std::move(exclude))
    {
    }

    void process(CompilerIndex& index) override
    {
        std::size_t total   = 0;
        std::size_t scoped  = 0;
        for (auto& [name, clazz] : index.getClasses()) {
            if (clazz.isLibrary) continue;
            ++total;
            // 默认全部在作用域内（不给任何模式时零行为变化）。
            const bool inScope = classPassesFilter(name, include_, exclude_);
            clazz.inObfuscationScope = inScope;
            if (inScope) ++scoped;
        }
        outOfScope_ = total - scoped;
        CA_LOGT_INFO("preprocess", "Obfuscation scope: {} in scope, {} out of {} class(es)\n",
                     scoped, outOfScope_, total);
        if (!include_.empty() && scoped == 0 && total > 0) {
            // include 非空却一个都没中：模式拼错是最常见原因，值得一条警告。
            CA_LOGT_WARN("preprocess",
                         "Obfuscation scope: --obf-scope-include matched 0 of {} class(es); "
                         "nothing will be obfuscated\n",
                         total);
        }
    }

    /// @brief 本轮落在作用域外的类数（日志/测试用）。
    std::size_t outOfScopeCount() const { return outOfScope_; }

private:
    std::vector<std::string> include_;
    std::vector<std::string> exclude_;
    std::size_t              outOfScope_ = 0;
};

}   // namespace mj2x
