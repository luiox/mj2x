// regc_computed_jumps.cpp
//
// computed jumps / 跳转目标间接化（--regc-computed-jumps，issue #7）。
//
// CFF 把控制流打散成 dispatcher，但 dispatcher 的 case key 仍是字面量——逆序
// case key 就能还原控制流。本项把 "key -> 目标块" 的映射从代码挪到数据表：
//
//   key 表按站点密钥异或后存放（密钥运行时派生，编译器不可折叠）
//   分派前先查表解析出块索引，再按索引 switch
//
// 于是产物里不再有 `case 5: goto block_b17` 这种直接可读的映射，还原控制流需要
// 先解出表。
//
// 便携性：MSVC 是主目标，不能用 GCC 的 computed goto（`&&label`）。表驱动 + 索引
// switch 是标准 C 内可表达的最强形态——这是本项的能力上限，不是偷懒。

#include "mj2x/backend/regc/regc_emitter_internal.hpp"

#include "mj2x/mir/passes/const_key_derivation.hpp"

namespace mj2x {

bool emitComputedJumpDispatch(codegen::CSourceStream& out, RegCFunctionEmitState& state,
                              const RegSwitchTerminator& t, std::string& indexExpression,
                              std::string& reason)
{
    (void)reason;
    if (state.forwardDecls == nullptr) return false;   // 无 TU 头部注册表则放弃

    const size_t      site = state.computedJumpSites++;
    const std::string keys =
        ca::str::format_std("_mj2x_cj_keys_{}_{}", state.functionName, site);
    const std::string idx = ca::str::format_std("_mj2x_cj_idx_{}", site);

    // 密钥与立即数隐藏同源：splitmix64(site ^ kConstKeyOpaque)，运行时派生、
    // 编译器不可折叠。key 表存的是 key ^ K，运行时把 switch 值也异或 K 后比对。
    const int64_t k = static_cast<int64_t>(deriveConstKey(static_cast<int64_t>(site)));

    // key 表（编译期常量，按 K 异或后存放）→ TU 头部
    std::string keyTable = ca::str::format_std("static const int64_t {}[] = {{", keys);
    for (size_t i = 0; i < t.cases.size(); ++i) {
        if (i != 0) keyTable += ", ";
        keyTable += ca::str::format_std("{}", static_cast<int64_t>(t.cases[i].key) ^ k);
    }
    keyTable += "};";
    state.forwardDecls->declare(keyTable);

    // 站点密钥只派生一次：static 局部量首次分派时初始化，之后只有一次加载。
    //
    // 每分派都调 mj2x_regc_const_key 实测让 ControlFlow probe 慢 70%——表驱动本身
    // 很便宜，贵的是那次跨 TU 调用。改成静态缓存后仍是运行时值，不可折叠。
    const std::string keyVar = ca::str::format_std("_mj2x_cj_k_{}", site);
    out.declare_var("static int64_t", keyVar, "0");
    out.iff(ca::str::format_std("{} == 0", keyVar), [&](codegen::CSourceStream& o) {
        o.assign(keyVar, ca::str::format_std("mj2x_regc_const_key({})", static_cast<int64_t>(site)));
    });

    // 运行时解析：probe = value ^ K，扫表拿到块索引（未命中保持 -1，由 default 兜底）。
    out.declare_var("int32_t", idx, "-1");
    out.block([&](codegen::CSourceStream& o) {
        o.declare_var("int64_t", "_mj2x_cj_probe",
                      ca::str::format_std("(int64_t)({}) ^ {}",
                                          operandExpr(t.value).to_std_string(), keyVar));
        o.block([&](codegen::CSourceStream& p) {
            p.declare_var("int32_t", "_mj2x_cj_i", "0");
            p.iff(ca::str::format_std("_mj2x_cj_i < {}", t.cases.size()),
                  [&](codegen::CSourceStream& q) {
                      q.iff(ca::str::format_std("{}[_mj2x_cj_i] == _mj2x_cj_probe", keys),
                            [&](codegen::CSourceStream& r) {
                                r.assign(idx, "_mj2x_cj_i");
                                // 命中即跳出：把计数推到上界。
                                r.assign("_mj2x_cj_i",
                                         ca::str::format_std("{}", t.cases.size()));
                            },
                            [&](codegen::CSourceStream& r) { r.emitLine("_mj2x_cj_i += 1;"); });
                  });
        });
    });

    indexExpression = idx;
    return true;
}

}   // namespace mj2x
