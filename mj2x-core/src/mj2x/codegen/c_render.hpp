#pragma once

// 文本模板渲染(#894):与 c_template.hpp 的 CodeTemplate(结构化发射注册表)
// 互补——本头处理"整文件占位符替换"型模板。模板文本经 resources.embed
// 嵌入(挂载 codegen),生成期做占位符替换与条件块裁剪后拼入产物。
//
// 模板指令:
// - `@NAME@`            占位符,替换为 values[name](未提供的名字报错)
// - `@#IF:FLAG@` / `@#ELSE@` / `@#ENDIF@` 各占一行的条件块,按 flags 裁剪;
//   可嵌套;ELSE 分支可省略
//
// 约定:占位符/指令名匹配 [A-Z_][A-Z0-9_]*;替换是纯文本拼接,不做任何转义。

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mj2x::codegen {

/// 渲染模板:values 提供占位符文本,flags 提供条件块开关。
/// 未知占位符/悬空指令视为模板缺陷,返回 false 并填充 err。
///
/// 字节扫描语义:占位符 @NAME@ 在任意位置内联替换;指令(@#IF:/@#ELSE@/@#ENDIF@)
/// 必须独占一行(含行尾换行),渲染后整行(连同换行)消失、不产出任何字节;
/// 其余文本逐字节透传——模板内文本自带的换行不做任何增删。
inline bool renderCTemplate(std::string_view tpl,
                            const std::unordered_map<std::string, std::string>& values,
                            const std::unordered_map<std::string, bool>& flags,
                            std::string& out, std::string& err)
{
    out.clear();
    out.reserve(tpl.size());

    // 指令栈:每个活跃 IF 记录"本分支是否在输出中"
    struct Frame
    {
        bool branchTaken;   // 已进入过真分支(IF 或 ELSE)
        bool emitting;      // 当前是否输出
    };
    std::vector<Frame> stack;
    auto emitting = [&] {
        for (const auto& f : stack) {
            if (!f.emitting) return false;
        }
        return true;
    };

    // 指令识别:token 从行首开始、到行尾结束(CRLF 容错)
    auto atLineStart = [&](size_t p) { return p == 0 || tpl[p - 1] == '\n'; };
    auto lineEnd = [&](size_t p) {
        size_t e = tpl.find('\n', p);
        size_t raw = (e == std::string_view::npos) ? tpl.size() : e;
        while (raw > p && (tpl[raw - 1] == '\r' || tpl[raw - 1] == ' ')) --raw;
        return raw;   // 指令 token 的结束位置(不含 \r/空格)
    };

    size_t pos = 0;
    size_t lineBegin = 0;      // 当前所在行的起始(pos 回溯不到更早)
    bool lastWasNewline = true;

    while (pos < tpl.size()) {
        const char c = tpl[pos];
        if (c != '@') {
            if (!emitting()) {
                ++pos;
                continue;
            }
            out.push_back(c);
            lastWasNewline = (c == '\n');
            ++pos;
            continue;
        }

        // 尝试指令:必须处于行首
        if (lastWasNewline) {
            const size_t lend = lineEnd(pos);
            const std::string_view token(tpl.data() + pos, lend - pos);
            if (token == "@#ELSE@") {
                if (stack.empty()) {
                    err = "@#ELSE without @#IF";
                    return false;
                }
                auto& f = stack.back();
                f.emitting = !f.branchTaken;
                f.branchTaken = true;
                pos = (lend < tpl.size()) ? lend + 1 : lend;   // 跳过整行含换行
                lastWasNewline = pos < tpl.size() ? false : true;
                continue;
            }
            if (token == "@#ENDIF@") {
                if (stack.empty()) {
                    err = "@#ENDIF without @#IF";
                    return false;
                }
                stack.pop_back();
                pos = (lend < tpl.size()) ? lend + 1 : lend;
                lastWasNewline = pos < tpl.size() ? false : true;
                continue;
            }
            if (token.size() >= 6 && token.compare(0, 5, "@#IF:") == 0 &&
                token.back() == '@') {
                const auto name = token.substr(5, token.size() - 6);
                const auto it = flags.find(std::string{name});
                if (it == flags.end()) {
                    err = "unknown template flag: " + std::string{name};
                    return false;
                }
                stack.push_back({it->second, it->second});
                pos = (lend < tpl.size()) ? lend + 1 : lend;
                lastWasNewline = pos < tpl.size() ? false : true;
                continue;
            }
        }

        // 占位符 @NAME@
        const size_t close = tpl.find('@', pos + 1);
        if (close == std::string_view::npos) {
            err = "unterminated placeholder";
            return false;
        }
        const auto name = tpl.substr(pos + 1, close - pos - 1);
        const auto it = values.find(std::string{name});
        if (it == values.end()) {
            err = "unknown template value: " + std::string{name};
            return false;
        }
        if (emitting()) out.append(it->second);
        pos = close + 1;
        lastWasNewline = false;
    }
    (void)lineBegin;
    if (!stack.empty()) {
        err = "unclosed @#IF";
        return false;
    }
    return true;
}

}   // namespace mj2x::codegen
