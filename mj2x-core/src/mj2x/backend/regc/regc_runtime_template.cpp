/// @file regc_runtime_template.cpp
/// @brief RegC 运行时 C-source 定义发射(从 regc_backend.cpp 抽出)。
///
/// @details 见 regc_runtime_template.hpp。emitRuntimeHelpers 全部主体已迁移为
/// resources/codegen/regc_runtime_helpers.c.in 文本模板(#894),此处仅做占位符
/// 渲染;field/method access helper 文本已并入模板(Phase 1 调整:不再单独
/// 发射,亦不物理下沉 mj2x-rt——依赖发射体内 thread_local _mj2x_regc_env,
/// 跨 TU 共享需改 ABI,收益不抵风险)。
#include "mj2x/backend/regc/regc_runtime_template.hpp"
#include "mj2x/codegen/c_render.hpp"

#include <string>
#include <unordered_map>

#include "libca/log/log_macros.hpp"
#include "libresources/resources.hpp"

// 模板经 resources.embed 嵌入 mj2x-core(挂载 codegen),include 即自注册。
#include "resources_mj2x_core.generated.hpp"

namespace mj2x {

void emitRuntimeHelpers(ca::str::Utf8StringBuilder& out, const std::string& cacheDeclarations,
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
                        const std::string& unsupportedOperationExceptionClassCacheName,
                        bool traceCallLog, codegen::CForwardDeclarations* fwd)
{
    // 模板主体见 resources/codegen/regc_runtime_helpers.c.in(#894)。
    const auto* bundle = resources::bundle("codegen");
    if (bundle == nullptr) {
        CA_LOGT_ERROR("codegen", "codegen template mount is unavailable\n");
        return;
    }
    const auto tpl = bundle->get("/regc_runtime_helpers.c.in");
    if (tpl.is_err()) {
        CA_LOGT_ERROR("codegen", "codegen template is missing: regc_runtime_helpers.c.in\n");
        return;
    }

    std::unordered_map<std::string, std::string> values;
    values["FORWARD_DECLARATIONS"] =
        (fwd != nullptr && !fwd->empty()) ? fwd->render() + "\n" : "";
    values["CACHE_DECLARATIONS"]                     = cacheDeclarations;
    values["STRING_CLASS_CACHE"]                     = stringClassCacheName;
    values["OBJECT_TO_STRING_METHOD_CACHE"]          = objectToStringMethodCacheName;
    values["STRING_VALUE_OF_FLOAT_METHOD_CACHE"]     = stringValueOfFloatMethodCacheName;
    values["STRING_VALUE_OF_DOUBLE_METHOD_CACHE"]    = stringValueOfDoubleMethodCacheName;
    values["FLOAT_PARSE_FLOAT_METHOD_CACHE"]         = floatParseFloatMethodCacheName;
    values["DOUBLE_PARSE_DOUBLE_METHOD_CACHE"]       = doubleParseDoubleMethodCacheName;
    values["NULL_POINTER_EXCEPTION_CLASS_CACHE"]     = nullPointerExceptionClassCacheName;
    values["CLASS_CAST_EXCEPTION_CLASS_CACHE"]       = classCastExceptionClassCacheName;
    values["OUT_OF_MEMORY_ERROR_CLASS_CACHE"]        = outOfMemoryErrorClassCacheName;
    values["STRING_INDEX_OUT_OF_BOUNDS_CLASS_CACHE"] = stringIndexOutOfBoundsClassCacheName;
    values["UNSUPPORTED_OPERATION_CLASS_CACHE"]      = unsupportedOperationExceptionClassCacheName;
    // traceCallLog 条件块(#894):trace 变体注入 dbg 深度追踪 + 带日志的
    // call_cached_instance_void;否则发射无日志变体。文本与原实现逐字节一致。
    values["TRACE_VOID_BLOCK"] =
        traceCallLog
            ? R"mj2x(static _Thread_local int _mj2x_regc_dbg_depth = 0;
static void _mj2x_regc_dbg_enter(const char* owner, const char* name, const char* desc) {
    _mj2x_regc_dbg_depth++;
    if (_mj2x_regc_dbg_depth > 64) {
        fprintf(stderr, "DEEP[%d] %s.%s%s\n", _mj2x_regc_dbg_depth, owner, name, desc);
    }
}
static void _mj2x_regc_dbg_leave() { _mj2x_regc_dbg_depth--; }
static void _mj2x_regc_call_cached_instance_void(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return; fprintf(stderr, "[CALL:%p] %s.%s%s\n", (void*)_mj2x_regc_env, cache->owner, cache->name, cache->desc); _mj2x_regc_dbg_enter(cache->owner, cache->name, cache->desc); (*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, receiver, m, values); _mj2x_regc_dbg_leave(); }
)mj2x"
            : R"mj2x(static void _mj2x_regc_call_cached_instance_void(mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...) { jvalue values[32]; va_list ap; va_start(ap, arg_count); jobject receiver = _mj2x_regc_collect_args(cache->desc, arg_count, ap, values); va_end(ap); if (receiver == NULL) { _mj2x_regc_throw_null("instance method call"); return; } jmethodID m = _mj2x_regc_cached_instance_method(cache, receiver); if (m == NULL) return; (*_mj2x_regc_env)->CallVoidMethodA(_mj2x_regc_env, receiver, m, values); }
)mj2x";

    std::string rendered;
    std::string err;
    if (!codegen::renderCTemplate(
            std::string_view{reinterpret_cast<const char*>(tpl.unwrap().data()),
                             tpl.unwrap().size()},
            values, {}, rendered, err)) {
        CA_LOGT_ERROR("codegen", "cannot render regc runtime helpers: {}\n", err.c_str());
        return;
    }
    out.append(rendered.data(), rendered.size());
}

}   // namespace mj2x
