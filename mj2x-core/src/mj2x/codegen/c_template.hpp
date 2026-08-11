/// @file c_template.hpp
/// @brief 命名代码模板：可复用发射片段 + 注册表。
///
/// @details
/// 见 doc_ai/regc_template_engine.md §3.3。CodeTemplate 是"命名 + 复用 + 可验证"
/// 的发射单元：定义时以 lambda 捕获上下文参数（值捕获），实例化 `emit(out)` 即
/// 带参发射。依赖开关用 `dependsOn` 声明（文档/校验用途，不强制运行时检查）。
/// 注册表按名字去重，单测可遍历验证模板存在性与输出。
#pragma once

#include "mj2x/codegen/c_source.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mj2x::codegen {

/// @brief 命名模板：一段可复用发射体（lambda），支持参数捕获与依赖开关声明。
class CodeTemplate
{
public:
    /// @brief 定义模板；body 捕获上下文参数（值捕获，避免悬垂引用）。
    static CodeTemplate define(std::string_view name, CBody body)
    {
        CodeTemplate tpl;
        tpl.name_ = std::string(name);
        tpl.body_ = std::move(body);
        return tpl;
    }

    /// @brief 实例化到流。
    void emit(CSourceStream& out) const
    {
        if (body_) body_(out);
    }

    /// @brief 声明模板依赖的开关名（文档/校验用途）。
    CodeTemplate& dependsOn(std::string_view optionName)
    {
        dependencies_.push_back(std::string(optionName));
        return *this;
    }

    std::string_view name() const { return name_; }

    const std::vector<std::string>& dependencies() const { return dependencies_; }

    /// @brief 是否携带有效发射体。
    explicit operator bool() const { return static_cast<bool>(body_); }

private:
    std::string                          name_;
    std::function<void(CSourceStream&)>  body_;
    std::vector<std::string>             dependencies_;
};

/// @brief 命名模板注册表（名字去重，map 保持稳定顺序便于测试断言）。
class CodeTemplateRegistry
{
public:
    /// @brief 注册模板；同名重复注册返回 false 且保留旧条目。
    bool add(const CodeTemplate& tpl)
    {
        if (templates_.find(std::string(tpl.name())) != templates_.end()) return false;
        templates_.emplace(std::string(tpl.name()), tpl);
        return true;
    }

    /// @brief 按名字查找；未注册返回 nullptr。
    const CodeTemplate* find(std::string_view name) const
    {
        const auto it = templates_.find(std::string(name));
        return it == templates_.end() ? nullptr : &it->second;
    }

    bool empty() const { return templates_.empty(); }

    const std::map<std::string, CodeTemplate>& all() const { return templates_; }

private:
    std::map<std::string, CodeTemplate> templates_;
};

}   // namespace mj2x::codegen
