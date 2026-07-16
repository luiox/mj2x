/// @file regc_runtime_template.hpp
/// @brief RegC runtime C-source definitions emitted into the generated TU.
///
/// @details
/// 这三个发射函数原先内嵌在 regc_backend.cpp 的匿名 namespace 里（共约 914 行
/// `out << "..."`），是 RegC 运行时 ABI 的 C 定义（field/method 访问 helper、
/// 异常分发、数组/分配/monitor、string/parseFloat/parseDouble intrinsic 等）。
/// 它们不依赖任何 backend 私有状态，所有缓存耦合都通过 finalize() 预解析的
/// 字符串参数传入，故抽出到此独立文件，让 regc_backend.cpp 只保留 SSA→RegIR→
/// Emitter 的 facade 和 finalize 编排。
///
/// 生成的 C 源在此文件搬移前后必须字节一致：raw-string 字面量、调用顺序、
/// cacheDeclarations 的 guard 都保持原样。
#pragma once

#include <sstream>
#include <string>

namespace mj2x {

/// 发射 _mj2x_regc_*_field（static/instance × load/store 四象限）helper 定义。
void emitRegCFieldAccessHelpers(std::ostringstream& out);

/// 发射 _mj2x_regc_call_* / _mj2x_regc_method_id helper 定义。
void emitRegCMethodCallHelpers(std::ostringstream& out);

/// @brief 发射 RegC 运行时主体（除上面两组 helper 外的全部 ABI 定义）。
///
/// @details 12 个 cacheName 参数是 finalize() 从 RegCCachePool 预解析出的符号名，
/// 作为子串内插到 `_mj2x_regc_cached_class(&<name>)` 调用里。cacheDeclarations
/// 非空时作为缓存变量声明块原样前插。内部依次调用上面两个函数。
void emitRuntimeHelpers(std::ostringstream& out, const std::string& cacheDeclarations,
                        const std::string& stringClassCacheName,
                        const std::string& objectToStringMethodCacheName,
                        const std::string& stringValueOfFloatMethodCacheName,
                        const std::string& stringValueOfDoubleMethodCacheName,
                        const std::string& floatParseFloatMethodCacheName,
                        const std::string& doubleParseDoubleMethodCacheName,
                        const std::string& nullPointerExceptionClassCacheName,
                        const std::string& classCastExceptionClassCacheName,
                        const std::string& outOfMemoryErrorClassCacheName,
                        const std::string& stringIndexOutOfBoundsClassCacheName,
                        const std::string& unsupportedOperationExceptionClassCacheName);

}   // namespace mj2x
