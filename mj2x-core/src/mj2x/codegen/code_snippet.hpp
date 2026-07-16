/// @file code_snippet.hpp
/// @brief Typed source snippet rendering for C/C++ emitting backends.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mj2x::codegen {

using SnippetVars = std::unordered_map<std::string, std::string>;

std::string renderSnippet(std::string_view text, const SnippetVars& vars);

template<typename Key>
class CodeSnippetSet
{
public:
    CodeSnippetSet(std::initializer_list<std::pair<const Key, std::string_view>> items)
    {
        for (const auto& item : items) snippets_.emplace(item.first, std::string(item.second));
    }

    std::string render(Key key, const SnippetVars& vars) const
    {
        auto found = snippets_.find(key);
        if (found == snippets_.end()) return {};
        return renderSnippet(found->second, vars);
    }

private:
    std::unordered_map<Key, std::string> snippets_;
};

}   // namespace mj2x::codegen
