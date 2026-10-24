#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "native_process_timeout.hpp"

namespace mj2x::cli {

/// @brief 选择 native 发射后端。
enum class CEmitterBackend
{
    StackC,
    RegC,
    LlvmIr,
    Vmc4,
};

enum class CCompilerKind
{
    Auto,
    Msvc,
    Gcc,
    Clang,
    Zig,
};

/// @brief 多目标产物 ABI（issue #11，--targets）。
///
/// @details
/// v1 注册表四项（issue #11 的目标清单；darwin/mach 生态后续按需补）。
/// 每项决定：产物扩展名、JDK include 平台子目录（jni.h 下的 jni_md.h 按
/// 平台选）、zig cc -target 三元组（zig 自带交叉 libc，是唯一的仓内默认
/// 交叉面；gcc/clang 交叉由用户显式提供预配置好的 cross 工具链——issue #11
/// 方案 (a)）。host ABI 由编译期平台宏推导，恒可走自动探测。
enum class TargetAbi
{
    WindowsX64,
    LinuxX64,
    LinuxAarch64,
    AndroidArm64,
};

/// @brief ABI 注册表条目。
struct TargetAbiInfo
{
    const char* id;             ///< CLI/manifest 标识（如 "windows-x64"）
    const char* librarySuffix;  ///< 产物扩展名（".dll" / ".so"）
    const char* jniPlatform;    ///< JDK include 平台子目录（"win32" / "linux"）
    const char* zigTriple;      ///< zig cc -target 三元组
};

/// @brief 解析 --targets 传入的 ABI 标识；未知返回 false（调用方给合法清单）。
bool parseTargetAbi(const std::string& id, TargetAbi& out);

/// @brief ABI 注册表条目。
const TargetAbiInfo& targetAbiInfo(TargetAbi abi);

/// @brief 全部合法 ABI 标识（逗号分隔，错误文案用）。
std::string targetAbiIdList();

/// @brief 宿主 ABI（编译期平台宏推导）。
TargetAbi hostTargetAbi();

/// @brief 从可执行文件路径推断编译器种类（--target-cc 显式路径用）：
/// cl*.exe → Msvc、zig* → Zig、*gcc* → Gcc、*clang* → Clang。
/// 推断失败返回 false（调用方 fail-closed 报错）。
bool inferCompilerKindFromPath(const std::filesystem::path& executable, CCompilerKind& out);

/// @brief 多目标编译规格（issue #11）：显式产物路径与显式编译器路径。
///
/// @details
/// libraryPath 非空时绕开「outputPath → 宿主扩展名」推导（多目标每 ABI 一个
/// 路径）；compilerPath 非空时绕开 PATH 探测（--target-cc 显式工具链）。
struct CompileTargetSpec
{
    TargetAbi             abi          = TargetAbi::WindowsX64;
    std::filesystem::path libraryPath;   ///< 产物库路径（显式）
    std::string           compilerPath;  ///< 编译器可执行文件（显式；空 = 探测）
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

/// @brief 编译期后端可用性（issue #886 单后端编译裁剪面）。
///
/// 全量构建（不带 with_mj2x_backend 选择器）恒为 true，行为与历史一致；
/// 单后端构建仅所选后端为 true，被裁后端的类不参与编译（二进制无其符号）。
/// 显式传入被裁后端名在参数解析期即被拒绝；本函数拦截默认值路径
/// （如 stackc 被裁时 --emit-c 未带 --backend，cBackendKind 仍为默认 StackC）。
constexpr bool backendEnabled(CEmitterBackend backend)
{
#if defined(MJ2X_DISABLE_STACKC_BACKEND)
    if (backend == CEmitterBackend::StackC) return false;
#endif
#if defined(MJ2X_DISABLE_REGC_BACKEND)
    if (backend == CEmitterBackend::RegC) return false;
#endif
#if defined(MJ2X_DISABLE_LLVM_BACKEND)
    if (backend == CEmitterBackend::LlvmIr) return false;
#endif
#if defined(MJ2X_DISABLE_VMC4_BACKEND)
    if (backend == CEmitterBackend::Vmc4) return false;
#endif
    return true;
}

/// @brief 后端的 CLI 名称（错误文案用，与 --backend 取值一致）。
inline std::string_view backendCliName(CEmitterBackend backend)
{
    switch (backend) {
    case CEmitterBackend::StackC: return "stackc";
    case CEmitterBackend::RegC: return "regc";
    case CEmitterBackend::LlvmIr: return "llvm";
    case CEmitterBackend::Vmc4: return "vmc4";
    }
    return {};
}

/// @brief 本构建可用的后端 CLI 名清单（逗号分隔，错误文案用）。
///
/// 注册表驱动（backend_registry.cpp）：顺序 = displayOrder（stackc, regc,
/// llvm, vmc4），与历史宏拼接清单逐字节一致；被裁后端不注册、不入清单。
std::string enabledBackendCliList();

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
/// @param backend 决定参与编译的 mj2x-rt 挂载子集与注入的后端宏
///        (embedded_rt.hpp 分发面)。
/// @param extraSources 用户自定义 intrinsic 实现文件（issue #818），追加进编译。
/// @param processTimeout 外部进程墙钟超时（每次外呼独立计时；0 = 不限时）。
///        见 native_process_timeout.hpp（P1-4）。
int compileCSource(const std::string& cOutputPath, const std::string& outputPath,
                   CEmitterBackend backend, CCompilerKind requestedCompiler,
                   const std::filesystem::path& crtRoot = {},
                   const std::vector<std::string>& extraSources = {},
                   ProcessTimeout processTimeout = {});

/// @brief 把已生成的 C++17 源码编译为与输出 class/JAR 同目录的 native library。
int compileCppSource(const std::string& cppOutputPath, const std::string& outputPath,
                     CEmitterBackend backend, CCompilerKind requestedCompiler,
                     const std::filesystem::path& crtRoot = {},
                     const std::vector<std::string>& extraSources = {},
                     ProcessTimeout processTimeout = {});

/// @brief 编译内存中的 C 源码；GCC/Clang/Zig 使用 stdin，MSVC 使用并清理临时源文件。
int compileCSourceFromPipe(std::string_view cSource, const std::string& outputPath,
                           CEmitterBackend backend, CCompilerKind requestedCompiler,
                           const std::filesystem::path& crtRoot = {},
                           const std::vector<std::string>& extraSources = {},
                           ProcessTimeout processTimeout = {});

/// @brief 把 textual LLVM IR 编译为与输出 class/JAR 同目录的 native library。
///
/// LLVM IR 由后端直接产生 JNI ABI wrapper，不需要 C/JNI include；round-2 起
/// intrinsic 直调（Math/包装类/Random/StringBuilder）需要 mj2x-rt 源码包
/// （crtRoot，--compile 无条件解析），与 RegC --compile 的 crt 契约一致。
/// `clang` 直接读取 `.ll`；`llc` 先生成临时 object，再由 clang 链接。失败时返回非 0。
/// @param withRuntimeProtection 为 true 时把内嵌 mj2x-rt rt_protect 源写到临时
///        目录并一起编译（--runtime-protection 检查点的实现侧）。
/// @param crtRoot 把 mj2x-rt 源一并编译（JDK intrinsic 直调的 runtime
///        符号，如 mj2x_math_*）。
/// @param processTimeout 外部进程墙钟超时（每次外呼独立计时；0 = 不限时）。
int compileLlvmIrSource(const std::string& llvmOutputPath, const std::string& outputPath,
                        LlvmTool tool, bool withRuntimeProtection,
                        const std::filesystem::path& crtRoot = {},
                        ProcessTimeout processTimeout = {});

/// @brief 编译内存中的 LLVM IR 源码（--compile-mode pipe）。
///
/// `clang` 经 stdin（`-x ir -`）直接链接；`llc` 经 stdin 生成临时 object 再由
/// clang 链接。失败时返回非 0。
/// @param withRuntimeProtection 同 compileLlvmIrSource。
/// @param crtRoot 同 compileLlvmIrSource。
/// @param processTimeout 同 compileLlvmIrSource。
int compileLlvmIrFromPipe(std::string_view llvmSource, const std::string& outputPath,
                          LlvmTool tool, bool withRuntimeProtection,
                          const std::filesystem::path& crtRoot = {},
                          ProcessTimeout processTimeout = {});

/// @brief 宿主编译器可用性探测（多目标 skip/failed 分类用，issue #11）：
/// requested 为 Auto 时按宿主优先序探测，否则探测指定种类。
bool probeHostCompiler(CCompilerKind requested);

/// @brief 编译器种类的 CLI 名（"msvc"/"gcc"/"clang"/"zig"；Auto 返回空）。
std::string_view compilerKindName(CCompilerKind kind);

/// @brief 按 ABI 目标编译 C 源文件（issue #11 多目标；文件模式）。
///
/// @details
/// 与 compileCSource 的差异全部由 @p target 驱动：产物路径显式给定；JNI include
/// 平台子目录、shared/PIC/链接 flag 按目标 ABI 而非宿主宏推导；zig 恒带
/// `-target <三元组>`。target.compilerPath 非空时直接用作编译器（按文件名推断
/// 种类，推断失败 fail-closed）；为空时按 @p requestedCompiler 探测（仅宿主
/// ABI 语义合理）。MSVC 只支持 Windows 宿主 + windows-x64 目标。
int compileCSourceForTarget(const std::string& cOutputPath, const CompileTargetSpec& target,
                            CEmitterBackend backend, CCompilerKind requestedCompiler,
                            const std::filesystem::path& crtRoot = {},
                            const std::vector<std::string>& extraSources = {},
                            ProcessTimeout processTimeout = {});

/// @brief 按 ABI 目标编译内存中的 C 源码（issue #11 多目标；pipe 模式）。
///
/// @details
/// 非 MSVC 编译器经 stdin（`-x c -`）喂源；MSVC 走临时文件回退（仅 Windows
/// 宿主 + windows-x64 目标）。其余同 compileCSourceForTarget。
int compileCSourceFromPipeForTarget(std::string_view cSource, const CompileTargetSpec& target,
                                    CEmitterBackend backend, CCompilerKind requestedCompiler,
                                    const std::filesystem::path& crtRoot = {},
                                    const std::vector<std::string>& extraSources = {},
                                    ProcessTimeout processTimeout = {});

}   // namespace mj2x::cli
