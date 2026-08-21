#include "mj2x/backend/stackc/stackc_runtime_prelude.hpp"

#include "mj2x/codegen/c_render.hpp"

#include <string>

#include "libca/log/log_macros.hpp"
#include "libresources/resources.hpp"

// 代码生成模板经 resources.embed 嵌入(#894):挂载 codegen,include 即自注册。
// 本 TU include 生成头,保证库自身与下游(mj2x-cli/unittest)索引可用。
#include "resources_mj2x_core.generated.hpp"

namespace mj2x::backend {

std::string stackCRuntimePrelude()
{
    const auto* bundle = resources::bundle("codegen");
    if (bundle == nullptr) {
        CA_LOGT_ERROR("codegen", "codegen template mount is unavailable\n");
        return {};
    }
    const auto tpl = bundle->get("/stackc_runtime_prelude.c.in");
    if (tpl.is_err()) {
        CA_LOGT_ERROR("codegen", "codegen template is missing: {}\n",
                      "stackc_runtime_prelude.c.in");
        return {};
    }
    std::string rendered;
    std::string err;
    if (!codegen::renderCTemplate(
            std::string_view{reinterpret_cast<const char*>(tpl.unwrap().data()),
                             tpl.unwrap().size()},
            {}, {}, rendered, err)) {
        CA_LOGT_ERROR("codegen", "cannot render stackc runtime prelude: {}\n", err.c_str());
        return {};
    }
    return rendered;
}

}   // namespace mj2x::backend
