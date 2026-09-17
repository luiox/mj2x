#include "mj2x/backend/vmc4/vm_encoder.hpp"

#include "mj2x/backend/vmc4/vmc4_instance.hpp"
#include "mj2x/mir/register_ir/reg_block.hpp"
#include "mj2x/mir/register_ir/reg_function.hpp"
#include "mj2x/mir/register_ir/reg_instruction.hpp"
#include "mj2x/mir/register_ir/reg_value.hpp"
#include "mj2x/mir/ssa/ssa_instruction.hpp"
#include "mj2x/mir/ssa/ssa_type.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace mj2x {

namespace {

// RegIR / SSA 类型直接在 mj2x 命名空间（无 mj2x::mir 子命名空间）。

// ---------------------------------------------------------------------------
// 字节流写入 helper：小端编码。
// ---------------------------------------------------------------------------
void emitU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void emitU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void emitI32(std::vector<uint8_t>& out, int32_t v)
{
    const uint32_t u = static_cast<uint32_t>(v);
    out.push_back(static_cast<uint8_t>(u & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
}

/// @brief SSAType → VmType 映射（计算类型 + 对象引用）。
///
/// @details
/// I32/I64/F32/F64 是计算类型；BOOL/I8/I16/CHAR 归一到 I32（计算位宽一致，
/// 见 ssa_type.hpp:38 ssaIsIntLike）。REF 映射到 VmType::REF（对象引用，
/// Phase 2.0 起支持，用于对象参数/返回值/字段/Copy）。UNKNOWN 不支持。
bool toVmType(SSAType s, VmType& out)
{
    switch (s) {
    case SSAType::BOOL:
    case SSAType::I8:
    case SSAType::I16:
    case SSAType::CHAR:
    case SSAType::I32: out = VmType::I32; return true;
    case SSAType::I64: out = VmType::I64; return true;
    case SSAType::F32: out = VmType::F32; return true;
    case SSAType::F64: out = VmType::F64; return true;
    case SSAType::REF: out = VmType::REF; return true;
    default: return false;   // UNKNOWN 等不支持
    }
}

/// @brief SSAUnaryOp → VmUnaryOp。
bool toVmUnaryOp(SSAUnaryOp op, VmUnaryOp& out)
{
    switch (op) {
    case SSAUnaryOp::Neg: out = VmUnaryOp::Neg; return true;
    case SSAUnaryOp::BitNot: out = VmUnaryOp::BitNot; return true;
    }
    return false;
}

/// @brief SSABinaryOp → VmBinaryOp。
bool toVmBinaryOp(SSABinaryOp op, VmBinaryOp& out)
{
    switch (op) {
    case SSABinaryOp::Add: out = VmBinaryOp::Add; return true;
    case SSABinaryOp::Sub: out = VmBinaryOp::Sub; return true;
    case SSABinaryOp::Mul: out = VmBinaryOp::Mul; return true;
    case SSABinaryOp::Div: out = VmBinaryOp::Div; return true;
    case SSABinaryOp::Rem: out = VmBinaryOp::Rem; return true;
    case SSABinaryOp::And: out = VmBinaryOp::And; return true;
    case SSABinaryOp::Or: out = VmBinaryOp::Or; return true;
    case SSABinaryOp::Xor: out = VmBinaryOp::Xor; return true;
    case SSABinaryOp::Shl: out = VmBinaryOp::Shl; return true;
    case SSABinaryOp::Shr: out = VmBinaryOp::Shr; return true;
    case SSABinaryOp::UShr: out = VmBinaryOp::UShr; return true;
    }
    return false;
}

/// @brief SSAType → VmJniKind（per-callsite helper marshalling 用）。
///
/// @details
/// boolean/byte/char/short/int 归 Int32（JNI 侧 jint，slot .i）；long → Int64（slot .i）；
/// float/double → Float/Double（slot .f）；REF → Ref（slot .ref）。UNKNOWN 不应出现在
/// 已通过 toVmType 校验的调用点，兜底归 Void（helper 不读该参数，调用结果错位会显式失败）。
VmJniKind toVmJniKind(SSAType s)
{
    switch (s) {
    case SSAType::BOOL:
    case SSAType::I8:
    case SSAType::I16:
    case SSAType::CHAR:
    case SSAType::I32: return VmJniKind::Int32;
    case SSAType::I64: return VmJniKind::Int64;
    case SSAType::F32: return VmJniKind::Float;
    case SSAType::F64: return VmJniKind::Double;
    case SSAType::REF: return VmJniKind::Ref;
    default: return VmJniKind::Void;
    }
}

/// @brief SSAType → VmArrayElemKind（数组元素访问 accessor 用，精确）。
///
/// @details
/// 数组元素必须用精确类型 accessor（GetIntArrayRegion vs GetBooleanArrayRegion 不可互换）。
/// BOOL/I8/CHAR/I16 在 SSAType 里是独立值，保留精确；UNKNOWN 不应出现在已校验的数组访问。
VmArrayElemKind toVmArrayElemKind(SSAType s)
{
    switch (s) {
    case SSAType::BOOL: return VmArrayElemKind::Boolean;
    case SSAType::I8: return VmArrayElemKind::Byte;
    case SSAType::CHAR: return VmArrayElemKind::Char;
    case SSAType::I16: return VmArrayElemKind::Short;
    case SSAType::I32: return VmArrayElemKind::Int;
    case SSAType::I64: return VmArrayElemKind::Long;
    case SSAType::F32: return VmArrayElemKind::Float;
    case SSAType::F64: return VmArrayElemKind::Double;
    case SSAType::REF: return VmArrayElemKind::Ref;
    default: return VmArrayElemKind::Int;
    }
}

/// @brief SSAAllocationKind → VmAllocKind。
VmAllocKind toVmAllocKind(SSAAllocationKind k)
{
    switch (k) {
    case SSAAllocationKind::Object: return VmAllocKind::Object;
    case SSAAllocationKind::Array: return VmAllocKind::Array;
    case SSAAllocationKind::MultiArray: return VmAllocKind::MultiArray;
    }
    return VmAllocKind::Object;
}

/// @brief SSAComparePredicate → VmComparePred。
bool toVmComparePred(SSAComparePredicate p, VmComparePred& out)
{
    switch (p) {
    case SSAComparePredicate::Eq: out = VmComparePred::Eq; return true;
    case SSAComparePredicate::Ne: out = VmComparePred::Ne; return true;
    case SSAComparePredicate::Lt: out = VmComparePred::Lt; return true;
    case SSAComparePredicate::Le: out = VmComparePred::Le; return true;
    case SSAComparePredicate::Gt: out = VmComparePred::Gt; return true;
    case SSAComparePredicate::Ge: out = VmComparePred::Ge; return true;
    }
    return false;
}

/// @brief 编码期上下文：常量池 + block 索引映射 + 寄存器计数。
struct EncoderState
{
    const RegFunction& fn;
    const VmInstance&  instance;
    /// 用户自定义 intrinsic（issue #818）；nullptr 表示未启用。
    const std::vector<UserIntrinsic>* userIntrinsics = nullptr;

    /// block 指针 → 在 fn.blocks 里的索引（也用作 blockTargets/Jump/Branch 的目标索引）。
    std::unordered_map<const RegBlock*, uint16_t> blockIndex;

    /// 每个常量的签名 → 在 constantPool 的索引（去重）。
    /// 签名 = type(1B) + intBits(8B) / floatValue(8B)，用字符串当 key 简化去重。
    std::unordered_map<std::string, uint16_t> constSlotBySignature;

    /// Call 调用点的方法引用表（Phase 2.0）：每个 Call 指令收集一个 VmMethodRef，
    /// 字节流只存 callSiteIdx（索引）。backend 为每项发射一个 TU-local helper。
    std::vector<VmMethodRef>                                  methodRefs;
    std::unordered_map<std::string, uint16_t>                methodRefByKey;

    /// LoadField/StoreField 字段引用表（Phase 2.A）：每个字段访问点收集一个 VmFieldRef，
    /// 字节流只存 fieldSiteIdx。load/store 各一张表（helper 语义不同：Get vs Set）。
    std::vector<VmFieldRef>                                  loadFieldRefs;
    std::unordered_map<std::string, uint16_t>                loadFieldByKey;
    std::vector<VmFieldRef>                                  storeFieldRefs;
    std::unordered_map<std::string, uint16_t>                storeFieldByKey;

    /// ArrayLoad/ArrayStore 元素类型表（Phase 2.A）：按 elemKind 去重（helper 只依赖
    /// 元素类型，数组/索引/值经参数传）。load/store 各一张表。
    std::vector<VmArrayRef>                                  arrayLoadRefs;
    std::unordered_map<uint8_t, uint16_t>                    arrayLoadByKey;
    std::vector<VmArrayRef>                                  arrayStoreRefs;
    std::unordered_map<uint8_t, uint16_t>                    arrayStoreByKey;

    /// Allocate 分配表（Phase 2.A）：按 (kind, typeDesc) 去重。hasArrayLength 标记
    /// 是否含 array.length intrinsic（单一无状态 helper，siteIdx 恒为 0）。
    std::vector<VmAllocRef>                                  allocRefs;
    std::unordered_map<std::string, uint16_t>                allocByKey;
    bool                                                     hasArrayLength = false;

    /// Intrinsic 表（Phase 2.D 泛化）：按 name 去重（array.length/cmp.nan_*/
    /// instanceof:<desc>/monitor.enter/exit），字节流存 siteIdx。
    std::vector<VmIntrinsicRef>                              intrinsicRefs;
    std::unordered_map<std::string, uint16_t>                intrinsicByKey;

    /// String LDC 常量槽（Phase 2.E）：按内容去重（ldc_string Opaque 物质化）。
    std::vector<VmStringLiteral>                             stringLiterals;
    std::unordered_map<std::string, uint16_t>                stringSlotByValue;

    /// `.class` 字面量常量槽：按内部名去重（ldc_class Opaque 物质化）。
    std::vector<VmClassLiteral>                              classLiterals;
    std::unordered_map<std::string, uint16_t>                classSlotByDesc;

    /// 常量寄存器槽从 maxReg 高水位之后分配（见 internConstant），起始索引
    /// 取决于计算寄存器扫描结果，而非固定 numInputs。
    uint16_t numInputs = 0;

    /// 已分配的最大寄存器索引 + 1（含参数、常量槽、计算寄存器）。
    uint16_t maxReg = 0;

    /// 指令发射计数器：pickOpcodeByte 的 nonce（每条指令递增，确定性选别名）。
    uint64_t instructionNonce = 0;

    /// 每条指令的起始偏移（emitOpcode/emitHaltOpcode 记录，与 bytecodes 同步
    /// 增长）。编码收尾搬移进 VmProgram.instructionOffsets，供懒解密分块
    /// （issue #760 切片 2）做指令对齐的块边界。
    std::vector<uint32_t> instructionOffsets;

    /// 带边拷贝（块参数 RegCopy）的边 → 合成块。合成块内容 = [Copy...] + Jump 真实目标，
    /// 索引排在所有真实块之后（blockTargets 后半段）。每条带拷贝的边一个合成块，
    /// 按 (src 块, dst 块, slot) 区分（slot：Jump=0，Branch true=0/false=1）。
    struct SyntheticEdge
    {
        const RegBlock*           realTarget;
        std::vector<RegCopy>      copies;   ///< 边拷贝快照（值语义，安全持有）
        uint16_t                  index = 0;
    };
    std::vector<SyntheticEdge> syntheticEdges;

    /// (src 块, dst 块, slot) → 合成块索引，供 encodeJump/encodeBranch 解析目标。
    struct EdgeKey
    {
        const RegBlock* src = nullptr;
        const RegBlock* dst = nullptr;
        uint16_t        slot = 0;
    };
    std::vector<std::pair<EdgeKey, uint16_t>> edgeToSynthetic;

    explicit EncoderState(const RegFunction& f, const VmInstance& inst)
        : fn(f)
        , instance(inst)
    {
        numInputs = static_cast<uint16_t>(f.parameters.size());
        // 预扫描全部计算寄存器（指令 dest + Register 操作数），把常量槽隔离到
        // 计算寄存器空间之外——否则常量槽与 SSA value id（密集递增）重叠，
        // 常量复用或槽被后续 dest 覆盖后读参即错值。
        maxReg = numInputs;
        scanComputeRegisters();
    }

    /// @brief 为带边拷贝的后继创建合成块并分配索引（在 indexBlocks 之后调用）。
    ///
    /// @details
    /// SSA 块参数经 RegIR 消除后变成后继边上的 RegCopy（reg_lowering.cpp lowerSuccessor）。
    /// 这些拷贝必须在该边被取用时执行——MVP 的解释器只有 Copy/Jump/Branch 原语，
    /// 因此把每条带拷贝的边重写为「合成块：Copy... + Jump 真实目标」，跳转/分支
    /// 的目标指向合成块。无拷贝的边仍直接指向真实块。
    void planSyntheticEdges()
    {
        const uint16_t base = static_cast<uint16_t>(fn.blocks.size());
        auto plan = [&](const RegBlock* src, const RegSuccessor& succ, uint16_t slot) {
            if (succ.block == nullptr || succ.copies.empty()) return;
            SyntheticEdge edge;
            edge.realTarget = succ.block;
            edge.copies     = succ.copies;
            edge.index      = static_cast<uint16_t>(base + syntheticEdges.size());
            syntheticEdges.push_back(std::move(edge));
            edgeToSynthetic.push_back({{src, succ.block, slot}, syntheticEdges.back().index});
        };
        for (const auto& block : fn.blocks) {
            std::visit(
                [&](const auto& term) {
                    using T = std::decay_t<decltype(term)>;
                    if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                        plan(block.get(), term.target, 0);
                    } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                        plan(block.get(), term.trueTarget, 0);
                        plan(block.get(), term.falseTarget, 1);
                    } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                        // Switch 每条 case 边 + default 边都可能带拷贝（块参数
                        // RegCopy）。slot 按后继序编号（case 0..n-1，default=n），
                        // 与 encodeSwitch 的 resolveEdgeTarget 约定一致——同一
                        // dst 的多条边（如循环内 switch 各 case 汇入循环尾）靠
                        // 不同 slot 区分合成块，各自执行自己的拷贝。
                        uint16_t slot = 0;
                        for (const auto& c : term.cases) plan(block.get(), c.target, slot++);
                        plan(block.get(), term.defaultTarget, slot);
                    }
                    // Throw/Return/Unreachable：无后继边拷贝。
                },
                block->terminator);
        }
    }

    /// @brief 解析边目标：该边有合成块则用合成块索引，否则用真实块索引。
    uint16_t resolveEdgeTarget(const RegBlock* src, const RegBlock* dst, uint16_t slot,
                               uint16_t realIndex)
    {
        for (const auto& kv : edgeToSynthetic) {
            if (kv.first.src == src && kv.first.dst == dst && kv.first.slot == slot)
                return kv.second;
        }
        return realIndex;
    }

    /// @brief 第一遍：扫描所有指令的 dest 与 Register 操作数，抬高 maxReg。
    ///
    /// @details
    /// 只观察 Register 操作数，字面量由 internConstant 在编码期分配槽位；
    /// 由于 maxReg 已含全部计算寄存器，常量槽必然落在计算寄存器之后，互不重叠。
    /// 边拷贝（后继块的块参数 RegCopy）也会占寄存器，必须一并扫描，否则常量槽
    /// 可能落在边拷贝 dest 寄存器之上被覆盖。
    void scanComputeRegisters()
    {
        for (const auto& block : fn.blocks) {
            for (const auto& ins : block->instructions) {
                observeReg(static_cast<uint16_t>(ins.dest.id.value));
                switch (ins.kind) {
                case RegInstructionKind::Copy:
                    observeReg(static_cast<uint16_t>(ins.copy.source.reg.value));
                    break;
                case RegInstructionKind::Unary:
                    observeReg(static_cast<uint16_t>(ins.value.reg.value));
                    break;
                case RegInstructionKind::Binary:
                case RegInstructionKind::Compare:
                    observeReg(static_cast<uint16_t>(ins.lhs.reg.value));
                    observeReg(static_cast<uint16_t>(ins.rhs.reg.value));
                    break;
                case RegInstructionKind::Convert:
                    observeReg(static_cast<uint16_t>(ins.value.reg.value));
                    break;
                default: break;
                }
            }
            const auto& term = block->terminator;
            std::visit([&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    for (const auto& copy : t.target.copies) {
                        observeReg(static_cast<uint16_t>(copy.dest.id.value));
                        observeReg(static_cast<uint16_t>(copy.source.reg.value));
                    }
                } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    observeReg(static_cast<uint16_t>(t.condition.reg.value));
                    for (const auto& copy : t.trueTarget.copies) {
                        observeReg(static_cast<uint16_t>(copy.dest.id.value));
                        observeReg(static_cast<uint16_t>(copy.source.reg.value));
                    }
                    for (const auto& copy : t.falseTarget.copies) {
                        observeReg(static_cast<uint16_t>(copy.dest.id.value));
                        observeReg(static_cast<uint16_t>(copy.source.reg.value));
                    }
                } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    // Switch 边拷贝与 Jump/Branch 同源（planSyntheticEdges 对三者
                    // 一视同仁）；漏扫会使常量槽落在 switch 边拷贝 dest 之上被
                    // 覆写，或 numRegs 偏小致解释器 regs[] 越界。
                    observeReg(static_cast<uint16_t>(t.value.reg.value));
                    for (const auto& c : t.cases) {
                        for (const auto& copy : c.target.copies) {
                            observeReg(static_cast<uint16_t>(copy.dest.id.value));
                            observeReg(static_cast<uint16_t>(copy.source.reg.value));
                        }
                    }
                    for (const auto& copy : t.defaultTarget.copies) {
                        observeReg(static_cast<uint16_t>(copy.dest.id.value));
                        observeReg(static_cast<uint16_t>(copy.source.reg.value));
                    }
                }
            },
                       term);
        }
    }

    /// 更新 maxReg（计算寄存器索引来自 RegId.value）。
    void observeReg(uint16_t idx)
    {
        if (idx >= maxReg) maxReg = idx + 1;
    }

    /// @brief 为字面量分配/复用一个常量寄存器槽。
    ///
    /// @details
    /// 常量槽从 maxReg 高水位（已含全部计算寄存器）之后连续分配，与计算寄存器
    /// 空间隔离。相同签名的字面量复用同一槽（去重）。
    /// 返回的索引写入 bytecodes 作为操作数，prologue 据此从 constantPool 预加载。
    uint16_t internConstant(VmType type, int64_t intBits, double floatValue)
    {
        std::string sig;
        sig.push_back(static_cast<char>(type));
        sig.append(reinterpret_cast<const char*>(&intBits), sizeof(int64_t));
        sig.append(reinterpret_cast<const char*>(&floatValue), sizeof(double));

        auto it = constSlotBySignature.find(sig);
        if (it != constSlotBySignature.end()) {
            observeReg(it->second);
            return it->second;
        }

        const uint16_t slot = maxReg;   // 常量槽紧接在已分配寄存器之后
        observeReg(slot);
        constSlotBySignature[sig] = slot;
        return slot;
    }

    /// @brief 为一个 Call 调用点分配/复用方法引用索引（Phase 2.0）。
    ///
    /// @details
    /// 相同 (owner, name, desc, invokeKind, hasDest) 的调用点复用同一 callSiteIdx，
    /// 共享一个 backend 发射的 helper。返回值写入字节流作为 Call 指令的操作数。
    uint16_t internMethodRef(VmMethodRef mref)
    {
        std::string key = mref.owner;
        key.push_back('\0');
        key += mref.name;
        key.push_back('\0');
        key += mref.desc;
        key.push_back('\0');
        key.push_back(static_cast<char>(mref.invokeKind));
        key.push_back(mref.hasDest ? '\1' : '\0');
        // 用户自定义 intrinsic（issue #818）：userSymbol 参与去重，用户降级
        // 与普通 JNI helper 是不同实现，不得共享 callSiteIdx。
        key.push_back('\0');
        key += mref.userSymbol;
        key.push_back(mref.userPassContext ? '\1' : '\0');
        auto it = methodRefByKey.find(key);
        if (it != methodRefByKey.end()) return it->second;
        const uint16_t idx = static_cast<uint16_t>(methodRefs.size());
        methodRefByKey[key] = idx;
        methodRefs.push_back(std::move(mref));
        return idx;
    }

    /// @brief 为一个字段访问点分配/复用字段引用索引（Phase 2.A）。
    ///
    /// @details
    /// 相同 (owner, name, desc, isStatic) 的访问点复用同一 fieldSiteIdx，共享一个
    /// backend 发射的 helper。load/store 各自的表传入（refs/keyMap），故同名同表内去重。
    uint16_t internFieldRef(std::vector<VmFieldRef>& refs,
                            std::unordered_map<std::string, uint16_t>& keyMap,
                            VmFieldRef fref)
    {
        std::string key = fref.owner;
        key.push_back('\0');
        key += fref.name;
        key.push_back('\0');
        key += fref.desc;
        key.push_back('\0');
        key.push_back(fref.isStatic ? '\1' : '\0');
        auto it = keyMap.find(key);
        if (it != keyMap.end()) return it->second;
        const uint16_t idx = static_cast<uint16_t>(refs.size());
        keyMap[key] = idx;
        refs.push_back(std::move(fref));
        return idx;
    }

    /// @brief 为一个数组访问点分配/复用元素类型索引（Phase 2.A）。
    ///
    /// @details
    /// 按 elemKind 去重（同元素类型的访问点共享一个 helper）。elemKind 用 uint8_t 当 key。
    uint16_t internArrayRef(std::vector<VmArrayRef>& refs,
                            std::unordered_map<uint8_t, uint16_t>& keyMap,
                            VmArrayElemKind elemKind)
    {
        const uint8_t k = static_cast<uint8_t>(elemKind);
        auto it = keyMap.find(k);
        if (it != keyMap.end()) return it->second;
        const uint16_t idx = static_cast<uint16_t>(refs.size());
        keyMap[k] = idx;
        refs.push_back(VmArrayRef{elemKind});
        return idx;
    }

    /// @brief 为一个 Allocate 调用点分配/复用索引（Phase 2.A）。
    ///
    /// @details
    /// 按 (kind, typeDesc) 去重，相同分配形态共享一个 helper。
    uint16_t internAllocRef(VmAllocRef aref)
    {
        std::string key;
        key.push_back(static_cast<char>(aref.kind));
        key.push_back('\0');
        key += aref.typeDesc;
        auto it = allocByKey.find(key);
        if (it != allocByKey.end()) return it->second;
        const uint16_t idx = static_cast<uint16_t>(allocRefs.size());
        allocByKey[key] = idx;
        allocRefs.push_back(std::move(aref));
        return idx;
    }

    /// @brief 为一个 Intrinsic 调用点分配/复用索引（Phase 2.D）。
    ///
    /// @details
    /// 按 name 去重（同 intrinsic 共享 helper；instanceof:<desc> 的 desc 在 name 里，
    /// 不同 desc 是不同 intrinsic）。
    uint16_t internIntrinsicRef(VmIntrinsicRef iref)
    {
        auto it = intrinsicByKey.find(iref.name);
        if (it != intrinsicByKey.end()) return it->second;
        const uint16_t idx = static_cast<uint16_t>(intrinsicRefs.size());
        intrinsicByKey[iref.name] = idx;
        intrinsicRefs.push_back(std::move(iref));
        return idx;
    }

    /// @brief 分配/复用 String LDC 常量槽（Phase 2.E）。
    ///
    /// @details
    /// ldc_string Opaque 在普通指令路径物质化为专用寄存器槽（与 internConstant
    /// 同模式：槽落在计算寄存器之后），wrapper 在 exec 前用 NewStringUTF 预加载。
    /// 按内容去重：同一字面量多处 LDC 共享一个槽。
    uint16_t internStringLiteral(const std::string& value)
    {
        auto it = stringSlotByValue.find(value);
        if (it != stringSlotByValue.end()) {
            observeReg(it->second);
            return it->second;
        }
        const uint16_t slot = maxReg;   // 常量槽紧接在已分配寄存器之后
        observeReg(slot);
        stringSlotByValue[value] = slot;
        VmStringLiteral lit;
        lit.slot  = slot;
        lit.value = value;
        stringLiterals.push_back(std::move(lit));
        return slot;
    }

    /// @brief 分配/复用 `.class` 字面量常量槽（ldc_class Opaque）。
    ///
    /// @details 与 internStringLiteral 同模式：专用寄存器槽 + wrapper 预加载
    /// （FindClass）。按内部名去重。
    uint16_t internClassLiteral(const std::string& typeDesc)
    {
        auto it = classSlotByDesc.find(typeDesc);
        if (it != classSlotByDesc.end()) {
            observeReg(it->second);
            return it->second;
        }
        const uint16_t slot = maxReg;
        observeReg(slot);
        classSlotByDesc[typeDesc] = slot;
        VmClassLiteral lit;
        lit.slot     = slot;
        lit.typeDesc = typeDesc;
        classLiterals.push_back(std::move(lit));
        return slot;
    }

    /// @brief 解析一个 RegOperand 为 VM 寄存器索引。
    ///
    /// @details
    /// - Register：直接用 reg.value（计算寄存器）
    /// - IntLiteral/FloatLiteral：intern 到常量池，返回常量槽索引
    /// - NullLiteral：MVP 不支持（fail-closed）
    /// - Opaque：tag 以 "ldc_string:" 开头的 String LDC 物质化为字符串常量槽
    ///   （wrapper 用 NewStringUTF 预加载，见 internStringLiteral）；边拷贝路径
    ///   （allowOpaqueNull）容忍其余 Opaque 为 REF NULL 常量槽（对齐 RegC
    ///   "Opaque → NULL"：异常 handler 参数/undef 边值在 JVM 语义下不可达或为
    ///   null，赋 NULL 安全）；普通指令路径 fail-closed
    ///
    /// @return 成功返回 true 并填 regIdx；失败返回 false 并填 reason。
    bool resolveOperand(const RegOperand& op, uint16_t& regIdx, std::string& reason,
                        bool allowOpaqueNull = false)
    {
        switch (op.kind) {
        case RegOperandKind::Register: {
            regIdx = static_cast<uint16_t>(op.reg.value);
            observeReg(regIdx);
            return true;
        }
        case RegOperandKind::IntLiteral: {
            VmType vt;
            if (!toVmType(op.type, vt)) {
                reason = "IntLiteral with unsupported VM type";
                return false;
            }
            regIdx = internConstant(vt, op.intValue, 0.0);
            return true;
        }
        case RegOperandKind::FloatLiteral: {
            VmType vt;
            if (!toVmType(op.type, vt)) {
                reason = "FloatLiteral with unsupported VM type";
                return false;
            }
            regIdx = internConstant(vt, 0, op.floatValue);
            return true;
        }
        case RegOperandKind::NullLiteral:
            // REF NULL 常量槽（prologue 写 .i=0，union 双关 .ref==NULL）——
            // 与 allowOpaqueNull 的 undef 边值物质化同一机制。NullLiteral 的
            // 语义场景（== null 比较 / return null / null 实参 / checkcast null
            // / null 存储）全部等价于 REF NULL。
            regIdx = internConstant(VmType::REF, 0, 0.0);
            return true;
        case RegOperandKind::Opaque: {
            // String LDC（Phase 2.E）：Opaque tag 携带 UTF-8 内容（ssa_builder.cpp:99），
            // 物质化为字符串常量槽，wrapper 在 exec 前 NewStringUTF 预加载。
            constexpr const char* kLdcStringPrefix = "ldc_string:";
            if (op.tag.rfind(kLdcStringPrefix, 0) == 0) {
                regIdx = internStringLiteral(op.tag.substr(std::strlen(kLdcStringPrefix)));
                return true;
            }
            // `.class` 字面量：Opaque tag 携带内部名（ldc_class:<内部名>），
            // 物质化为 Class 常量槽，wrapper 在 exec 前 FindClass 预加载
            // （JVM ldc 类常量语义：触发加载不触发初始化）。
            constexpr const char* kLdcClassPrefix = "ldc_class:";
            if (op.tag.rfind(kLdcClassPrefix, 0) == 0) {
                regIdx = internClassLiteral(op.tag.substr(std::strlen(kLdcClassPrefix)));
                return true;
            }
            // undef 值在边拷贝上下文按 NULL 引用物质化：REF NULL 常量槽 prologue
            // 写 .i=0（union 双关，.ref==NULL）。
            if (allowOpaqueNull) {
                regIdx = internConstant(VmType::REF, 0, 0.0);
                return true;
            }
            reason = "Opaque operand not supported in MVP";
            if (!op.tag.empty()) reason += " (tag: " + op.tag + ")";
            return false;
        }
        }
        reason = "unknown RegOperandKind";
        return false;
    }
};

/// 写入 VmOpcode 的实例字节。drift 开启时按字节流偏移选变体（语义标签轮转），
/// 与解释器 variantForPc 同一公式天然同步；nonce 负责 canonical/ALT 别名的
/// 确定性选取（同一语义集合内多字节）。
void emitOpcode(EncoderState& ctx, VmOpcode op, std::vector<uint8_t>& out)
{
    const uint64_t pc = out.size();
    // 指令边界记录（issue #760 切片 2）：opcode 字节恒为指令首字节，
    // emitOpcode 是全部指令发射的唯一入口（Halt 走 emitHaltOpcode 同样记录）。
    ctx.instructionOffsets.push_back(static_cast<uint32_t>(pc));
    uint8_t byte;
    if (ctx.instance.driftEnabled()) {
        const int d = ctx.instance.variantForPc(pc);
        byte = ctx.instance.variantPickOpcodeByte(op, ctx.instructionNonce++, d);
    } else {
        byte = ctx.instance.pickOpcodeByte(op, ctx.instructionNonce++);
    }
    emitU8(out, byte);
}

/// 发射 Halt 哨兵：固定用 canonical 字节（发射器的 Halt case 覆盖全部别名，
/// 别名槽在字节流里不会出现，仅作为防御性停机面）。
void emitHaltOpcode(EncoderState& ctx, std::vector<uint8_t>& out)
{
    ctx.instructionOffsets.push_back(static_cast<uint32_t>(out.size()));
    emitU8(out, ctx.instance.opcodeFor(VmOpcode::Halt));
}

// ---------------------------------------------------------------------------
// 前向声明：layoutBlocks → encodeBlock → encodeInstruction/encodeTerminator →
// encodeCopy/Unary/Binary/Compare/Convert/Jump/Branch/Return。
// ---------------------------------------------------------------------------
bool layoutBlocks(EncoderState& ctx, VmProgram& prog, std::string& reason);
bool encodeBlock(EncoderState& ctx, const RegBlock& block, VmProgram& prog, std::string& reason);
bool encodeInstruction(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog,
                       std::string& reason);
bool encodeTerminator(EncoderState& ctx, const RegBlock& block, VmProgram& prog, std::string& reason);
bool encodeCopy(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeUnary(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeBinary(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeCompare(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeConvert(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeCall(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeLoadField(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeStoreField(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeArrayLoad(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeArrayStore(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeAllocate(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeIntrinsic(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason);
bool encodeJump(EncoderState& ctx, const RegJumpTerminator& term, VmProgram& prog,
                std::string& reason);
bool encodeThrow(EncoderState& ctx, const RegThrowTerminator& term, VmProgram& prog,
                 std::string& reason);
bool encodeBranch(EncoderState& ctx, const RegBranchTerminator& term, VmProgram& prog,
                  std::string& reason);
bool encodeReturn(EncoderState& ctx, const RegReturnTerminator& term, VmProgram& prog,
                  std::string& reason);

/// @brief 第一遍：为每个 block 分配索引，记录到 ctx.blockIndex。
///
/// @details
/// block 索引用作 blockTargets 的下标和 Jump/Branch 的目标操作数。
/// fn.blocks 的顺序即索引顺序（entry 是 blocks[0]，见 reg_function.hpp:38）。
bool indexBlocks(EncoderState& ctx, std::string& reason)
{
    if (ctx.fn.blocks.size() > 0xFFFF) {
        reason = "too many blocks (>65535) for u16 block index";
        return false;
    }
    uint16_t idx = 0;
    for (const auto& block : ctx.fn.blocks) {
        ctx.blockIndex[block.get()] = idx++;
    }
    return true;
}

/// @brief 第二遍 A：计算每个 block 的字节码长度，填 blockTargets（起始 offset）。
///
/// @details
/// 一遍完成：按 block 顺序边编码边记录起始 offset（block 的偏移只取决于前面
/// block 的字节数，故一遍即够）。真实块之后布局合成块（边拷贝块，见
/// EncoderState::planSyntheticEdges），末尾追加 Halt 作为停机哨兵。
bool layoutBlocks(EncoderState& ctx, VmProgram& prog, std::string& reason)
{
    prog.blockTargets.assign(ctx.fn.blocks.size() + ctx.syntheticEdges.size(), 0);
    for (size_t bi = 0; bi < ctx.fn.blocks.size(); ++bi) {
        prog.blockTargets[bi] = static_cast<uint32_t>(prog.bytecodes.size());
        // 编码本 block 到 prog.bytecodes（offset 已记录，后续编码直接追加）。
        if (!encodeBlock(ctx, *ctx.fn.blocks[bi], prog, reason)) return false;
    }
    // 合成块：边拷贝 + Jump 到真实目标。
    for (const auto& edge : ctx.syntheticEdges) {
        prog.blockTargets[edge.index] = static_cast<uint32_t>(prog.bytecodes.size());
        for (const auto& copy : edge.copies) {
            VmType type;
            if (!toVmType(copy.dest.type, type)) {
                reason = "edge copy dest type not supported in MVP";
                return false;
            }
            uint16_t src;
            // 边拷贝的 undef 源（异常 handler 参数未定义）容忍为 NULL 引用。
            if (!ctx.resolveOperand(copy.source, src, reason, true)) return false;
            emitOpcode(ctx, VmOpcode::Copy, prog.bytecodes);
            emitU8(prog.bytecodes, static_cast<uint8_t>(type));
            emitU16(prog.bytecodes, static_cast<uint16_t>(copy.dest.id.value));
            emitU16(prog.bytecodes, src);
        }
        const auto realIt = ctx.blockIndex.find(edge.realTarget);
        if (realIt == ctx.blockIndex.end()) {
            reason = "edge copy target block not indexed";
            return false;
        }
        emitOpcode(ctx, VmOpcode::Jump, prog.bytecodes);
        emitU16(prog.bytecodes, realIt->second);
    }
    // 末尾 Halt 作为停机哨兵（解释器主循环遇到 Halt 退出）。
    emitHaltOpcode(ctx, prog.bytecodes);
    return true;
}

/// @brief 编码单个 block 的指令 + terminator。
///
/// @details
/// 在 layoutBlocks 里被调用，直接追加到 prog.bytecodes。block 起始 offset
/// 已由调用方记录到 blockTargets。
bool encodeBlock(EncoderState& ctx, const RegBlock& block, VmProgram& prog, std::string& reason)
{
    for (const auto& ins : block.instructions) {
        if (!encodeInstruction(ctx, ins, prog, reason)) return false;
    }
    return encodeTerminator(ctx, block, prog, reason);
}

bool encodeInstruction(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog,
                       std::string& reason)
{
    switch (ins.kind) {
    case RegInstructionKind::Copy: return encodeCopy(ctx, ins, prog, reason);
    case RegInstructionKind::Unary: return encodeUnary(ctx, ins, prog, reason);
    case RegInstructionKind::Binary: return encodeBinary(ctx, ins, prog, reason);
    case RegInstructionKind::Compare: return encodeCompare(ctx, ins, prog, reason);
    case RegInstructionKind::Convert: return encodeConvert(ctx, ins, prog, reason);
    case RegInstructionKind::Call: return encodeCall(ctx, ins, prog, reason);
    case RegInstructionKind::LoadField: return encodeLoadField(ctx, ins, prog, reason);
    case RegInstructionKind::StoreField: return encodeStoreField(ctx, ins, prog, reason);
    case RegInstructionKind::ArrayLoad: return encodeArrayLoad(ctx, ins, prog, reason);
    case RegInstructionKind::ArrayStore: return encodeArrayStore(ctx, ins, prog, reason);
    case RegInstructionKind::Allocate: return encodeAllocate(ctx, ins, prog, reason);
    case RegInstructionKind::Intrinsic: return encodeIntrinsic(ctx, ins, prog, reason);
    case RegInstructionKind::Barrier:
        reason = "RegIR family not supported in VMC4 yet (barrier)";
        return false;
    }
    reason = "unknown RegInstructionKind";
    return false;
}

bool encodeCopy(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    VmType type;
    if (!toVmType(ins.dest.type, type)) {
        reason = "Copy dest type not supported in MVP";
        return false;
    }
    // Copy 指令的源操作数存在 ins.copy.source（不是 ins.value——那是 Unary/Convert
    // 用的字段，对 Copy 是默认构造的 Opaque）。
    uint16_t src;
    if (!ctx.resolveOperand(ins.copy.source, src, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitOpcode(ctx, VmOpcode::Copy, prog.bytecodes);
    emitU8(prog.bytecodes, static_cast<uint8_t>(type));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, src);
    return true;
}

bool encodeUnary(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    VmType type;
    if (!toVmType(ins.dest.type, type)) {
        reason = "Unary dest type not supported in MVP";
        return false;
    }
    VmUnaryOp op;
    if (!toVmUnaryOp(ins.unaryOp, op)) {
        reason = "Unary op not supported";
        return false;
    }
    uint16_t src;
    if (!ctx.resolveOperand(ins.value, src, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitOpcode(ctx, VmOpcode::Unary, prog.bytecodes);
    emitU8(prog.bytecodes, static_cast<uint8_t>(op));
    emitU8(prog.bytecodes, static_cast<uint8_t>(type));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, src);
    return true;
}

bool encodeBinary(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    VmType type;
    if (!toVmType(ins.dest.type, type)) {
        reason = "Binary dest type not supported in MVP";
        return false;
    }
    VmBinaryOp op;
    if (!toVmBinaryOp(ins.binaryOp, op)) {
        reason = "Binary op not supported";
        return false;
    }

    // super-operator 折叠（Phase 3）：Binary(x, 1, Add/Sub) → SuperInc/SuperDec。
    // 语义等价（整数环绕/浮点 ±1 一致），但指令格式更短（无 rhs operand），
    // 且引入独立 opcode 增加指令集膨胀。rhs 必须是 IntLiteral(1) 字面量。
    if (ins.rhs.kind == RegOperandKind::IntLiteral && ins.rhs.intValue == 1 &&
        (ins.binaryOp == SSABinaryOp::Add || ins.binaryOp == SSABinaryOp::Sub)) {
        const VmOpcode superOp = ins.binaryOp == SSABinaryOp::Add ? VmOpcode::SuperInc
                                                                  : VmOpcode::SuperDec;
        uint16_t src;
        if (!ctx.resolveOperand(ins.lhs, src, reason)) return false;
        const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
        ctx.observeReg(dest);
        emitOpcode(ctx, superOp, prog.bytecodes);
        emitU8(prog.bytecodes, static_cast<uint8_t>(type));
        emitU16(prog.bytecodes, dest);
        emitU16(prog.bytecodes, src);
        return true;
    }

    uint16_t lhs, rhs;
    if (!ctx.resolveOperand(ins.lhs, lhs, reason)) return false;
    if (!ctx.resolveOperand(ins.rhs, rhs, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitOpcode(ctx, VmOpcode::Binary, prog.bytecodes);
    emitU8(prog.bytecodes, static_cast<uint8_t>(op));
    emitU8(prog.bytecodes, static_cast<uint8_t>(type));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, lhs);
    emitU16(prog.bytecodes, rhs);
    return true;
}

bool encodeCompare(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    // Compare 结果类型恒为 I32（布尔条件），但 operand 类型来自 lhs/rhs。
    VmType operandType;
    if (!toVmType(ins.lhs.type, operandType)) {
        reason = "Compare operand type not supported in MVP";
        return false;
    }
    VmComparePred pred;
    if (!toVmComparePred(ins.comparePredicate, pred)) {
        reason = "Compare predicate not supported";
        return false;
    }
    // REF 只支持 Eq/Ne（对象身份比较，解释器用 IsSameObject）；有序比较
    // 在 JVM 字节码中不存在（IF_ACMPEQ/IF_ACMPNE 只有 eq/ne）。
    if (operandType == VmType::REF && pred != VmComparePred::Eq && pred != VmComparePred::Ne) {
        reason = "REF ordered comparison not supported";
        return false;
    }
    uint16_t lhs, rhs;
    if (!ctx.resolveOperand(ins.lhs, lhs, reason)) return false;
    if (!ctx.resolveOperand(ins.rhs, rhs, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitOpcode(ctx, VmOpcode::Compare, prog.bytecodes);
    emitU8(prog.bytecodes, static_cast<uint8_t>(pred));
    emitU8(prog.bytecodes, static_cast<uint8_t>(operandType));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, lhs);
    emitU16(prog.bytecodes, rhs);
    return true;
}

bool encodeConvert(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    // CHECKCAST（ReferenceCast）→ castcheck:<desc> intrinsic：IsInstanceOf +
    // 失败抛 ClassCastException（null 通过）。其余非 Numeric convert 形态
    // 保持 fail-closed。
    if (ins.convertKind != SSAConvertKind::Numeric) {
        if (ins.convertKind != SSAConvertKind::ReferenceCast) {
            reason = "Convert: unsupported convert kind";
            return false;
        }
        if (ins.referenceTypeDesc.is_empty()) {
            reason = "Convert: ReferenceCast missing type descriptor";
            return false;
        }
        RegInstruction asIntrinsic = ins;
        asIntrinsic.kind           = RegInstructionKind::Intrinsic;
        asIntrinsic.intrinsicName =
            std::string("castcheck:") + ins.referenceTypeDesc.to_std_string();
        asIntrinsic.args = {ins.value};
        return encodeIntrinsic(ctx, asIntrinsic, prog, reason);
    }
    VmType from, to;
    if (!toVmType(ins.value.type, from)) {
        reason = "Convert source type not supported in MVP";
        return false;
    }
    if (!toVmType(ins.targetType, to)) {
        reason = "Convert target type not supported in MVP";
        return false;
    }
    uint16_t src;
    if (!ctx.resolveOperand(ins.value, src, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitOpcode(ctx, VmOpcode::Convert, prog.bytecodes);
    emitU8(prog.bytecodes, static_cast<uint8_t>(from));
    emitU8(prog.bytecodes, static_cast<uint8_t>(to));
    emitU16(prog.bytecodes, dest);
    emitU16(prog.bytecodes, src);
    return true;
}

/// @brief 编码 Call 指令（Phase 2.0）。
///
/// @details
/// 把 SSAMethodRef 收集到 VmProgram.methodRefs，字节流只存 callSiteIdx。
/// backend 为每个 methodRef 发射一个 TU-local helper（按 invokeKind + desc 调 JNI），
/// 解释器经函数指针表 mj2x_vm_call_table_<tag>[callSiteIdx] 分派。
///
/// 字节流：[Call][callSiteIdx:u16][recv:u16][dest:u16][argCount:u8][args:argCount×u16]
/// - recv：receiver 寄存器（static 调用时填 0，helper 不读）
/// - dest：结果寄存器（void 方法填 0，helper 不写）
bool encodeCall(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    // JVM invoke opcode → VmInvokeKind（182=virtual,183=special,184=static,185=interface）。
    VmInvokeKind ik;
    switch (ins.method.invokeOpcode) {
    case 182: ik = VmInvokeKind::Virtual; break;
    case 183: ik = VmInvokeKind::Special; break;
    case 184: ik = VmInvokeKind::Static; break;
    case 185: ik = VmInvokeKind::Interface; break;
    default:
        reason = "Call: unknown invoke opcode " + std::to_string(ins.method.invokeOpcode);
        return false;
    }
    const bool isStatic = (ik == VmInvokeKind::Static);

    // SSA 约定（ssa_instruction.hpp:223）：instance 调用 args[0]=receiver，其后是描述符
    // 参数；static 调用 args 全是描述符参数。RegInstruction.receiver 字段 Call 不填（留
    // 默认 Opaque），receiver 经 args[0] 取。故 argKinds/argIdxs 只含描述符参数。
    const size_t startArg = isStatic ? 0 : 1;
    if (!isStatic && ins.args.empty()) {
        reason = "Call: instance invoke missing receiver (args[0])";
        return false;
    }

    VmMethodRef mref;
    mref.owner      = ins.method.owner.to_std_string();
    mref.name       = ins.method.name.to_std_string();
    mref.desc       = ins.method.desc.to_std_string();
    mref.invokeKind = ik;
    mref.hasDest    = ins.hasDest;
    mref.retKind    = ins.hasDest ? toVmJniKind(ins.dest.type) : VmJniKind::Void;
    // argKinds 仅含描述符参数（不含 receiver），与 desc 对齐，与下方 argIdxs 同序。
    mref.argKinds.reserve(ins.args.size() - startArg);
    for (size_t a = startArg; a < ins.args.size(); ++a) {
        mref.argKinds.push_back(toVmJniKind(ins.args[a].type));
    }
    // 用户自定义 intrinsic（issue #818）：匹配调用点降级为直接调用用户 symbol
    // （helper 解包参数，不经 JNI 通用派发）。匹配契约：static 调用点匹配
    // passReceiver=false 条目；instance 调用点仅匹配显式 passReceiver=true
    // 条目（未声明不匹配，走通用 JNI helper——G-1 行为保持）；static 调用点
    // 遇 passReceiver 条目属配置错误（静态方法无 receiver），整方法编码失败。
    if (ctx.userIntrinsics != nullptr) {
        const std::string owner = ins.method.owner.to_std_string();
        const std::string name  = ins.method.name.to_std_string();
        const std::string desc  = ins.method.desc.to_std_string();
        for (const auto& u : *ctx.userIntrinsics) {
            if (u.owner != owner || u.name != name || u.desc != desc) continue;
            if (isStatic && u.passReceiver) {
                reason = "user intrinsic " + owner + "." + name + desc +
                         ": passReceiver=true cannot match INVOKESTATIC call site "
                         "(static methods have no receiver)";
                return false;
            }
            if (!isStatic && !u.passReceiver) break;
            mref.userSymbol       = u.symbol;
            mref.userPassContext  = u.passContext;
            mref.userPassReceiver = u.passReceiver;
            break;
        }
    }
    const uint16_t callSiteIdx = ctx.internMethodRef(std::move(mref));

    // receiver 寄存器（instance 调用取 args[0]）；static 调用 recv=0（helper 不读）。
    uint16_t recv = 0;
    if (!isStatic) {
        if (!ctx.resolveOperand(ins.args[0], recv, reason)) return false;
    }

    // 返回值寄存器（void 方法无 dest）。
    uint16_t dest = 0;
    if (ins.hasDest) {
        dest = static_cast<uint16_t>(ins.dest.id.value);
        ctx.observeReg(dest);
    }

    // 描述符参数寄存器列表（变长，不含 receiver）。argCount 上限 32：解释器 handler
    // 用固定大小 argBuf[32] 承载参数索引数组（避免动态分配）；JVM 实际参数极少超此数。
    const size_t descArgCount = ins.args.size() - startArg;
    if (descArgCount > 32) {
        reason = "Call: too many arguments (>32)";
        return false;
    }
    std::vector<uint16_t> argIdxs;
    argIdxs.reserve(descArgCount);
    for (size_t a = startArg; a < ins.args.size(); ++a) {
        uint16_t ai;
        if (!ctx.resolveOperand(ins.args[a], ai, reason)) return false;
        argIdxs.push_back(ai);
    }

    emitOpcode(ctx, VmOpcode::Call, prog.bytecodes);
    emitU16(prog.bytecodes, callSiteIdx);
    emitU16(prog.bytecodes, recv);
    emitU16(prog.bytecodes, dest);
    emitU8(prog.bytecodes, static_cast<uint8_t>(argIdxs.size()));
    for (uint16_t ai : argIdxs) emitU16(prog.bytecodes, ai);
    return true;
}

/// @brief 编码 LoadField 指令（Phase 2.A：getfield/getstatic）。
///
/// @details
/// 把 SSAFieldRef 收集到 VmProgram.loadFieldRefs，字节流只存 fieldSiteIdx。
/// backend 为每项发射一个 helper（懒缓存 fieldID + Get[Static]<Type>Field）。
///
/// 字节流：[LoadField][fieldSiteIdx:u16][recv:u16][dest:u16][argc:u8=0]
/// - recv：receiver 寄存器（static 时填 0，helper 不读）
/// - dest：结果寄存器（字段值写入处）
/// - argc 恒为 0：field load 无额外参数，handler 与 Call 同结构以复用 argBuf 机制
bool encodeLoadField(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    VmFieldRef fref;
    fref.owner    = ins.field.owner.to_std_string();
    fref.name     = ins.field.name.to_std_string();
    fref.desc     = ins.field.desc.to_std_string();
    fref.isStatic = ins.field.isStatic;
    fref.jniKind  = toVmJniKind(ins.dest.type);
    const uint16_t siteIdx =
        ctx.internFieldRef(ctx.loadFieldRefs, ctx.loadFieldByKey, std::move(fref));

    // instance 字段 receiver；static 字段无 receiver（helper 不读 recv）。
    uint16_t recv = 0;
    if (!ins.field.isStatic) {
        if (!ctx.resolveOperand(ins.receiver, recv, reason)) return false;
    }
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitOpcode(ctx, VmOpcode::LoadField, prog.bytecodes);
    emitU16(prog.bytecodes, siteIdx);
    emitU16(prog.bytecodes, recv);
    emitU16(prog.bytecodes, dest);
    emitU8(prog.bytecodes, 0);   // argc = 0（LoadField 无额外参数）
    return true;
}

/// @brief 编码 StoreField 指令（Phase 2.A：putfield/putstatic）。
///
/// @details
/// 把 SSAFieldRef 收集到 VmProgram.storeFieldRefs，字节流只存 fieldSiteIdx。
/// backend 为每项发射一个 helper（懒缓存 fieldID + Set[Static]<Type>Field）。
///
/// 字节流：[StoreField][fieldSiteIdx:u16][recv:u16][dest:u16=0][argc:u8=1][valueIdx:u16]
/// - recv：receiver 寄存器（static 时填 0）
/// - dest 恒为 0：store 无结果（helper 不写 dest）
/// - argc=1，argIdx[0]=valueIdx：要存的值（helper 经 argIdx 取值）
bool encodeStoreField(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    VmFieldRef fref;
    fref.owner    = ins.field.owner.to_std_string();
    fref.name     = ins.field.name.to_std_string();
    fref.desc     = ins.field.desc.to_std_string();
    fref.isStatic = ins.field.isStatic;
    fref.jniKind  = toVmJniKind(ins.value.type);
    const uint16_t siteIdx =
        ctx.internFieldRef(ctx.storeFieldRefs, ctx.storeFieldByKey, std::move(fref));

    // instance 字段 receiver；static 字段无 receiver。
    uint16_t recv = 0;
    if (!ins.field.isStatic) {
        if (!ctx.resolveOperand(ins.receiver, recv, reason)) return false;
    }
    // 要存的值经 argIdx 传给 helper（argc=1）。
    uint16_t valueIdx;
    if (!ctx.resolveOperand(ins.value, valueIdx, reason)) return false;

    emitOpcode(ctx, VmOpcode::StoreField, prog.bytecodes);
    emitU16(prog.bytecodes, siteIdx);
    emitU16(prog.bytecodes, recv);
    emitU16(prog.bytecodes, 0);   // dest unused（store 无结果）
    emitU8(prog.bytecodes, 1);    // argc = 1（value）
    emitU16(prog.bytecodes, valueIdx);
    return true;
}

/// @brief 编码 ArrayLoad 指令（Phase 2.A：iaload..aaload）。
///
/// @details
/// 按 elementType 收集到 VmProgram.arrayLoadRefs，字节流只存 siteIdx。
/// RegIR 中 array 存于 ins.receiver，index 存于 ins.index。
///
/// 字节流：[ArrayLoad][siteIdx:u16][arrayIdx:u16][dest:u16][argc:u8=1][indexIdx:u16]
/// - arrayIdx：数组寄存器（helper 的 recv）
/// - dest：结果寄存器（元素值写入处）
/// - argc=1，argIdx[0]=indexIdx：索引（helper 经 argIdx 取）
bool encodeArrayLoad(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    const VmArrayElemKind elemKind = toVmArrayElemKind(ins.elementType);
    const uint16_t siteIdx = ctx.internArrayRef(ctx.arrayLoadRefs, ctx.arrayLoadByKey, elemKind);

    // RegIR：array 在 ins.receiver，index 在 ins.index。
    uint16_t arrayIdx;
    if (!ctx.resolveOperand(ins.receiver, arrayIdx, reason)) return false;
    uint16_t indexIdx;
    if (!ctx.resolveOperand(ins.index, indexIdx, reason)) return false;
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    emitOpcode(ctx, VmOpcode::ArrayLoad, prog.bytecodes);
    emitU16(prog.bytecodes, siteIdx);
    emitU16(prog.bytecodes, arrayIdx);
    emitU16(prog.bytecodes, dest);
    emitU8(prog.bytecodes, 1);    // argc = 1（index）
    emitU16(prog.bytecodes, indexIdx);
    return true;
}

/// @brief 编码 ArrayStore 指令（Phase 2.A：iastore..aastore）。
///
/// @details
/// 字节流：[ArrayStore][siteIdx:u16][arrayIdx:u16][dest:u16=0][argc:u8=2][indexIdx:u16][valueIdx:u16]
/// - argc=2，argIdx[0]=indexIdx，argIdx[1]=valueIdx。
bool encodeArrayStore(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    const VmArrayElemKind elemKind = toVmArrayElemKind(ins.elementType);
    const uint16_t siteIdx = ctx.internArrayRef(ctx.arrayStoreRefs, ctx.arrayStoreByKey, elemKind);

    uint16_t arrayIdx;
    if (!ctx.resolveOperand(ins.receiver, arrayIdx, reason)) return false;
    uint16_t indexIdx;
    if (!ctx.resolveOperand(ins.index, indexIdx, reason)) return false;
    uint16_t valueIdx;
    if (!ctx.resolveOperand(ins.value, valueIdx, reason)) return false;

    emitOpcode(ctx, VmOpcode::ArrayStore, prog.bytecodes);
    emitU16(prog.bytecodes, siteIdx);
    emitU16(prog.bytecodes, arrayIdx);
    emitU16(prog.bytecodes, 0);   // dest unused（store 无结果）
    emitU8(prog.bytecodes, 2);    // argc = 2（index + value）
    emitU16(prog.bytecodes, indexIdx);
    emitU16(prog.bytecodes, valueIdx);
    return true;
}

/// @brief 编码 Allocate 指令（Phase 2.A：NEW/NEWARRAY/ANEWARRAY）。
///
/// @details
/// Object → AllocObject（<init> 由独立 Call 调用，已在 Phase 2.0 支持）；
/// Array → New<Type>Array / NewObjectArray（length 在 args[0]）；
/// MultiArray 暂 fail-closed（递归分配复杂，后续补）。
///
/// 字节流：[Allocate][siteIdx:u16][recv:u16=0][dest:u16][argc:u8][dims...]
/// - Object：argc=0
/// - Array：argc=1，argIdx[0]=lengthIdx
bool encodeAllocate(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog, std::string& reason)
{
    const VmAllocKind kind = toVmAllocKind(ins.allocationKind);
    VmAllocRef aref;
    aref.kind     = kind;
    aref.typeDesc = ins.typeDesc.to_std_string();
    const uint16_t siteIdx = ctx.internAllocRef(std::move(aref));

    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);

    // 维度寄存器（MultiArray：每维一个 int；Array：length；Object：无 args）。
    // MultiArray 的 dimCount 编码在 argc，解释器据此递归分配（Phase 2.E）。
    // 解释器 object-op handler 固定 argBuf[32]、多维路径 _dims[32]——JVM 合法
    // 上限 255 维，超 32 必须 fail-closed（对齐 encodeCall/encodeIntrinsic）。
    if (ins.args.size() > 32) {
        reason = "Allocate: too many dimensions (>32)";
        return false;
    }
    std::vector<uint16_t> dimIdxs;
    for (const auto& a : ins.args) {
        uint16_t di;
        if (!ctx.resolveOperand(a, di, reason)) return false;
        dimIdxs.push_back(di);
    }

    emitOpcode(ctx, VmOpcode::Allocate, prog.bytecodes);
    emitU16(prog.bytecodes, siteIdx);
    emitU16(prog.bytecodes, 0);   // recv unused（分配无 receiver）
    emitU16(prog.bytecodes, dest);
    emitU8(prog.bytecodes, static_cast<uint8_t>(dimIdxs.size()));
    for (uint16_t di : dimIdxs) emitU16(prog.bytecodes, di);
    return true;
}

/// @brief 编码 Intrinsic 指令（Phase 2.D 泛化）。
///
/// @details
/// SSA 产生的 intrinsic 名称（ssa_builder lower*）：
///   - array.length：GetArrayLength（null → NPE）
///   - cmp.nan_greater/nan_less：FCMPG/FCMPL/LCMP 三值比较（-1/0/1），纯计算
///     （浮点 NaN-aware：nan_greater 取 1，nan_less 取 -1）
///   - instanceof:<内部名>：IsInstanceOf（null → false）
///   - monitor.enter/exit：MonitorEnter/MonitorExit（null → NPE）
/// 其余（indy:<idx> 等）保持 fail-closed。所有 intrinsic 进 intrinsics 表
/// （按 name 去重），字节流只存 siteIdx。
///
/// 字节流：[Intrinsic][siteIdx:u16][recv:u16=0][dest:u16][argc:u8][args...]
/// - args 是操作数寄存器列表（array.length 的数组 / cmp 的双操作数 / instanceof
///   与 monitor 的引用）
bool encodeIntrinsic(EncoderState& ctx, const RegInstruction& ins, VmProgram& prog,
                     std::string& reason)
{
    VmIntrinsicRef iref;
    iref.name = ins.intrinsicName;

    if (ins.intrinsicName == "array.length") {
        iref.arg0Kind = VmJniKind::Ref;
        iref.retKind  = VmJniKind::Int32;
        iref.hasArgs  = !ins.args.empty();
    } else if (ins.intrinsicName == "cmp.nan_greater" ||
               ins.intrinsicName == "cmp.nan_less") {
        if (ins.args.size() < 2) {
            reason = "cmp.nan_* intrinsic requires two operands";
            return false;
        }
        iref.arg0Kind = toVmJniKind(ins.args[0].type);   // Float/Double/Int64
        iref.retKind  = VmJniKind::Int32;
        iref.hasArgs  = true;
    } else if (ins.intrinsicName.rfind("instanceof:", 0) == 0) {
        if (ins.args.empty()) {
            reason = "instanceof intrinsic missing operand";
            return false;
        }
        iref.arg0Kind = VmJniKind::Ref;
        iref.retKind  = VmJniKind::Int32;
        iref.hasArgs  = true;
    } else if (ins.intrinsicName.rfind("castcheck:", 0) == 0) {
        // CHECKCAST 语义（encodeConvert 重定向而来）：IsInstanceOf + 失败抛
        // ClassCastException（null 通过）；结果 REF 原样传递。
        if (ins.args.empty()) {
            reason = "castcheck intrinsic missing operand";
            return false;
        }
        iref.arg0Kind = VmJniKind::Ref;
        iref.retKind  = VmJniKind::Ref;
        iref.hasArgs  = true;
    } else if (ins.intrinsicName == "monitor.enter" || ins.intrinsicName == "monitor.exit") {
        if (ins.args.empty()) {
            reason = "monitor intrinsic missing operand";
            return false;
        }
        iref.arg0Kind = VmJniKind::Ref;
        iref.retKind  = VmJniKind::Void;
        iref.hasArgs  = true;
    } else {
        reason = "Intrinsic not supported yet: " + ins.intrinsicName;
        return false;
    }

    const uint16_t siteIdx = ctx.internIntrinsicRef(std::move(iref));

    // 操作数寄存器列表（变长）。argc 上限 8（intrinsic 操作数少，防御上限）。
    if (ins.args.size() > 8) {
        reason = "Intrinsic: too many arguments (>8)";
        return false;
    }
    std::vector<uint16_t> argIdxs;
    argIdxs.reserve(ins.args.size());
    for (const auto& a : ins.args) {
        uint16_t ai;
        if (!ctx.resolveOperand(a, ai, reason)) return false;
        argIdxs.push_back(ai);
    }
    const uint16_t dest = static_cast<uint16_t>(ins.dest.id.value);
    ctx.observeReg(dest);
    if (ins.intrinsicName == "array.length") ctx.hasArrayLength = true;

    emitOpcode(ctx, VmOpcode::Intrinsic, prog.bytecodes);
    emitU16(prog.bytecodes, siteIdx);
    emitU16(prog.bytecodes, 0);   // recv unused（操作数全在 args）
    emitU16(prog.bytecodes, dest);
    emitU8(prog.bytecodes, static_cast<uint8_t>(argIdxs.size()));
    for (uint16_t ai : argIdxs) emitU16(prog.bytecodes, ai);
    return true;
}

bool encodeTerminator(EncoderState& ctx, const RegBlock& block, VmProgram& prog, std::string& reason)
{
    // std::variant 访问：按 terminator 类型分派。
    return std::visit(
        [&](const auto& term) -> bool {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                return encodeJump(ctx, &block, term, prog, reason);
            } else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                return encodeBranch(ctx, &block, term, prog, reason);
            } else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                return encodeReturn(ctx, term, prog, reason);
            } else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                return encodeSwitch(ctx, &block, term, prog, reason);
            } else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                return encodeThrow(ctx, term, prog, reason);
            } else if constexpr (std::is_same_v<T, RegUnreachableTerminator>) {
                // Unreachable：发射 Halt 作为防御性停机（不应被执行到）。
                emitHaltOpcode(ctx, prog.bytecodes);
                (void)term;
                return true;
            }
            reason = "unknown terminator kind";
            return false;
        },
        block.terminator);
}

bool encodeJump(EncoderState& ctx, const RegBlock* srcBlock, const RegJumpTerminator& term,
                VmProgram& prog, std::string& reason)
{
    const auto it = ctx.blockIndex.find(term.target.block);
    if (it == ctx.blockIndex.end()) {
        reason = "Jump target block not indexed";
        return false;
    }
    // 边拷贝（块参数）走合成块：resolveEdgeTarget 优先返回合成块索引。
    const uint16_t target =
        ctx.resolveEdgeTarget(srcBlock, term.target.block, 0, it->second);
    emitOpcode(ctx, VmOpcode::Jump, prog.bytecodes);
    emitU16(prog.bytecodes, target);
    return true;
}

/// @brief 编码 THROW（athrow，Phase 2.C）。
///
/// @details
/// 字节流：[Throw][src:u16]——src 是异常对象寄存器（.ref 承载 jthrowable）。
/// 解释器 handler 调 mj2x_vm_throw（null → NPE），随后主循环的异常检查点
/// 按当前块活跃区域匹配/传播。无异常区域时 throw 直接传播到 Java。
bool encodeThrow(EncoderState& ctx, const RegThrowTerminator& term, VmProgram& prog,
                 std::string& reason)
{
    uint16_t src;
    if (!ctx.resolveOperand(term.value, src, reason)) return false;
    emitOpcode(ctx, VmOpcode::Throw, prog.bytecodes);
    emitU16(prog.bytecodes, src);
    prog.hasThrow = true;
    return true;
}

/// @brief 编码异常区域（Phase 2.C）：RegFunction.exceptionRegions → VmProgram 区域表。
///
/// @details
/// 每个 (region, 保护块) 生成一条 VmExceptionRegionEntry：handler 块索引、catch 类型
/// （空 = catch-all/finally，索引 0xFFFF）、exceptionValue 寄存器、保护块索引、边拷贝
/// （handler 参数寄存器 ← 来源）。blockExcRegions[blockIdx] 按异常表顺序（= region
/// 收集顺序）给出活跃条目，解释器异常分派据此匹配。边拷贝的源经 resolveOperand
/// 解析（字面量 intern 常量槽，prologue 预加载），目标/异常寄存器 observeReg 确保
/// 寄存器文件覆盖。
void encodeExceptionRegions(EncoderState& ctx, VmProgram& prog, std::string& reason)
{
    if (ctx.fn.exceptionRegions.empty()) return;
    const uint16_t totalBlocks =
        static_cast<uint16_t>(ctx.fn.blocks.size() + ctx.syntheticEdges.size());
    prog.blockExcRegions.assign(totalBlocks, {});
    for (const auto& region : ctx.fn.exceptionRegions) {
        if (region.handler == nullptr) {
            reason = "exception region missing handler block";
            return;
        }
        const auto handlerIt = ctx.blockIndex.find(region.handler);
        if (handlerIt == ctx.blockIndex.end()) {
            reason = "exception region handler block not indexed";
            return;
        }
        // catch 类型去重（空串 = catch-all/finally，用 0xFFFF 标记，不进表）。
        uint16_t caughtTypeIdx = 0xFFFF;
        if (!region.caughtType.empty()) {
            const auto it = std::find(prog.catchTypes.begin(), prog.catchTypes.end(),
                                      region.caughtType);
            if (it != prog.catchTypes.end()) {
                caughtTypeIdx = static_cast<uint16_t>(it - prog.catchTypes.begin());
            } else {
                caughtTypeIdx = static_cast<uint16_t>(prog.catchTypes.size());
                prog.catchTypes.push_back(region.caughtType);
            }
        }
        // 该 region 各保护块的边拷贝（edges 按 protectedBlock 对齐）。
        for (auto* protectedBlock : region.protectedBlocks) {
            if (protectedBlock == nullptr) continue;
            const auto blockIt = ctx.blockIndex.find(protectedBlock);
            if (blockIt == ctx.blockIndex.end()) {
                reason = "exception region protected block not indexed";
                return;
            }
            VmExceptionRegionEntry entry;
            entry.handlerBlock  = handlerIt->second;
            entry.caughtTypeIdx = caughtTypeIdx;
            entry.exceptionReg  = static_cast<uint16_t>(region.exceptionValue.id.value);
            entry.blockIdx      = blockIt->second;
            ctx.observeReg(entry.exceptionReg);
            // 该块的边拷贝（edges 中 protectedBlock 匹配的项）。
            for (const auto& edge : region.edges) {
                if (edge.protectedBlock != protectedBlock) continue;
                for (const auto& copy : edge.copies) {
                    uint16_t src;
                    // 异常边拷贝的 undef 源（handler 参数在该帧未定义）容忍为 NULL。
                    if (!ctx.resolveOperand(copy.source, src, reason, true)) return;
                    const uint16_t dest = static_cast<uint16_t>(copy.dest.id.value);
                    ctx.observeReg(dest);
                    entry.copies.emplace_back(dest, src);
                }
                break;
            }
            const uint16_t entryIdx = static_cast<uint16_t>(prog.excRegions.size());
            prog.excRegions.push_back(std::move(entry));
            prog.blockExcRegions[entry.blockIdx].push_back(entryIdx);
        }
    }
}

bool encodeBranch(EncoderState& ctx, const RegBlock* srcBlock, const RegBranchTerminator& term,
                  VmProgram& prog, std::string& reason)
{
    uint16_t cond;
    if (!ctx.resolveOperand(term.condition, cond, reason)) return false;
    const auto trueIt = ctx.blockIndex.find(term.trueTarget.block);
    const auto falseIt = ctx.blockIndex.find(term.falseTarget.block);
    if (trueIt == ctx.blockIndex.end() || falseIt == ctx.blockIndex.end()) {
        reason = "Branch target block not indexed";
        return false;
    }
    const uint16_t trueIdx  = ctx.resolveEdgeTarget(srcBlock, term.trueTarget.block, 0, trueIt->second);
    const uint16_t falseIdx = ctx.resolveEdgeTarget(srcBlock, term.falseTarget.block, 1, falseIt->second);
    emitOpcode(ctx, VmOpcode::Branch, prog.bytecodes);
    emitU16(prog.bytecodes, cond);
    emitU16(prog.bytecodes, trueIdx);
    emitU16(prog.bytecodes, falseIdx);
    return true;
}

bool encodeReturn(EncoderState& ctx, const RegReturnTerminator& term, VmProgram& prog,
                  std::string& reason)
{
    if (!term.hasValue) {
        emitOpcode(ctx, VmOpcode::Return, prog.bytecodes);
        emitU8(prog.bytecodes, 0);   // has_value = 0
        emitU16(prog.bytecodes, 0);  // src unused
        return true;
    }
    uint16_t src;
    if (!ctx.resolveOperand(term.value, src, reason)) return false;
    emitOpcode(ctx, VmOpcode::Return, prog.bytecodes);
    emitU8(prog.bytecodes, 1);   // has_value = 1
    emitU16(prog.bytecodes, src);
    return true;
}

/// @brief 编码 SWITCH（tableswitch/lookupswitch 统一形态，RegIR 已平坦化）。
///
/// @details
/// 字节流：[Switch][value:u16][default:u16][count:u16] + count ×
/// [key:i32][target:u16]。key 按 I32 编码（字节码 switch 只支持 int；
/// RegSwitchCase.key 是 int64 但来自字节码 int，高位必然为零）。case 与
/// default 目标都经 resolveEdgeTarget（边拷贝走合成块，与 Jump/Branch 同）。
/// 解释器线性扫描匹配（case 数少；二分/跳表为后续优化）。
bool encodeSwitch(EncoderState& ctx, const RegBlock* srcBlock, const RegSwitchTerminator& term,
                  VmProgram& prog, std::string& reason)
{
    uint16_t value;
    if (!ctx.resolveOperand(term.value, value, reason)) return false;
    if (term.cases.size() > 0xFFFF) {
        reason = "Switch: too many cases (>65535)";
        return false;
    }
    // 目标 slot 约定与 planSyntheticEdges 一致：case 0..n-1，default=n
    // （同一 dst 的多条边靠不同 slot 区分合成块，各自执行自己的边拷贝）。
    // 字节流布局：[Switch][value:u16][default:u16][count:u16] +
    // count × [key:i32][target:u16]（解释器 handler 按此顺序解码）。
    const auto defIt = ctx.blockIndex.find(term.defaultTarget.block);
    if (defIt == ctx.blockIndex.end()) {
        reason = "Switch default target block not indexed";
        return false;
    }
    const uint16_t defTarget = ctx.resolveEdgeTarget(
        srcBlock, term.defaultTarget.block, static_cast<uint16_t>(term.cases.size()), defIt->second);
    emitOpcode(ctx, VmOpcode::Switch, prog.bytecodes);
    emitU16(prog.bytecodes, value);
    emitU16(prog.bytecodes, defTarget);
    emitU16(prog.bytecodes, static_cast<uint16_t>(term.cases.size()));
    for (size_t i = 0; i < term.cases.size(); ++i) {
        const auto& c  = term.cases[i];
        const auto  it = ctx.blockIndex.find(c.target.block);
        if (it == ctx.blockIndex.end()) {
            reason = "Switch case target block not indexed";
            return false;
        }
        const uint16_t t =
            ctx.resolveEdgeTarget(srcBlock, c.target.block, static_cast<uint16_t>(i), it->second);
        emitI32(prog.bytecodes, static_cast<int32_t>(c.key));
        emitU16(prog.bytecodes, t);
    }
    return true;
}

}   // namespace

// ===========================================================================
// VmEncoder 公开入口
// ===========================================================================
std::unique_ptr<VmProgram> VmEncoder::encode(const RegFunction& fn, const VmInstance& instance,
                                             std::string& reason,
                                             const std::vector<UserIntrinsic>* userIntrinsics)
{
    auto prog = std::make_unique<VmProgram>();
    EncoderState ctx(fn, instance);
    ctx.userIntrinsics = userIntrinsics;

    if (!indexBlocks(ctx, reason)) return nullptr;
    ctx.planSyntheticEdges();
    // 异常区域（Phase 2.C）：先于 layoutBlocks 收集（边拷贝可能 intern 新常量槽，
    // 常量槽分配独立于字节流布局，顺序无影响；但必须保证区域表在 layoutBlocks
    // 之后仍有效——区域表只存块索引/寄存器索引，不依赖字节流偏移）。
    encodeExceptionRegions(ctx, *prog, reason);
    if (!reason.empty()) return nullptr;
    if (!layoutBlocks(ctx, *prog, reason)) return nullptr;

    // 填充常量池：constSlotBySignature 存 (签名 → 槽索引)，签名含 type+intBits+floatValue。
    // 常量槽全部落在计算寄存器之后（EncoderState 构造期预扫描保证隔离），
    // 收集 (slotIndex, entry) 对后按 slot 排序，prologue 按 entry.destSlot 写入。
    std::vector<std::pair<uint16_t, std::pair<std::string, VmConstantEntry>>> sortedConsts;
    for (const auto& kv : ctx.constSlotBySignature) {
        const std::string& sig = kv.first;
        const uint16_t     slot = kv.second;
        VmConstantEntry    entry{};
        entry.type     = static_cast<VmType>(static_cast<uint8_t>(sig[0]));
        entry.destSlot = slot;
        std::memcpy(&entry.intBits, sig.data() + 1, sizeof(int64_t));
        std::memcpy(&entry.floatValue, sig.data() + 1 + sizeof(int64_t), sizeof(double));
        sortedConsts.emplace_back(slot, std::make_pair(sig, entry));
    }
    std::sort(sortedConsts.begin(), sortedConsts.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    prog->constantPool.reserve(sortedConsts.size());
    for (auto& sc : sortedConsts) {
        prog->constantPool.push_back(sc.second.second);
    }

    // Call 调用点的方法引用表（Phase 2.0）：直接搬移 EncoderState 收集的结果。
    prog->methodRefs = std::move(ctx.methodRefs);

    // LoadField/StoreField 字段引用表（Phase 2.A）。
    prog->loadFieldRefs  = std::move(ctx.loadFieldRefs);
    prog->storeFieldRefs = std::move(ctx.storeFieldRefs);
    // ArrayLoad/ArrayStore 元素类型表（Phase 2.A）。
    prog->arrayLoadRefs  = std::move(ctx.arrayLoadRefs);
    prog->arrayStoreRefs = std::move(ctx.arrayStoreRefs);
    // Allocate 分配表 + array.length 标记（Phase 2.A）。
    prog->allocRefs      = std::move(ctx.allocRefs);
    prog->hasArrayLength = ctx.hasArrayLength;
    // Intrinsic 表（Phase 2.D 泛化）。
    prog->intrinsics = std::move(ctx.intrinsicRefs);
    // 指令边界表（issue #760 切片 2）：emitOpcode/emitHaltOpcode 按发射顺序
    // 记录，与 bytecodes 布局天然同步；懒解密分块据此取指令对齐的块边界。
    prog->instructionOffsets = std::move(ctx.instructionOffsets);
    // String LDC 常量槽（Phase 2.E）。
    prog->stringLiterals = std::move(ctx.stringLiterals);
    prog->classLiterals  = std::move(ctx.classLiterals);

    prog->numInputs = ctx.numInputs;
    prog->numRegs   = ctx.maxReg;
    return prog;
}

}   // namespace mj2x
