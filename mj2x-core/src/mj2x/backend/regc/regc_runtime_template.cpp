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
#include "libca/resources/resources.hpp"

// 模板经 resources.embed 嵌入 mj2x-core(挂载 codegen),include 即自注册。
#include "resources_mj2x_core.generated.hpp"

namespace mj2x {

void emitRuntimeHelpers(ca::str::Utf8StringBuilder& out, const std::string& cacheDeclarations,
                        codegen::CForwardDeclarations* fwd)
{
    // 残量模板见 resources/codegen/regc_runtime_helpers.c.in(#894 B 类真下沉):
    // 主体已物理下沉 mj2x-rt,此处仅注入 fwd 前向声明与 per-callsite 缓存声明。
    const auto* bundle = ca::resources::bundle("codegen");
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
    values["CACHE_DECLARATIONS"] = cacheDeclarations;

    std::string rendered;
    std::string err;
    if (!codegen::renderCTemplate(
            std::string_view{reinterpret_cast<const char*>(tpl.unwrap().data()),
                             tpl.unwrap().size()},
            values, {}, rendered, err)) {
        CA_LOGT_ERROR("codegen", "cannot render regc runtime residual: {}\n", err.c_str());
        return;
    }
    out.append(rendered.data(), rendered.size());
}

}   // namespace mj2x
