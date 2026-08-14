#pragma once

// param_registry.hpp
//
// mj2x 配置系统（主线十二）核心：参数注册表单源定义。
// 每个参数的 id/CLI flag/JSON 键/类型/默认值/描述/分组只声明一次，
// 统一驱动：
//   - CLI 解析（main.cpp 查表调用 parseCli）
//   - 配置文件解析（config_file.cpp 查表调用 parseJson）
//   - --help 输出（printHelp，按 group 分组）
//   - --dump-config-schema（JSON Schema draft-07，GUI 配置工具消费）
//   - --dump-config-template（全量键 + 默认值 + 注释的 JSONC 起点模板）
//
// 设计文档：mj2x/doc_ai/config_system_design.md

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "mj2x/codegen/c_options.hpp"
#include "mj2x/mj2x.hpp"
#include "native_compiler.hpp"

namespace mj2x::cli {

/// @brief 选择 native 发射后端。
enum class CEmitterBackend
{
    StackC,
    RegC,
    LlvmIr,
    Vmc4,
};

/// @brief native 编译驱动方式（直接驱动编译器 vs xmake 工程）。
enum class NativeBuildDriver
{
    Direct,
    Xmake,
};

/// @brief CLI/配置文件共享的运行时参数容器。
struct CliOptions
{
    std::string              inputPath;
    std::string              outputPath;
    std::string              cOutputPath;
    std::string              cppOutputPath;
    std::string              llvmOutputPath;
    std::string              nativeDirOutput;
    std::string              cmakeProjectDir;
    std::string              xmakeProjectDir;
    std::string              nativeAnnotation;
    std::vector<std::string> nativeMethodPrefixes;
    CEmitterBackend          cBackendKind     = CEmitterBackend::StackC;
    CCompilerKind            cCompilerKind    = CCompilerKind::Auto;
    CompileMode              compileMode      = CompileMode::File;
    LlvmTool                 llvmTool         = LlvmTool::Clang;
    NativeBuildDriver        buildDriver      = NativeBuildDriver::Direct;
    bool                     backendSelected  = false;
    bool                     compileNative    = false;
    bool                     compileModeSelected = false;
    bool                     compilerSelected = false;
    bool                     llvmToolSelected = false;
    bool                     dumpMIR          = false;
    bool                     regcTraceCalls   = false;
    bool                     regcDirectCalls  = false;
    mj2x::codegen::RegCCommentLevel regcCommentLevel = mj2x::codegen::RegCCommentLevel::Summary;
    bool                     regcSafeEnv      = false;
    bool                     regcSafeEnvDebug = false;
    // issue #829：Cloakwork 借鉴项（默认关闭，RegC 高性能定位）。
    bool                     regcObfuscate       = false;
    bool                     regcNoInline        = false;
    int                      regcCffDead        = 0;
    bool                     regcCffStateEncrypt = false;
    bool                     regcMbaDeep         = false;
    bool                     regcOpaqueZero      = false;
    uint64_t                 regcTransformSeed   = 0;
    std::string              stackcExceptionCheck = "full";
    std::string              vmc4Dispatcher      = "switch_table";
    uint64_t                 vmc4Seed            = 0;
    bool                     vmc4SeedSelected    = false;
    uint64_t                 clientId            = 0;   ///< per-client 唯一派生（0 = 不变）
    std::string              vmc4Granularity     = "class";
    int                      vmc4Alias           = 1;
    int                      vmc4Bogus           = 1;
    int                      vmc4Drift           = 0;
    int                      vmc4DriftPeriod     = 16;
    std::string              vmc4TraceSurface;   ///< 非空时写 expected trace surface 文件
    std::string              runtimeProtection        = "none";   ///< none|log|delay|fake|terminate
    int                      runtimeProtectionStep    = 16;
    int                      runtimeProtectionDelayMs = 500;
    /// 用户自定义 intrinsic 配置（issue #818）。
    std::string              intrinsicConfigPath;
    std::vector<std::string> intrinsicSources;
    std::uint32_t            nativeLibKey         = 0;   ///< native lib 加密密钥（--native-lib-key，hex）
    bool                     nativeLibKeySelected = false;
    std::string              nativeLibPackHook;          ///< 嵌入前外部处理钩子（--native-lib-pack-hook）
    /// 日志级别（配置键 log_quiet / log_verbose；CLI --quiet/--verbose 优先）。
    bool                     quiet                = false;
    bool                     verbose              = false;
    /// CLI 侧内部标志：首个 emit 输出参数出现时清空其他输出（与历史行为一致）。
    bool                     cliEmitOutputSeen    = false;
    bool                     cliNativePrefixSeen  = false;
};

/// @brief 参数类型。
enum class ParamType
{
    Bool,       ///< 无值开关
    Int,        ///< 有符号整数（minValue/maxValue 约束，0 表示无界）
    UInt64,     ///< 无符号 64 位（strtoull 语义，0x 前缀支持）
    Hex,        ///< 16 进制解析（strtoul base 16）
    Enum,       ///< 字符串枚举（enumValues 逗号分隔）
    String,     ///< 字符串
    StringList, ///< 字符串数组（JSON array / CLI 可重复）
};

/// @brief 单参数声明。parseCli/parseJson 由注册表提供，读写 CliOptions。
struct ParamDef
{
    const char* id;          ///< 规范 ID："regc.cff_dead"
    const char* cliFlag;     ///< CLI 参数名："--regc-cff-dead"
    const char* jsonPath;    ///< 配置文件 JSON 键："regc_cff_dead"（snake_case 平铺）
    ParamType   type;
    const char* enumValues;  ///< Enum 用：逗号分隔合法值
    ca::i64     minValue;    ///< Int 用：最小值（0 = 无约束）
    ca::i64     maxValue;    ///< Int 用：最大值（0 = 无约束）
    const char* defaultStr;  ///< 默认值文本（help/schema 展示用）
    const char* description; ///< 描述（help 与 schema description 同源）
    const char* group;       ///< 分组（help 分组标题 / schema 文档）
    const char* requires;    ///< 依赖说明（文档用；运行时校验沿用 main.cpp 交叉校验）
    bool (*parseCli)(CliOptions&, const char* value, std::string& error);
    bool (*parseJson)(CliOptions&, const nlohmann::json& value, std::string& error);
};

/// 全量参数表（param_registry.cpp 定义）。
extern const ParamDef* kParams;
extern const std::size_t kParamCount;

/// @brief 按 CLI flag 查找（无匹配返回 nullptr）。
const ParamDef* findParamByCli(const std::string& cliFlag);

/// @brief 按 JSON 键查找（无匹配返回 nullptr）。
const ParamDef* findParamByJsonPath(const std::string& jsonPath);

/// @brief 解析后端名（stackc/regc/llvm/vmc4 + 兼容别名 cbackend/c）。
bool parseBackendName(const std::string& name, CEmitterBackend& backend);

/// @brief 构建配置 JSON Schema（draft-07，平铺 properties，additionalProperties=false）。
nlohmann::json buildConfigSchemaJson();

/// @brief 打印 --help（按 group 分组，与 schema description 同源）。
void printHelp();

/// @brief 生成全量键 + 默认值 + 注释的 JSONC 模板文本。
std::string buildConfigTemplateText();

}   // namespace mj2x::cli
