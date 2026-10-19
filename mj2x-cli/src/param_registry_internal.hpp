#pragma once

// param_registry_internal.hpp
//
// 参数表声明工具（内部共享）：param_registry.cpp（基表）与 backend_wiring_*.cpp
// （后端参数段）共用的解析回调生成宏与文案工具。生成的 cli_*/json_* 函数为
// TU 内 static（匿名命名空间），跨 TU 不冲突。外部代码一律走 param_registry.hpp
// 的公开面，勿直接 include 本头。

#include "param_registry.hpp"

#include <initializer_list>
#include <sstream>
#include <string>

#include "libca/i18n/i18n.hpp"

namespace mj2x::cli {

/// DEF_* 宏生成的 json_* 回调与后端自定义回调共用的 json 别名。
using json = nlohmann::json;

namespace param_internal {

/// trf 的 std::string 便捷包装:错误回调填 std::string& e 的场景。
inline std::string trfs(const char* key, std::initializer_list<std::string_view> args)
{
    return ca::i18n::trf(key, args).to_std_string();
}

/// 逗号分隔枚举值集合查询。
inline bool enumContains(const char* values, const std::string& v)
{
    std::istringstream in(values);
    std::string        item;
    while (std::getline(in, item, ',')) {
        if (item == v) return true;
    }
    return false;
}

}   // namespace param_internal
}   // namespace mj2x::cli

// ---------- 简单类型参数回调宏 ----------
// 每个宏生成 cli_<member> / json_<member> 两个 static 函数，供参数表引用；
// 须在匿名命名空间内使用（生成的函数依赖 param_internal 的 inline 工具）。

#define DEF_BOOL(member, flag, jp)                                            \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        (void)v;                                                                              \
        (void)e;                                                                              \
        o.member = true;                                                                      \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_boolean()) {                                                                \
            e = param_internal::trfs("mj2x.cli.err.cfg_type_bool", {jp});                           \
            return false;                                                                     \
        }                                                                                     \
        o.member = j.get<bool>();                                                             \
        return true;                                                                          \
    }

#define DEF_STR(member, flag, jp)                                             \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {flag});                                      \
            return false;                                                                     \
        }                                                                                     \
        o.member = v;                                                                         \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_string()) {                                                                 \
            e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {jp});                            \
            return false;                                                                     \
        }                                                                                     \
        o.member = j.get<std::string>();                                                      \
        return true;                                                                          \
    }

#define DEF_INT(member, flag, jp, lo, hi)                                     \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {flag});                                      \
            return false;                                                                     \
        }                                                                                     \
        char*  end  = nullptr;                                                                \
        long long val = std::strtoll(v, &end, 0);                                             \
        if (end == v || *end != '\0' || val < 0 || (lo != 0 && val < lo) || (hi != 0 && val > hi)) { \
            std::string range;                                                          \
            if (lo == 0 && hi == 0) {                                                         \
                range = ">= 0";                                                               \
            }                                                                                 \
            else {                                                                            \
                range = "[" + std::to_string(lo) + "," + std::to_string(hi) + "]";            \
            }                                                                                 \
            e = param_internal::trfs("mj2x.cli.err.invalid_int", {flag, v, range});                                                                         \
            return false;                                                                     \
        }                                                                                     \
        o.member = static_cast<int>(val);                                                     \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_number_integer()) {                                                         \
            e = param_internal::trfs("mj2x.cli.err.cfg_type_int", {jp});                          \
            return false;                                                                     \
        }                                                                                     \
        const long long val = j.get<long long>();                                             \
        if (val < 0 || (lo != 0 && val < lo) || (hi != 0 && val > hi)) {                      \
            std::string range;                                                                 \
            if (lo == 0 && hi == 0) {                                                         \
                range = ">= 0";                                                                \
            }                                                                                  \
            else {                                                                            \
                range = "[" + std::to_string(lo) + "," + std::to_string(hi) + "]";             \
            }                                                                                 \
            e = param_internal::trfs("mj2x.cli.err.cfg_int_range", {jp, range});                              \
            return false;                                                                     \
        }                                                                                     \
        o.member = static_cast<int>(val);                                                     \
        return true;                                                                          \
    }

#define DEF_U64(member, flag, jp)                                             \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {flag});                                      \
            return false;                                                                     \
        }                                                                                     \
        if (v[0] == '-') {                                                                    \
            e = param_internal::trfs("mj2x.cli.err.negative_unsigned",                                        \
                     {flag, v, "[0,18446744073709551615]"});                                  \
            return false;                                                                     \
        }                                                                                     \
        char*      end = nullptr;                                                             \
        const unsigned long long val = std::strtoull(v, &end, 0);                             \
        if (end == v || *end != '\0') {                                                       \
            e = param_internal::trfs("mj2x.cli.err.invalid_value", {flag, v});                          \
            return false;                                                                     \
        }                                                                                     \
        o.member = static_cast<uint64_t>(val);                                                \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (j.is_number_unsigned()) {                                                     \
            o.member = j.get<uint64_t>();                                                     \
            return true;                                                                      \
        }                                                                                     \
        if (j.is_number_integer() && j.get<long long>() >= 0) {                               \
            o.member = static_cast<uint64_t>(j.get<long long>());                             \
            return true;                                                                      \
        }                                                                                     \
        if (j.is_number_integer()) {                                                          \
            e = param_internal::trfs("mj2x.cli.err.negative_unsigned",                                        \
                     {jp, j.dump(), "[0,18446744073709551615]"});                             \
            return false;                                                                     \
        }                                                                                     \
        e = param_internal::trfs("mj2x.cli.err.cfg_type_uint", {jp});                  \
        return false;                                                                         \
    }

#define DEF_HEX(member, flag, jp)                                             \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {flag});                                      \
            return false;                                                                     \
        }                                                                                     \
        if (v[0] == '-') {                                                                    \
            e = param_internal::trfs("mj2x.cli.err.negative_unsigned",                                        \
                     {flag, v, "[0,0xFFFFFFFF]"});                                            \
            return false;                                                                     \
        }                                                                                     \
        char*  end = nullptr;                                                                 \
        const unsigned long val = std::strtoul(v, &end, 16);                                  \
        if (end == v || *end != '\0') {                                                       \
            e = param_internal::trfs("mj2x.cli.err.invalid_hex", {flag, v});                      \
            return false;                                                                     \
        }                                                                                     \
        o.member = static_cast<std::uint32_t>(val);                                           \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (j.is_string()) {                                                                  \
            const std::string s = j.get<std::string>();                                       \
            if (!s.empty() && s[0] == '-') {                                                  \
                e = param_internal::trfs("mj2x.cli.err.negative_unsigned",                                    \
                         {jp, s, "[0,0xFFFFFFFF]"});                                          \
                return false;                                                                 \
            }                                                                                 \
            char*            end = nullptr;                                                   \
            const unsigned long val = std::strtoul(s.c_str(), &end, 16);                      \
            if (end == s.c_str() || *end != '\0') {                                           \
                e = param_internal::trfs("mj2x.cli.err.cfg_invalid_hex", {jp, s});         \
                return false;                                                                 \
            }                                                                                 \
            o.member = static_cast<std::uint32_t>(val);                                       \
            return true;                                                                      \
        }                                                                                     \
        if (j.is_number_unsigned()) {                                                         \
            o.member = j.get<std::uint32_t>();                                                \
            return true;                                                                      \
        }                                                                                     \
        e = param_internal::trfs("mj2x.cli.err.cfg_type_hex", {jp});                  \
        return false;                                                                         \
    }

#define DEF_ENUM_STR(member, flag, jp, values, def)                           \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = param_internal::trfs("mj2x.cli.err.flag_requires_value_enum", {flag, values});                           \
            return false;                                                                     \
        }                                                                                     \
        if (!param_internal::enumContains(values, v)) {                                                \
            e = param_internal::trfs("mj2x.cli.err.unknown_enum", {flag, v, values});                                                                 \
            return false;                                                                     \
        }                                                                                     \
        o.member = v;                                                                         \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_string()) {                                                                 \
            e = param_internal::trfs("mj2x.cli.err.cfg_type_str", {jp});                            \
            return false;                                                                     \
        }                                                                                     \
        const std::string v = j.get<std::string>();                                           \
        if (!param_internal::enumContains(values, v)) {                                                \
            e = param_internal::trfs("mj2x.cli.err.cfg_invalid_enum", {jp, v, values});                                                \
            return false;                                                                     \
        }                                                                                     \
        o.member = v;                                                                         \
        return true;                                                                          \
    }

#define DEF_STR_LIST(member, flag, jp)                                        \
    static bool cli_##member(CliOptions& o, const char* v, std::string& e)                    \
    {                                                                                         \
        if (!v) {                                                                             \
            e = param_internal::trfs("mj2x.cli.err.flag_requires_value", {flag});                                      \
            return false;                                                                     \
        }                                                                                     \
        o.member.push_back(v);                                                                \
        return true;                                                                          \
    }                                                                                         \
    static bool json_##member(CliOptions& o, const json& j, std::string& e)                   \
    {                                                                                         \
        if (!j.is_array()) {                                                                  \
            e = param_internal::trfs("mj2x.cli.err.cfg_type_strarr", {jp});                      \
            return false;                                                                     \
        }                                                                                     \
        o.member.clear();                                                                     \
        for (const auto& item : j) {                                                          \
            if (!item.is_string()) {                                                          \
                e = param_internal::trfs("mj2x.cli.err.cfg_arr_nonstr", {jp});            \
                return false;                                                                 \
            }                                                                                 \
            o.member.push_back(item.get<std::string>());                                      \
        }                                                                                     \
        return true;                                                                          \
    }

/// 参数行声明（kParamTable 段与基表共用字段序）。
#define PARAM(id_, flag_, jp_, type_, enum_, lo_, hi_, def_, grp_, req_, cliFn, jsonFn) \
    { id_, flag_, jp_, type_, enum_, lo_, hi_, def_, grp_, req_, cliFn, jsonFn }
