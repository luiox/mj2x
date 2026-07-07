#pragma once

#include <string>

namespace mj2x::cli {

enum class CCompilerKind {
    Auto,
    Msvc,
    Gcc,
    Clang,
};

/**
 * @brief 解析 CLI 传入的 C 编译器名称。
 *
 * 支持 `msvc`、`gcc`、`clang`。解析失败时返回 false,由 CLI 层打印参数错误。
 */
bool parseCompilerName(const std::string& name, CCompilerKind& compiler);

/**
 * @brief 把已生成的 C 源码编译为与输出 class/JAR 同目录的 native library。
 *
 * 编译器可由 @p requestedCompiler 指定,也可自动探测。JNI include 路径从
 * `JAVA_HOME` 推导。失败时向 stderr 打印明确错误并返回非 0。
 */
int compileCSource(
    const std::string& cOutputPath,
    const std::string& outputPath,
    CCompilerKind requestedCompiler);

} // namespace mj2x::cli
