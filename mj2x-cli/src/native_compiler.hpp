#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

/// @brief LLVM textual IR 的外部编译路径。
enum class LlvmTool
{
    Clang,
    Llc,
};

/// @brief 解析 CLI 传入的 C 编译器名称。
///
/// 支持 `msvc`、`gcc`、`clang`、`zig`。解析失败时返回 false,由 CLI 层打印参数错误。
bool parseCompilerName(const std::string& name, CCompilerKind& compiler);

/// @brief 解析 CLI 传入的 native 编译输入模式。
bool parseCompileModeName(const std::string& name, CompileMode& mode);

/// @brief 解析 LLVM IR 编译工具名称。
///
/// 支持 `clang`（直接由 LLVM IR 链接动态库）和 `llc`（先生成 object，再由 clang 链接）。
bool parseLlvmToolName(const std::string& name, LlvmTool& tool);

std::filesystem::path nativeLibraryPathForOutput(const std::string& outputPath);

/// @brief 把已生成的 C 源码编译为与输出 class/JAR 同目录的 native library。
///
/// 编译器可由 @p
/// requestedCompiler 指定,也可自动探测。JNI include 路径从
/// `JAVA_HOME` 推导。RegC 调用方传入 @p
/// crtRoot 后,编译器按其 manifest
/// 加入 CRT C 源码与 public include 根。失败时向 stderr
/// 打印明确错误并返回非 0。
/// @param extraSources 用户自定义 intrinsic 实现文件（issue #818），追加进编译。
int compileCSource(const std::string& cOutputPath, const std::string& outputPath,
                   CCompilerKind requestedCompiler, const std::filesystem::path& crtRoot = {},
                   const std::vector<std::string>& extraSources = {});

/// @brief 把已生成的 C++17 源码编译为与输出 class/JAR 同目录的 native library。
int compileCppSource(const std::string& cppOutputPath, const std::string& outputPath,
                     CCompilerKind requestedCompiler,
                     const std::filesystem::path& crtRoot = {},
                     const std::vector<std::string>& extraSources = {});

/// @brief 编译内存中的 C 源码；GCC/Clang/Zig 使用 stdin，MSVC 使用并清理临时源文件。
int compileCSourceFromPipe(std::string_view cSource, const std::string& outputPath,
                           CCompilerKind requestedCompiler,
                           const std::filesystem::path& crtRoot = {},
                           const std::vector<std::string>& extraSources = {});

/// @brief 把 textual LLVM IR 编译为与输出 class/JAR 同目录的 native library。
///
/// LLVM IR 由后端直接产生 JNI ABI wrapper，不需要 C/JNI include；round-2 起
/// intrinsic 直调（Math/包装类/Random/StringBuilder）需要 mj2x-crt 源码包
/// （crtRoot，--compile 无条件解析），与 RegC --compile 的 crt 契约一致。
/// `clang` 直接读取 `.ll`；`llc` 先生成临时 object，再由 clang 链接。失败时返回非 0。
/// @param withRuntimeProtection 为 true 时把内嵌 mj2x-crt rt_protect 源写到临时
///        目录并一起编译（--runtime-protection 检查点的实现侧）。
/// @param crtRoot 把 mj2x-crt 源一并编译（JDK intrinsic 直调的 runtime
///        符号，如 mj2x_math_*）。
int compileLlvmIrSource(const std::string& llvmOutputPath, const std::string& outputPath,
                        LlvmTool tool, bool withRuntimeProtection,
                        const std::filesystem::path& crtRoot = {});

/// @brief 编译内存中的 LLVM IR 源码（--compile-mode pipe）。
///
/// `clang` 经 stdin（`-x ir -`）直接链接；`llc` 经 stdin 生成临时 object 再由
/// clang 链接。失败时返回非 0。
/// @param withRuntimeProtection 同 compileLlvmIrSource。
/// @param crtRoot 同 compileLlvmIrSource。
int compileLlvmIrFromPipe(std::string_view llvmSource, const std::string& outputPath,
                          LlvmTool tool, bool withRuntimeProtection,
                          const std::filesystem::path& crtRoot = {});

}   // namespace mj2x::cli
