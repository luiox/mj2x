/// @defgroup regc_r3_abi RegC R3 运行时 ABI 与 Helper
///
/// RegC 是无栈后端（register-style），其生成的 C 不应在每处翻译单元中保持大型
/// inline helper body。R3 运行时 ABI 是将选定 helper 语义移入可分发 `mj2x-crt`
/// 源码包的边界。
///
/// ## R3 ABI 切片
///
/// 当前 ABI 切片保持小型：
/// - 每条目 `mj2x_regc_context`
/// - JVM 兼容整数除/余 helper
/// - Java `ArithmeticException` 路由（通过生成的 adapter callback）
/// - 运行时 barrier hook（预留给 anti-debug/anti-JVMTI/key 检查）
/// - 基本类型 compare 和基本类型到 `String` 的 helper
///
/// 字段、数组、对象分配、方法调用和异常表分派仍使用现有生成的 helper 层，
/// 将在后期移至运行时 ABI 后。
///
/// ## Context 所有权
///
/// 生成的 JNI wrapper 在调用无栈 RegC 函数体前初始化一个 `mj2x_regc_context`。
/// context 存储 JVM 环境为不透明指针加上抛出 Java 异常的 callback。
/// 运行时拥有 helper 语义，而非本地 JNI adapter 机制。
///
/// ## 可分发 ABI 边界
///
/// 公共权威是 `mj2x-crt/src/regc/mj2x_regc_crt.h`。公共契约属于该 header 的
/// Doxygen 注释。生成的 RegC C 包含 `regc/mj2x_regc_crt.h` 并调用其 C99/JNI ABI。
/// CLI 将 `mj2x-crt/resources.manifest` 中的每个条目复制到生成的 native 项目中，
/// 其构建模板编译 CRT 源码。这是唯一支持的可分发运行时入口。
///
/// ## RegC Helper 层
///
/// Helper 调用是无栈 generated C 和 JVM 之间的第一个运行时 ABI。
/// 当前 `RegCBackend` 将 helper 定义发射到生成的 C 翻译单元中。
///
/// MVP helper 层实现：
/// - 对象分配（`AllocObject`）
/// - 基本类型和对象数组分配/加载/存储
/// - 数组长度
/// - 实例/静态字段加载/存储（带 null 检查）
/// - `checkcast`、`instanceof`、monitor enter/exit
/// - 静态/虚/特殊/构造函数调用（通过 descriptor 驱动 `jvalue` 参数打包）
/// - 多维数组分配（递归 JNI allocation）
/// - 未捕获 `ATHROW` 传播
/// - try/catch 分派（`_mj2x_regc_exception_check` 等 4 个 helper）
/// - Java 整数除/余（CRT ABI 调用，带 `ArithmeticException`）
/// - 运行时策略 barrier（`mj2x_regc_barrier`，当前 no-op）
/// - 基本类型 compare 和 `Integer`/`Long`/`Boolean`/`Character` `toString`

#pragma once

#include <jni.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* mj2x_ref;

typedef void (*mj2x_regc_throw_callback)(void* env, const char* class_name, const char* message);

/// @brief Result code shared by the distributable RegC runtime ABI.
///
/// `PENDING_EXCEPTION` means a JNI call has already raised a Java exception;
/// callers must preserve it instead of throwing a second exception.  Other
/// non-zero statuses can be bridged through `mj2x_regc_report_status`.
typedef enum mj2x_regc_status
{
    MJ2X_REGC_STATUS_OK                = 0,
    MJ2X_REGC_STATUS_INVALID_ARGUMENT  = 1,
    MJ2X_REGC_STATUS_OUT_OF_MEMORY     = 2,
    MJ2X_REGC_STATUS_PENDING_EXCEPTION = 3,
    MJ2X_REGC_STATUS_INVALID_STATE     = 4,
    MJ2X_REGC_STATUS_UNSUPPORTED       = 5,
} mj2x_regc_status;

typedef struct mj2x_regc_context
{
    void*                    env;
    mj2x_regc_throw_callback throwException;
} mj2x_regc_context;

typedef struct mj2x_regc_class_cache
{
    const char* desc;
    jclass      value;
} mj2x_regc_class_cache;

typedef struct mj2x_regc_static_method_cache
{
    const char* owner;
    const char* name;
    const char* desc;
    jclass      ownerClass;
    jmethodID   method;
} mj2x_regc_static_method_cache;

typedef struct mj2x_regc_instance_method_cache
{
    const char* owner;
    const char* name;
    const char* desc;
    jclass      ownerClass;
    jmethodID   method;
    /// 最近一次非 owner 类 receiver 的运行时类与解析结果（接口/抽象方法
    /// 分派用）。jfieldID/jmethodID 在类存活期间有效，global ref 保活。
    jclass      lastReceiverClass;
    jmethodID   lastReceiverMethod;
} mj2x_regc_instance_method_cache;

typedef struct mj2x_regc_static_field_cache
{
    const char* owner;
    const char* name;
    const char* desc;
    jclass      ownerClass;
    jfieldID    field;
} mj2x_regc_static_field_cache;

typedef struct mj2x_regc_instance_field_cache
{
    const char* owner;
    const char* name;
    const char* desc;
    jclass      ownerClass;
    jfieldID    field;
} mj2x_regc_instance_field_cache;

/// @brief javac 合成 `values()` 调用点的站点缓存（数组类 clone 方法解析结果）。
///
/// @details
/// `$VALUES` 静态字段走 `mj2x_regc_static_field_cache`；本结构只缓存 clone 的
/// jmethodID 与其解析所用的数组类 global ref（jmethodID 在类存活期间有效，
/// global ref 保活）。发射端已验证 values() 为 javac 合成形态
/// （getstatic $VALUES + Object.clone + checkcast + areturn）。
typedef struct mj2x_regc_enum_values_site
{
    jclass    arrayClass;
    jmethodID clone;
} mj2x_regc_enum_values_site;

/// @brief javac 合成 `valueOf(String)` 调用点的站点缓存。
///
/// @details
/// 快路径直接调 `java/lang/Enum.valueOf(Class, String)`（JDK 侧
/// enumConstantDirectory 映射查找），比原路径省 JNI 静态调用进 trampoline、
/// Push/PopLocalFrame 与 checkcast。NPE/IllegalArgumentException 的语义与
/// 消息由 JDK 实现保证（与合成 valueOf 完全一致）。
typedef struct mj2x_regc_enum_valueof_site
{
    const char* ownerDesc;
    jclass      ownerClass;
    jclass      enumClass;
    jmethodID   enumValueOf;
} mj2x_regc_enum_valueof_site;

/// @brief Opaque UTF-16 StringBuilder state owned by mj2x-crt.
///
/// The allocation and representation stay private so a later C++ source pack
/// can replace the implementation without changing generated-C call sites.
typedef struct mj2x_string_builder mj2x_string_builder;

/// @brief 与 StringBuilder 共用 UTF-16 存储、但操作经过同步锁的 StringBuffer 状态。
///
/// 该类型保持 opaque；调用方只能通过 `mj2x_string_buffer_*` ABI 访问。状态逃逸到
/// Java 时会物化为真实 `java.lang.StringBuffer`，调用方仍负责销毁 native 状态。
typedef struct mj2x_string_builder mj2x_string_buffer;

void    mj2x_regc_init_context(mj2x_regc_context* context, void* env,
                               mj2x_regc_throw_callback throwException);
void    mj2x_regc_set_env(mj2x_regc_context* context, void* env);
void*   mj2x_regc_env(const mj2x_regc_context* context);
void    mj2x_regc_throw_exception(mj2x_regc_context* context, const char* class_name,
                                  const char* message);
void    mj2x_regc_report_status(mj2x_regc_context* context, mj2x_regc_status status,
                                const char* operation);
void    mj2x_regc_throw_arithmetic(mj2x_regc_context* context, const char* message);
int32_t mj2x_regc_div_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int64_t mj2x_regc_div_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);
int32_t mj2x_regc_rem_i32(mj2x_regc_context* context, int32_t lhs, int32_t rhs);
int64_t mj2x_regc_rem_i64(mj2x_regc_context* context, int64_t lhs, int64_t rhs);
void    mj2x_regc_barrier(mj2x_regc_context* context, const char* reason);

jclass    mj2x_regc_cached_class(JNIEnv* env, mj2x_regc_class_cache* cache);
jmethodID mj2x_regc_cached_static_method(JNIEnv* env, mj2x_regc_static_method_cache* cache);
jfieldID  mj2x_regc_cached_static_field(JNIEnv* env, mj2x_regc_static_field_cache* cache);

/// @brief javac 合成 `values()` 语义：返回 `$VALUES` 数组的 clone。
///
/// @details
/// values 数组由调用方经 `mj2x_regc_static_field_cache` 读取（cached
/// GetStaticObjectField）；本 helper 用站点缓存的 clone jmethodID 复制数组
/// （1 个 JNI）。clone 失败（OOM）时保持 pending exception 并返回 NULL。
mj2x_ref  mj2x_intrinsic_enum_values(mj2x_regc_context* context, mj2x_ref values,
                                     mj2x_regc_enum_values_site* site);

/// @brief javac 合成 `valueOf(String)` 语义：Enum.valueOf(owner, name) 直调。
///
/// @details
/// 命中/未命中统一走 JDK `Enum.valueOf`（enumConstantDirectory 映射），
/// NPE/IllegalArgumentException 语义与消息由 JDK 保证。返回 NULL 表示
/// pending exception（初始化失败或查找未命中）。
mj2x_ref  mj2x_intrinsic_enum_valueof(mj2x_regc_context* context, mj2x_ref name,
                                      mj2x_regc_enum_valueof_site* site);

int32_t _mj2x_intrinsic_int_compare(int32_t lhs, int32_t rhs);
int32_t _mj2x_intrinsic_long_compare(int64_t lhs, int64_t rhs);
int32_t _mj2x_intrinsic_byte_compare(int8_t lhs, int8_t rhs);
int32_t _mj2x_intrinsic_short_compare(int16_t lhs, int16_t rhs);
int32_t _mj2x_intrinsic_float_compare(float lhs, float rhs);
int32_t _mj2x_intrinsic_double_compare(double lhs, double rhs);
int32_t _mj2x_intrinsic_bool_compare(int8_t lhs, int8_t rhs);
int32_t _mj2x_intrinsic_char_compare(uint16_t lhs, uint16_t rhs);

mj2x_ref _mj2x_intrinsic_int_to_string(mj2x_regc_context* context, int32_t value);
mj2x_ref _mj2x_intrinsic_long_to_string(mj2x_regc_context* context, int64_t value);
mj2x_ref _mj2x_intrinsic_bool_to_string(mj2x_regc_context* context, int8_t value);
mj2x_ref _mj2x_intrinsic_char_to_string(mj2x_regc_context* context, uint16_t value);

/// @brief UTF-16 `java.lang.String` operations used by RegC intrinsics.
///
/// These functions preserve Java UTF-16 code-unit behavior. Returned references
/// are JNI local references owned by the current native invocation.
mj2x_regc_status mj2x_string_concat(mj2x_regc_context* context, mj2x_ref value, mj2x_ref suffix,
                                    mj2x_ref* out_string);
mj2x_regc_status mj2x_string_substring_from(mj2x_regc_context* context, mj2x_ref value,
                                            int32_t begin_index, mj2x_ref* out_string);
mj2x_regc_status mj2x_string_substring_range(mj2x_regc_context* context, mj2x_ref value,
                                             int32_t begin_index, int32_t end_index,
                                             mj2x_ref* out_string);
mj2x_regc_status mj2x_string_compare_to(mj2x_regc_context* context, mj2x_ref value, mj2x_ref other,
                                        int32_t* out_result);
mj2x_regc_status mj2x_string_index_of_char(mj2x_regc_context* context, mj2x_ref value,
                                           int32_t code_point, int32_t* out_index);
mj2x_regc_status mj2x_string_index_of_char_from(mj2x_regc_context* context, mj2x_ref value,
                                                int32_t code_point, int32_t from_index,
                                                int32_t* out_index);
mj2x_regc_status mj2x_string_index_of_string(mj2x_regc_context* context, mj2x_ref value,
                                             mj2x_ref needle, int32_t* out_index);
mj2x_regc_status mj2x_string_index_of_string_from(mj2x_regc_context* context, mj2x_ref value,
                                                  mj2x_ref needle, int32_t from_index,
                                                  int32_t* out_index);
mj2x_regc_status mj2x_string_replace_char(mj2x_regc_context* context, mj2x_ref value,
                                          uint16_t old_char, uint16_t new_char,
                                          mj2x_ref* out_string);
mj2x_regc_status mj2x_string_trim(mj2x_regc_context* context, mj2x_ref value, mj2x_ref* out_string);
mj2x_regc_status mj2x_string_split(mj2x_regc_context* context, mj2x_ref value, mj2x_ref regex,
                                   mj2x_ref* out_array);
mj2x_regc_status mj2x_string_split_limit(mj2x_regc_context* context, mj2x_ref value, mj2x_ref regex,
                                         int32_t limit, mj2x_ref* out_array);

/// @brief Allocate or destroy an opaque StringBuilder state.
///
/// `init_jstring` follows `StringBuilder(String)` and therefore reports a
/// `NullPointerException` for a null initial value.  `append_jstring`, like
/// Java `append(String)`, appends the literal `"null"` for a null value.
mj2x_regc_status mj2x_string_builder_init(mj2x_regc_context*    context,
                                          mj2x_string_builder** out_state);
mj2x_regc_status mj2x_string_builder_init_jstring(mj2x_regc_context*    context,
                                                  mj2x_ref              initial_value,
                                                  mj2x_string_builder** out_state);
mj2x_regc_status mj2x_string_builder_init_capacity(mj2x_regc_context* context, int32_t capacity,
                                                   mj2x_string_builder** out_state);
mj2x_regc_status mj2x_string_builder_init_char_sequence(mj2x_regc_context*    context,
                                                        mj2x_ref              initial_value,
                                                        mj2x_string_builder** out_state);
void             mj2x_string_builder_destroy(mj2x_string_builder* state);

/// @brief Append Java UTF-16 code units without converting through UTF-8.
mj2x_regc_status mj2x_string_builder_append_utf16(mj2x_regc_context*   context,
                                                  mj2x_string_builder* state, const uint16_t* units,
                                                  size_t length);
mj2x_regc_status mj2x_string_builder_append_jstring(mj2x_regc_context*   context,
                                                    mj2x_string_builder* state, mj2x_ref value);
mj2x_regc_status mj2x_string_builder_append_object(mj2x_regc_context*   context,
                                                   mj2x_string_builder* state, mj2x_ref value);
mj2x_regc_status mj2x_string_builder_append_char_sequence(mj2x_regc_context*   context,
                                                          mj2x_string_builder* state,
                                                          mj2x_ref             value);
mj2x_regc_status mj2x_string_builder_append_char_sequence_range(mj2x_regc_context*   context,
                                                                mj2x_string_builder* state,
                                                                mj2x_ref value, int32_t start,
                                                                int32_t end);
mj2x_regc_status mj2x_string_builder_append_i32(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, int32_t value);
mj2x_regc_status mj2x_string_builder_append_i64(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, int64_t value);
mj2x_regc_status mj2x_string_builder_append_bool(mj2x_regc_context*   context,
                                                 mj2x_string_builder* state, int8_t value);
mj2x_regc_status mj2x_string_builder_append_char(mj2x_regc_context*   context,
                                                 mj2x_string_builder* state, uint16_t value);
mj2x_regc_status mj2x_string_builder_append_char_array(mj2x_regc_context*   context,
                                                       mj2x_string_builder* state, mj2x_ref value);
mj2x_regc_status mj2x_string_builder_append_char_array_range(mj2x_regc_context*   context,
                                                             mj2x_string_builder* state,
                                                             mj2x_ref value, int32_t offset,
                                                             int32_t length);
mj2x_regc_status mj2x_string_builder_append_code_point(mj2x_regc_context*   context,
                                                       mj2x_string_builder* state,
                                                       int32_t              code_point);
mj2x_regc_status mj2x_string_builder_append_f32(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, float value);
mj2x_regc_status mj2x_string_builder_append_f64(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, double value);

/// @brief Materialize a UTF-16 snapshot as a local `java.lang.String` reference.
mj2x_regc_status mj2x_string_builder_to_jstring(mj2x_regc_context*         context,
                                                const mj2x_string_builder* state,
                                                mj2x_ref*                  out_jstring);

/// @brief Materialize a UTF-16 snapshot as a local `java.lang.StringBuilder`.
///
/// This is the escape bridge for later lowering: it returns a real Java object
/// while preserving the opaque native state for the caller to destroy.
mj2x_regc_status mj2x_string_builder_materialize(mj2x_regc_context*         context,
                                                 const mj2x_string_builder* state,
                                                 mj2x_ref*                  out_builder);

/// @brief 查询或修改 Java 可观察的 StringBuilder 长度、容量与 UTF-16 单元。
///
/// 所有索引按 UTF-16 code unit 计算；越界和负长度通过 Java 异常桥接返回
/// `MJ2X_REGC_STATUS_PENDING_EXCEPTION`。
mj2x_regc_status mj2x_string_builder_length(mj2x_regc_context*         context,
                                            const mj2x_string_builder* state, int32_t* out_length);
mj2x_regc_status mj2x_string_builder_capacity(mj2x_regc_context*         context,
                                              const mj2x_string_builder* state,
                                              int32_t*                   out_capacity);
mj2x_regc_status mj2x_string_builder_ensure_capacity(mj2x_regc_context*   context,
                                                     mj2x_string_builder* state,
                                                     int32_t              minimum_capacity);
mj2x_regc_status mj2x_string_builder_trim_to_size(mj2x_regc_context*   context,
                                                  mj2x_string_builder* state);
mj2x_regc_status mj2x_string_builder_set_length(mj2x_regc_context*   context,
                                                mj2x_string_builder* state, int32_t new_length);
mj2x_regc_status mj2x_string_builder_char_at(mj2x_regc_context*         context,
                                             const mj2x_string_builder* state, int32_t index,
                                             uint16_t* out_char);
mj2x_regc_status mj2x_string_builder_set_char_at(mj2x_regc_context*   context,
                                                 mj2x_string_builder* state, int32_t index,
                                                 uint16_t value);

/// @brief Return the number of retained UTF-16 code units, or zero for null.
size_t mj2x_string_builder_utf16_length(const mj2x_string_builder* state);

/// @brief 创建、操作和销毁带同步语义的 opaque StringBuffer 状态。
///
/// 每个公开操作都持有状态内部锁；不同状态之间互不阻塞。销毁前调用方必须确保没有
/// 仍在执行的并发操作。
mj2x_regc_status mj2x_string_buffer_init(mj2x_regc_context*   context,
                                         mj2x_string_buffer** out_state);
mj2x_regc_status mj2x_string_buffer_init_jstring(mj2x_regc_context* context, mj2x_ref initial_value,
                                                 mj2x_string_buffer** out_state);
mj2x_regc_status mj2x_string_buffer_init_capacity(mj2x_regc_context* context, int32_t capacity,
                                                  mj2x_string_buffer** out_state);
mj2x_regc_status mj2x_string_buffer_init_char_sequence(mj2x_regc_context*   context,
                                                       mj2x_ref             initial_value,
                                                       mj2x_string_buffer** out_state);
void             mj2x_string_buffer_destroy(mj2x_string_buffer* state);
mj2x_regc_status mj2x_string_buffer_append_jstring(mj2x_regc_context*  context,
                                                   mj2x_string_buffer* state, mj2x_ref value);
mj2x_regc_status mj2x_string_buffer_append_object(mj2x_regc_context*  context,
                                                  mj2x_string_buffer* state, mj2x_ref value);
mj2x_regc_status mj2x_string_buffer_append_char_sequence(mj2x_regc_context*  context,
                                                         mj2x_string_buffer* state, mj2x_ref value);
mj2x_regc_status mj2x_string_buffer_append_char_sequence_range(mj2x_regc_context*  context,
                                                               mj2x_string_buffer* state,
                                                               mj2x_ref value, int32_t start,
                                                               int32_t end);
mj2x_regc_status mj2x_string_buffer_append_i32(mj2x_regc_context*  context,
                                               mj2x_string_buffer* state, int32_t value);
mj2x_regc_status mj2x_string_buffer_append_i64(mj2x_regc_context*  context,
                                               mj2x_string_buffer* state, int64_t value);
mj2x_regc_status mj2x_string_buffer_append_bool(mj2x_regc_context*  context,
                                                mj2x_string_buffer* state, int8_t value);
mj2x_regc_status mj2x_string_buffer_append_char(mj2x_regc_context*  context,
                                                mj2x_string_buffer* state, uint16_t value);
mj2x_regc_status mj2x_string_buffer_append_char_array(mj2x_regc_context*  context,
                                                      mj2x_string_buffer* state, mj2x_ref value);
mj2x_regc_status mj2x_string_buffer_append_char_array_range(mj2x_regc_context*  context,
                                                            mj2x_string_buffer* state,
                                                            mj2x_ref value, int32_t offset,
                                                            int32_t length);
mj2x_regc_status mj2x_string_buffer_append_code_point(mj2x_regc_context*  context,
                                                      mj2x_string_buffer* state,
                                                      int32_t             code_point);
mj2x_regc_status mj2x_string_buffer_append_f32(mj2x_regc_context*  context,
                                               mj2x_string_buffer* state, float value);
mj2x_regc_status mj2x_string_buffer_append_f64(mj2x_regc_context*  context,
                                               mj2x_string_buffer* state, double value);
mj2x_regc_status mj2x_string_buffer_to_jstring(mj2x_regc_context*        context,
                                               const mj2x_string_buffer* state,
                                               mj2x_ref*                 out_jstring);
mj2x_regc_status mj2x_string_buffer_materialize(mj2x_regc_context*        context,
                                                const mj2x_string_buffer* state,
                                                mj2x_ref*                 out_buffer);
mj2x_regc_status mj2x_string_buffer_length(mj2x_regc_context*        context,
                                           const mj2x_string_buffer* state, int32_t* out_length);
mj2x_regc_status mj2x_string_buffer_capacity(mj2x_regc_context*        context,
                                             const mj2x_string_buffer* state,
                                             int32_t*                  out_capacity);
mj2x_regc_status mj2x_string_buffer_ensure_capacity(mj2x_regc_context*  context,
                                                    mj2x_string_buffer* state,
                                                    int32_t             minimum_capacity);
mj2x_regc_status mj2x_string_buffer_trim_to_size(mj2x_regc_context*  context,
                                                 mj2x_string_buffer* state);
mj2x_regc_status mj2x_string_buffer_set_length(mj2x_regc_context*  context,
                                               mj2x_string_buffer* state, int32_t new_length);
mj2x_regc_status mj2x_string_buffer_char_at(mj2x_regc_context*        context,
                                            const mj2x_string_buffer* state, int32_t index,
                                            uint16_t* out_char);
mj2x_regc_status mj2x_string_buffer_set_char_at(mj2x_regc_context*  context,
                                                mj2x_string_buffer* state, int32_t index,
                                                uint16_t value);
size_t           mj2x_string_buffer_utf16_length(const mj2x_string_buffer* state);

#ifdef __cplusplus
}
#endif
