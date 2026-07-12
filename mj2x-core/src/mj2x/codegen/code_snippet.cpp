#include "mj2x/codegen/code_snippet.hpp"

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
                    out += text.substr(i, end - i + 1);
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
