#pragma once

#include <filesystem>
#include <string>

namespace mj2x::cli {

enum class CCompilerKind
{
    Auto,
    Msvc,
    Gcc,
    Clang,
    Zig,
};

enum class CompileMode
{
    File,
    Pipe,
};

/// @brief 解析 CLI 传入的 C 编译器名称。
///
/// 支持 `msvc`、`gcc`、`clang`、`zig`。解析失败时返回 false,由 CLI 层打印参数错误。
bool parseCompilerName(const std::string& name, CCompilerKind& compiler);

/// @brief 解析 CLI 传入的 native 编译输入模式。
bool parseCompileModeName(const std::string& name, CompileMode& mode);

std::filesystem::path nativeLibraryPathForOutput(const std::string& outputPath);

/// @brief 把已生成的 C 源码编译为与输出 class/JAR 同目录的 native library。
///
/// 编译器可由 @p
/// requestedCompiler 指定,也可自动探测。JNI include 路径从
/// `JAVA_HOME` 推导。RegC 调用方传入 @p
/// crtRoot 后,编译器按其 manifest
/// 加入 CRT C 源码与 public include 根。失败时向 stderr
/// 打印明确错误并返回非 0。
int compileCSource(const std::string& cOutputPath, const std::string& outputPath,
                   CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot = {});

/// @brief 编译内存中的 C 源码；GCC/Clang/Zig 使用 stdin，MSVC 使用并清理临时源文件。
int compileCSourceFromPipe(const std::string& cSource, const std::string& outputPath,
                           CCompilerKind requestedCompiler,
                           const std::filesystem::path& crtRoot = {});

}   // namespace mj2x::cli
