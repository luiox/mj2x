#include "mj2x/codegen/runtime_protection.hpp"

#include "mj2x/codegen/c_source.hpp"

#include <string>

namespace mj2x {
namespace codegen {

const char* runtimeProtectionActionToken(RuntimeProtectionAction action)
{
    switch (action) {
    case RuntimeProtectionAction::Log: return "MJ2X_RT_PROTECT_LOG";
    case RuntimeProtectionAction::Delay: return "MJ2X_RT_PROTECT_DELAY";
    case RuntimeProtectionAction::Fake: return "MJ2X_RT_PROTECT_FAKE";
    case RuntimeProtectionAction::Terminate: return "MJ2X_RT_PROTECT_TERMINATE";
    case RuntimeProtectionAction::None: break;
    }
    return "";
}

void emitRuntimeProtectionCheckpoint(CSourceStream& out, const RuntimeProtectionOptions& opts,
                                     const std::string& checkpointVar,
                                     const std::string& fakeReturn)
{
    if (!opts.enabled()) return;
    out.appendLineStart("/* runtime protection checkpoint (--runtime-protection) */");
    out.appendLineStart("static mj2x_rt_checkpoint ").append(checkpointVar).append(";");
    out.appendLineStart("if (mj2x_rt_checkpoint_run(&")
        .append(checkpointVar)
        .append(", ")
        .append(runtimeProtectionActionToken(opts.action))
        .append(", ")
        .append(std::to_string(opts.step))
        .append("u, ")
        .append(std::to_string(opts.delayMs))
        .append("u, 1)) {");
    out.indent();
    if (!fakeReturn.empty()) {
        // 调用方提供的完整返回语句（如 `return 0;` / `goto _mj2x_regc_cleanup;`）。
        out.appendLineStart(fakeReturn);
    }
    out.dedent();
    out.appendLineStart("}");
    out.newLine();
}

}   // namespace codegen
}   // namespace mj2x
