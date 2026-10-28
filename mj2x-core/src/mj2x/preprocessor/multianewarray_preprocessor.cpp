#include "multianewarray_preprocessor.hpp"

#include "jbc/core/insn_codec.hpp"
#include "libca/log/log_macros.hpp"

#include "mj2x/core/dom_edit.hpp"

namespace mj2x {

void MultiANewArrayPreprocessor::process(CompilerIndex& index)
{
    // Keep this preprocessor observational.  CodeConverter and RegC already
    // preserve MULTIANEWARRAY as a helper-backed operation, so bytecode-level
    // loop expansion would only make the input harder to reason about.
    // 批 4：读侧落 DOM（Code 解码指令流统计，不触树）。
    int total = 0;
    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;
        for (auto& method : cls.methods) {
            auto& dm = domMethodFor(cls, method.name, method.desc);
            auto code = cls.dom->decodeCode(dm);
            if (code.is_err()) continue;
            auto* attr = std::move(code).unwrap();
            // decodeCode 对无 Code 方法（abstract/native，如接口的抽象方法）
            // 返回 Ok(nullptr)——只查 is_err 不查 null 即空指针解引用。
            if (attr == nullptr) continue;
            auto insns = jbc::core::decodeInsns(
                jbc::core::ByteView{attr->code.data(), attr->code.size()});
            if (insns.is_err()) continue;
            for (const auto& in : std::move(insns).unwrap()) {
                if (in.op == jbc::core::Op::MULTIANEWARRAY) {
                    ++total;
                }
            }
        }
    }
    if (total > 0) {
        CA_LOGT_INFO("preprocess",
                     "Detected {} multianewarray site(s). Keeping bytecode shape; RegC lowers "
                     "supported sites through its multi-array helper.\n",
                     total);
    }
}

}   // namespace mj2x
