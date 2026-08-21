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

    size_t pos = 0;
    while (pos < tpl.size()) {
        // 逐行处理:指令必须独占一行
        const size_t eol = tpl.find('\n', pos);
        const size_t lineLen = (eol == std::string_view::npos) ? tpl.size() - pos : eol - pos;
        std::string_view line(tpl.data() + pos, lineLen);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);  // CRLF 容错
        const size_t next = (eol == std::string_view::npos) ? tpl.size() : eol + 1;

        if (line.size() >= 5 && line.compare(0, 5, "@#IF:") == 0) {
            if (line.size() <= 6 || line.back() != '@') {
                err = "malformed @#IF directive";
                return false;
            }
            const auto name = line.substr(5, line.size() - 6);
            const auto it = flags.find(std::string{name});
            if (it == flags.end()) {
                err = "unknown template flag: " + std::string{name};
                return false;
            }
            stack.push_back({it->second, it->second});
            pos = next;
            continue;
        }
        if (line == "@#ELSE@") {
            if (stack.empty()) {
                err = "@#ELSE without @#IF";
                return false;
            }
            auto& f = stack.back();
            f.emitting = !f.branchTaken;
            f.branchTaken = true;
            pos = next;
            continue;
        }
        if (line == "@#ENDIF@") {
            if (stack.empty()) {
                err = "@#ENDIF without @#IF";
                return false;
            }
            stack.pop_back();
            pos = next;
            continue;
        }

        if (emitting()) {
            // 行内占位符扫描
            size_t cursor = 0;
            while (cursor < line.size()) {
                const size_t at = line.find('@', cursor);
                if (at == std::string_view::npos) {
                    out.append(line.substr(cursor));
                    break;
                }
                const size_t close = line.find('@', at + 1);
                if (close == std::string_view::npos) {
                    err = "unterminated placeholder";
                    return false;
                }
                const auto name = line.substr(at + 1, close - at - 1);
                const auto it = values.find(std::string{name});
                if (it == values.end()) {
                    err = "unknown template value: " + std::string{name};
                    return false;
                }
                out.append(line.substr(cursor, at - cursor));
                out.append(it->second);
                cursor = close + 1;
            }
            out.append("\n");
        }
        pos = next;
    }
    if (!stack.empty()) {
        err = "unclosed @#IF";
        return false;
    }
    return true;
}

}   // namespace mj2x::codegen
