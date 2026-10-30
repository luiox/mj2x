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
#include "libca/opt/opt.hpp"
#include "mj2x/codegen/c_options.hpp"
#include "mj2x/mj2x.hpp"
#include "native_compiler.hpp"

namespace mj2x::cli {

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
    /// 生成 native 工程的优化级别（`0`/`1`/`2`/`3`/`s`），默认 "2"。
    /// 控制 CFF 平坦化会不会被宿主编译器优化掉——见 capabilities §3 CFF 边界。
    std::string              nativeOptLevel = "2";
    std::string              nativeAnnotation;
    std::vector<std::string> nativeMethodPrefixes;
    /// 类级 glob 过滤器（对标 jvmtp filter.*）：include 非空时只编译命中的类，
    /// exclude 命中的类一律剔除（exclude 优先）。模式为 `.` 分隔的内部类名，
    /// `*` 不跨段、`**` 跨段；`/` 归一化为 `.`。见 native_class_filter_preprocessor.hpp。
    std::vector<std::string> nativeClassIncludes;
    std::vector<std::string> nativeClassExcludes;
    /// 混淆作用域（issue #8）：按类级 glob 收窄"哪些类受 RegIR 变换影响"。
    /// 语义与 nativeClassIncludes/Excludes 一致（exclude 优先、include 空 = 全部）。
    /// 两者都为空时零行为变化。
    std::vector<std::string> obfScopeIncludes;
    std::vector<std::string> obfScopeExcludes;
    /// native 编译外部进程墙钟超时（秒；0 = 不限时，默认 900，对标 jvmtp
    /// `compiler.timeout`）。每次外呼（编译器/link/llc/vcvars 探针/xmake/
    /// pack hook）独立计时，超时整树终止并以非 0 退出。见
    /// native_process_timeout.hpp。
    int                      nativeTimeoutSec   = 900;
    CEmitterBackend          cBackendKind     = CEmitterBackend::StackC;
    CCompilerKind            cCompilerKind    = CCompilerKind::Auto;
    CompileMode              compileMode      = CompileMode::File;
    LlvmTool                 llvmTool         = LlvmTool::Clang;
    NativeBuildDriver        buildDriver      = NativeBuildDriver::Direct;
    bool                     backendSelected  = false;
    bool                     compileNative    = false;
    /// --targets（issue #11）：多目标 ABI 列表（windows-x64,linux-x64,...）。
    /// 非空即多目标模式：一次输入逐 ABI 产物 + manifest（--compile 语义不变）。
    std::vector<std::string> targets;
    /// --target-cc（issue #11）："abi=path" 显式交叉工具链（方案 a）；
    /// 未配置的非宿主 ABI 记 skipped 并在 manifest/汇总报告。
    std::vector<std::string> targetCc;
    bool                     compileModeSelected = false;
    bool                     compilerSelected = false;
    bool                     llvmToolSelected = false;
    bool                     dumpMIR          = false;
    /// 帧换装 LCA 供给面：classpath 依赖 jar（os 路径分隔符分隔；空 = 不供）。
    /// 供谱系图扩容，帧重算公共父类求解从 Object 兜底变真 LCA。
    std::string              lcaClasspath;
    /// 关闭 JDK 谱系扫描（默认开；jmods 扫描带磁盘缓存，首次几秒）。
    bool                     noJdkHierarchy   = false;
    bool                     regcTraceCalls   = false;
    /// native→native 直调档位（--regc-direct-calls，默认 "off"）。
    /// 存字符串而非枚举值：注册表是纯声明式的参数表，枚举解析与档位映射统一
    /// 由 DEF_ENUM_STR + main 的一次转换承担，不在此处引入自定义类型。
    std::string              regcDirectCalls   = "off";
    /// POD 类降级为 C struct（--regc-pod-struct；默认关）。
    bool                     regcPodStruct     = false;
    /// computed jumps / 跳转目标间接化（--regc-computed-jumps；默认关）。
    bool                     regcComputedJumps = false;
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
    bool                     regcHideConstants   = false;   ///< 立即数隐藏（obfuscation.constants，不被 --regc-obfuscate 联动）
    bool                     regcIndirectCalls   = false;   ///< 调用目标间接化（obfuscation.calls.*，不被 --regc-obfuscate 联动）
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
    bool                     vmc4EncryptProgram  = false; ///< VMC4 程序 blob at-rest 加密
    bool                     vmc4LazyDecrypt     = false; ///< VMC4 程序 blob 分块懒解密（要求 encrypt_program）
    bool                     vmc4BlockLazyDecrypt = false; ///< VMC4 块粒度链式懒解密（要求 encrypt_program；与 lazy_decrypt 互斥，#1116 切片 B）
    bool                     vmc4ShuffleBlocks   = false; ///< VMC4 编码期块乱序（issue #1116 切片 A）
    bool                     vmc4SuperOps        = false; ///< VMC4 super-operator 融合（issue #9）
    bool                     vmc4Fragment        = false; ///< VMC4 指令碎片化（issue #9）
    int                      vmc4Regfile         = 0;     ///< VMC4 寄存器文件定长化池顶（0 = 关，issue #10）
    bool                     stringsEncrypt      = false; ///< 字符串面 at-rest 加密
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
    bool                     cliNativeIncludeSeen = false;
    bool                     cliNativeExcludeSeen = false;
    bool                     cliObfScopeIncludeSeen = false;
    bool                     cliObfScopeExcludeSeen = false;
};

/// @brief 参数类型。
enum class ParamType
{
    Bool,       ///< 无值开关
    Int,        ///< 非负整数（恒拒负值；minValue/maxValue 追加界，0 表示无追加界）
    UInt64,     ///< 无符号 64 位（恒拒负值字面量；strtoull 语义，0x 前缀支持）
    Hex,        ///< 16 进制解析（恒拒负号；strtoul base 16）
    Enum,       ///< 字符串枚举（enumValues 逗号分隔）
    String,     ///< 字符串
    StringList, ///< 字符串数组（JSON array / CLI 可重复）
};

/// @brief 单参数声明。parseCli/parseJson 由注册表提供,读写 CliOptions。
///
/// 用户可见文案(描述/分组标题)不入本表:描述经 mj2x.param.<id>.desc、分组经
/// mj2x.param.group.<group> 查 i18n 单源表(translations/*.lang),help/schema/
/// template 三表面同源本地化。
struct ParamDef
{
    const char* id;          ///< 规范 ID:"regc.cff_dead"
    const char* cliFlag;     ///< CLI 参数名:"--regc-cff-dead"
    const char* jsonPath;    ///< 配置文件 JSON 键:"regc_cff_dead"(snake_case 平铺)
    ParamType   type;
    const char* enumValues;  ///< Enum 用:逗号分隔合法值
    ca::i64     minValue;    ///< Int 用:最小值(0 = 无约束)
    ca::i64     maxValue;    ///< Int 用:最大值(0 = 无约束)
    const char* defaultStr;  ///< 默认值文本(help/schema 展示用)
    const char* group;       ///< 分组 slug(help 标题经 mj2x.param.group.<slug> 查表)
    const char* requires;    ///< 依赖的 CLI flag 名(文档用;运行时校验沿用 main.cpp 交叉校验)
    bool (*parseCli)(CliOptions&, const char* value, std::string& error);
    bool (*parseJson)(CliOptions&, const nlohmann::json& value, std::string& error);
};

/// @brief 全量参数表（运行时拼装：静态基表 + 各后端模块参数段，段序 = 注册表
///        displayOrder；首次访问在 main() 启动后，接线单元注册已完成）。
///        行序即 --help 分组 / schema 属性 / 模板键的展示序。
const ParamDef* params();

/// @brief 全量参数表行数。
std::size_t paramCount();

/// @brief 按 CLI flag 查找（无匹配返回 nullptr）。
const ParamDef* findParamByCli(const std::string& cliFlag);

/// @brief 按 JSON 键查找（无匹配返回 nullptr）。
const ParamDef* findParamByJsonPath(const std::string& jsonPath);

/// @brief CLI flag → ca::opt canonical 名（剥除前导 '-'，如 "--regc-seed"→
///        "regc-seed"、"-i"→"i"）。注册表驱动构建 opt Command 与解析后
///        按来源回查注册表共用。
std::string optCanonicalName(const char* cliFlag);

/// @brief 解析后端名（stackc/regc/llvm/vmc4 + 兼容别名 cbackend/c）。
bool parseBackendName(const std::string& name, CEmitterBackend& backend);

/// @brief 注册表 + CLI 特殊参数 → ca::opt 根命令定义（CLI 解析单源）。
///
/// 参数类型映射：Bool→Flag、StringList→StringList、其余→String——值语义
/// 校验（Int 的 base0/范围、UInt64/Hex/Enum）统一留在 parseCli 单一校验源，
/// opt 只做 argv 结构归一。特殊参数：config/lang（String）、
/// dump-config-schema/template（OptionalString：裸=stdout，=file 写文件，
/// 二者互斥）。--help/-h 为 opt 内建，HelpRequested 由调用方接自研帮助。
ca::opt::Command buildOptCommand();

/// @brief 构建配置 JSON Schema（draft-07，平铺 properties，additionalProperties=false）。
nlohmann::json buildConfigSchemaJson();

/// @brief 打印 --help（按 group 分组，与 schema description 同源）。
void printHelp();

/// @brief 生成全量键 + 默认值 + 注释的 JSONC 模板文本。
std::string buildConfigTemplateText();

}   // namespace mj2x::cli
