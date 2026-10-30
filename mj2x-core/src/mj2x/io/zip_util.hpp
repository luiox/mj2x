/// @file        zip_util.hpp
/// @brief       压缩包解压工具（ZIP / tar / tar.xz / tar.gz / tar.zst 等）。
///
/// 移植自 mjbase model 的 resource_io::ZipUtil（libarchive 解包后端，与 mbase
/// 同源），服务于"下载后解压"场景（dev_plan 交叉编译切片的 bundled zig /
/// toolchain 获取面）。libca::zip 是 JVM 语义的 ZIP 条目读取器（无 tar 面、
/// 不落盘），与本工具的整包落盘解压互补，不重复。
///
/// 错误语义：源侧 ModelResult<T, ModelError> 按仓约定改写为 libca
/// Result<void, ZipError> 值语义；码表只收 extract 面实际产生的五个
/// （mjbase ModelError 的 BadArgument/TypeMismatch 属 jar 资源模型面，不随迁）。
/// 原 mbase throw 文本 → 错误码的映射见实现侧注释。
#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

namespace mj2x {

/// @brief 解包错误码（ZipUtil 专用小码表）。
enum class ZipError : ca::u8
{
    Io,                ///< 目标目录创建失败 / 条目文件打开写出失败
    NotFound,          ///< 归档文件不存在 / extractFile 未命中条目
    BadEntry,          ///< 条目头或条目数据读取失败
    PathEscape,        ///< 条目路径逃逸目标目录（zip-slip 防线）
    UnsupportedFormat, ///< 归档格式/过滤器嗅探失败
};

/// @brief 错误码 → 短名（日志/断言输出用）。
inline const char* to_string(ZipError code) noexcept
{
    switch (code) {
    case ZipError::Io:                return "Io";
    case ZipError::NotFound:          return "NotFound";
    case ZipError::BadEntry:          return "BadEntry";
    case ZipError::PathEscape:        return "PathEscape";
    case ZipError::UnsupportedFormat: return "UnsupportedFormat";
    }
    return "Unknown";
}

/// @brief ZipUtil 结果别名。
template <typename T>
using ZipResult = ca::core::Result<T, ZipError>;

/// @brief 压缩包解压工具（ZIP / tar / tar.xz / tar.gz / tar.zst 等）。
///
/// 格式识别走 libarchive 内容嗅探（support_filter_all + support_format_all，
/// 按 bytes 而非文件后缀识别），天然覆盖 zip / tar / tar.xz / tar.gz / tar.zst
/// 等组合，无需手工 detectFormat。路径一律按 UTF-8 解释（std::filesystem::u8path，
/// Windows 上不依赖系统 ACP）。
class ZipUtil
{
public:
    /// @brief 自动识别格式并解压到目标目录（目录不存在则创建）。
    ///
    /// 错误码：Io（目标目录创建失败/条目写出失败）、NotFound（归档文件不存在）、
    /// UnsupportedFormat（格式/过滤器嗅探失败）、BadEntry（条目头或数据读取失败）、
    /// PathEscape（条目路径逃逸目标目录，zip-slip 防线）。
    static ZipResult<void> extract(std::string_view archivePath, std::string_view targetDir);
    /// @brief 解压 ZIP 文件（委托 extract；保留独立入口与 mjbase API 面对齐）。
    static ZipResult<void> unzip(std::string_view zipPath, std::string_view targetDir);
    /// @brief 解压压缩包中指定的单个文件（条目名需与归档内 pathname 完全匹配，
    ///        条目内相对路径结构保留）。未命中条目报 NotFound。
    static ZipResult<void> extractFile(std::string_view path, std::string_view file,
                                       std::string_view destination);
};

}   // namespace mj2x
