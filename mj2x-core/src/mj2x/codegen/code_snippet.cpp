#include "mj2x/codegen/code_snippet.hpp"

#include <stdexcept>

namespace mj2x::codegen {

std::string renderSnippet(std::string_view text, const SnippetVars& vars)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            const size_t end = text.find('}', i + 2);
            if (end != std::string_view::npos) {
                std::string key(text.substr(i + 2, end - (i + 2)));
                auto        found = vars.find(key);
                if (found != vars.end()) {
                    out += found->second;
                }
                else {
                    // 未定义变量属调用方契约违背（漏传 var）：抛异常以便尽早暴露，
                    // 而非把 ${name} 原样留在生成代码里（旧式的静默保留隐患）。
                    throw std::runtime_error(
                        "renderSnippet: undefined variable '${" + key + "}'");
                }
                i = end + 1;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

}   // namespace mj2x::codegen
