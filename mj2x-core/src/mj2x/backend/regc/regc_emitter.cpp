#include "regc_emitter_internal.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/codegen/c_expr.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/runtime_protection.hpp"
#include "libca/str/format.hpp"
#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"
#include "mj2x/core/compiler_index.hpp"
#include "mj2x/mir/register_ir/reg_dump.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;

namespace {

// cStringLiteral is shared with StackC via codegen::cStringLiteral; alias it
// here so the existing call sites stay one-line.
using codegen::cStringLiteral;
// 源码拼接统一走 CSourceStream（Utf8 家族三层模型的排版层）：局部片段构建器与
// emit 路径的 sink 都用它，其 append 重载覆盖 Utf8String/Utf8StringRef/字面量/
// string_view，消除了 Utf8StringBuilder 无法直接接 std::string 的缺口。
using codegen::CSourceStream;

/// @brief 纯计算 intrinsic 判定（issue #778 P2 异常检查稀疏化）。
///
/// RuntimeFunction emitKind 的 intrinsic（位操作/Math 等）在 emitIntrinsicCall
/// 兜底分支发射为纯 C 表达式调用，不经过 JNI、不可能抛异常；SSA 阶段无法区分
/// intrinsic 与普通调用，统一标 mayThrow，导致热循环每轮都做 ExceptionCheck。
/// 这里在发射异常分发前复判一次：匹配成功且为 RuntimeFunction 时省略检查
/// （与 emitIntrinsicCall 的发射行为一致——该分支无 native-state 回退）。
bool isPureRuntimeIntrinsic(const RegInstruction& ins)
{
    if (ins.kind != RegInstructionKind::Call) return false;
    const RegCIntrinsicDecision decision = RegCIntrinsicRegistry::builtins().matchCall(ins);
    return decision.kind == RegCIntrinsicDecisionKind::Emit &&
           decision.emitKind == RegCIntrinsicEmitKind::RuntimeFunction;
}

/// Object.getClass() intrinsic（发射为 JNI GetObjectClass）本身不抛 Java 异常，
/// NULL receiver 的 NPE 已在发射点显式处理。无异常区域的方法可省略调用后
/// ExceptionCheck（有异常区域时保守保留，避免 NPE 挂起后错过 catch 分发）。
bool isObjectGetClass(const RegInstruction& ins)
{
    if (ins.kind != RegInstructionKind::Call) return false;
    const RegCIntrinsicDecision decision = RegCIntrinsicRegistry::builtins().matchCall(ins);
    return decision.kind == RegCIntrinsicDecisionKind::Emit &&
           decision.emitKind == RegCIntrinsicEmitKind::ObjectGetClass;
}

/// IR 级"方法体是否产 JNI local ref"判定：PushLocalFrame 帧发射的补充判定
/// （文本级 bodyProducesLocalRefs 不识别 resolved 调用产生的 GetObjectClass
/// ref）与 resolved 调用省略 DeleteLocalRef 的门槛。规则：
/// - Call（非纯计算 intrinsic）：返回 ref 或经物化/helper 产 ref → true
/// - Allocate：true
/// - LoadField/StoreField 的 desc 以 'L'/'[' 开头（引用字段）：true
/// - ArrayLoad/ArrayStore 的 elementType 为 REF：true
/// - Intrinsic 指令（array.length/monitor/cmp.nan）纯计算不产 ref；string
///   literal 物化走 LDC 表达式路径（tag 前缀），不在指令层面 → false
bool fnProducesLocalRefsIR(const RegFunction& fn)
{
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            switch (ins.kind) {
                case RegInstructionKind::Call:
                    if (!isPureRuntimeIntrinsic(ins)) return true;
                    break;
                case RegInstructionKind::Allocate: return true;
                case RegInstructionKind::LoadField:
                case RegInstructionKind::StoreField: {
                    const char first =
                        ins.field.desc.is_empty() ? '\0' : (char)ins.field.desc.data()[0];
                    if (first == 'L' || first == '[') return true;
                    break;
                }
                case RegInstructionKind::ArrayLoad:
                case RegInstructionKind::ArrayStore:
                    if (ins.elementType == SSAType::REF) return true;
                    break;
                default: break;
            }
        }
    }
    return false;
}

/// 统计方法体产 JNI local ref 的指令数（与 fnProducesLocalRefsIR 同一分类，
/// 计数而非布尔）。无环 CFG 中每条指令至多执行一次，计数即单次调用的 local
/// ref 消耗上界（保守：Call 一律计 1，含稳态零 ref 的 resolved/intrinsic 调用）。
size_t fnLocalRefCountIR(const RegFunction& fn)
{
    size_t count = 0;
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            switch (ins.kind) {
                case RegInstructionKind::Call:
                    if (!isPureRuntimeIntrinsic(ins)) ++count;
                    break;
                case RegInstructionKind::Allocate: ++count; break;
                case RegInstructionKind::LoadField:
                case RegInstructionKind::StoreField: {
                    const char first =
                        ins.field.desc.is_empty() ? '\0' : (char)ins.field.desc.data()[0];
                    if (first == 'L' || first == '[') ++count;
                    break;
                }
                case RegInstructionKind::ArrayLoad:
                case RegInstructionKind::ArrayStore:
                    if (ins.elementType == SSAType::REF) ++count;
                    break;
                default: break;
            }
        }
    }
    return count;
}

/// CFG 是否含环（回边）。循环体内的产 ref 点随迭代累积，帧豁免只对无环函数
/// 安全。successor 从 terminator 提取，DFS 三色找回边。
bool fnCfgHasLoop(const RegFunction& fn)
{
    std::unordered_map<const RegBlock*, int> color;
    bool foundLoop = false;
    std::function<void(const RegBlock*)> visit = [&](const RegBlock* node) {
        if (node == nullptr || foundLoop) return;
        const int c = color.count(node) ? color[node] : 0;
        if (c == 1) {
            foundLoop = true;
            return;
        }
        if (c == 2) return;
        color[node] = 1;
        const auto visitSuccessor = [&](const RegSuccessor& succ) { visit(succ.block); };
        if (auto* jump = std::get_if<RegJumpTerminator>(&node->terminator)) {
            visitSuccessor(jump->target);
        }
        else if (auto* branch = std::get_if<RegBranchTerminator>(&node->terminator)) {
            visitSuccessor(branch->trueTarget);
            visitSuccessor(branch->falseTarget);
        }
        else if (auto* sw = std::get_if<RegSwitchTerminator>(&node->terminator)) {
            for (const auto& c2 : sw->cases) visitSuccessor(c2.target);
            visitSuccessor(sw->defaultTarget);
        }
        color[node] = 2;
    };
    if (fn.entry != nullptr) visit(fn.entry);
    return foundLoop;
}

}   // namespace

void emitRegCHelperPrelude(ca::str::Utf8StringBuilder& out,
                           const RegCIntrinsicRegistry* registry)
{
    emitHelperPrelude(out, registry);
}

RegCEmitter::RegCEmitter(RegCCachePool* cachePool, const codegen::CodeOptions& options,
                         std::vector<RegFunction::InvokeDynamicSite>* fileIndySites,
                         size_t indySiteBase, CompilerIndex* classIndex,
                         const std::unordered_map<std::string, std::string>* convertedFunctions,
                         const std::unordered_set<std::string>* indyFunctions,
                         const RegCIntrinsicRegistry* intrinsicRegistry,
                         const std::set<std::string>* enumSyntheticMethods)
    : cachePool_(cachePool)
    , options_(options)
    , fileIndySites_(fileIndySites)
    , indySiteBase_(indySiteBase)
    , classIndex_(classIndex)
    , convertedFunctions_(convertedFunctions)
    , indyFunctions_(indyFunctions)
    , intrinsicRegistry_(intrinsicRegistry)
    , enumSyntheticMethods_(enumSyntheticMethods)
{}

std::string RegCEmitter::functionNameFor(const SSAFunctionSymbol& symbol)
{
    return functionName(symbol);
}

RegCEmitResult RegCEmitter::emitFunction(const RegFunction& fn)
{
    RegCEmitResult result;
    result.functionName = functionName(fn.symbol);
    if (!fn.entry) {
        result.reason = "RegC requires an entry block";
        return result;
    }
    // --strings-encrypt：本函数发射期间挂上字面量加密登记池（RAII 恢复；
    // operandExpr 的 ldc_string 分支据此发射密文缓冲引用）。
    const RegCStringEncryptScope stringEncryptScope{
        (cachePool_ != nullptr && cachePool_->stringsEncryptEnabled()) ? cachePool_ : nullptr};
    if (fileIndySites_ != nullptr && !fn.indySites.empty()) {
        fileIndySites_->insert(fileIndySites_->end(), fn.indySites.begin(), fn.indySites.end());
    }

    std::string validationReason;
    if (!validateRegIRNoStackSlots(fn, validationReason)) {
        result.reason = validationReason;
        return result;
    }

    std::map<uint32_t, SSAType> regs;
    if (!collectRegisters(fn, regs, result.reason)) return result;
    const RegCStringBuilderPlan stringBuilderPlan = analyzeStringBuilders(fn, intrinsicRegistry_);
    const std::set<uint32_t>    nativeWrapperRegisters = analyzeNativeWrappers(fn);

    RegCFunctionEmitState emitState;
    emitState.functionName                       = result.functionName;
    emitState.symbol                             = fn.symbol;
    emitState.cachePool                          = cachePool_;
    emitState.commentLevel                       = options_.commentLevel;
    emitState.traceCalls                         = options_.traceCallLog;
    emitState.directCalls                        = options_.directCalls;
    emitState.classIndex                         = classIndex_;
    emitState.convertedFunctions                 = convertedFunctions_;
    emitState.indyFunctions                      = indyFunctions_;
    emitState.intrinsicRegistry                  = intrinsicRegistry_;
    emitState.enumSyntheticMethods               = enumSyntheticMethods_;
    emitState.resolvedDispatch                   = options_.resolvedDispatch;
    emitState.hasLocalFrame                      = fnProducesLocalRefsIR(fn);
    emitState.nativeStringBuilderRegisters       = stringBuilderPlan.nativeRegisters;
    emitState.nativeStringBuilderOwners          = stringBuilderPlan.ownerRegisters;
    emitState.nativeStringBuilderOwnerByRegister = stringBuilderPlan.ownerByRegister;
    emitState.nativeWrapperRegisters             = nativeWrapperRegisters;
    for (uint32_t id : stringBuilderPlan.nativeRegisters) {
        const auto kindIt = stringBuilderPlan.kindByRegister.find(id);
        if (kindIt == stringBuilderPlan.kindByRegister.end()) {
            result.reason = "mutable string plan is missing register kind";
            return result;
        }
        emitState.nativeObjects.emplace(id,
                                        RegCNativeObjectState{kindIt->second, regName(RegId{id})});
    }
    int                dispatchIndex = 0;
    int                edgeTempIndex = 0;
    CSourceStream body;
    body.indent();
    for (const auto& block : fn.blocks) {
        auto activeRegions = activeExceptionRegions(fn, block.get());
        if (options_.commentLevel >= RegCCommentLevel::Summary) {
            std::string header = ca::str::format_std("block_b{}: ", block->id.value);
            if (block.get() == fn.entry) {
                header += "entry";
            }
            else {
                const RegExceptionRegion* handlerRegion = nullptr;
                for (const auto& region : fn.exceptionRegions) {
                    if (region.handler == block.get()) {
                        handlerRegion = &region;
                        break;
                    }
                }
                if (handlerRegion != nullptr) {
                    header += "exception handler of ";
                    header += codegen::escapeCommentIdent(
                        ca::str::Utf8StringRef::from_string_view(handlerRegion->caughtType))
                                  .to_std_string();
                }
                else {
                    header += "block";
                }
            }
            body.comment(header);
        }
        body.label_(ca::str::format_std("block_b{}", block->id.value));
        // Allocate + 后续 <init> 融合为 NewObjectA：Allocate 处前瞻扫描同块内
        // 首个 receiver 匹配的 <init>（中间只允许纯计算指令且不得使用分配结果），
        // 分配结果延迟到 <init> 位置一次发射（NewObjectA 1 JNI 替代 AllocObject +
        // <init> 解析，JDK 类构造场景省 1-2 JNI/对象）。
        struct PendingAllocInit
        {
            bool                    active = false;
            const RegInstruction*   alloc  = nullptr;
            const RegInstruction*   initCall = nullptr;
            ca::str::Utf8String     allocCacheName;
            std::vector<ca::str::Utf8String> fusedArgs;
        } pendingFuse;
        /// getClass().getName().hashCode() 融合窗口：仅 getName/hashCode 两条
        /// 指令被融合结果取代（融合值在 getClass 位置一次产出），窗口内其余
        /// 指令（isFuseTransparent 放行的字段/数组读写等）必须原位发射——
        /// 副作用与中间 dest 的后续使用不可丢。
        const RegInstruction* fusedNameHashSkip1 = nullptr;
        const RegInstruction* fusedNameHashEnd   = nullptr;
        const auto operandUsesReg = [](const RegOperand& op, RegId id) {
            return op.kind == RegOperandKind::Register && op.reg.value == id.value;
        };
        const auto instructionUsesReg = [&](const RegInstruction& ins, RegId id) {
            if (operandUsesReg(ins.copy.source, id)) return true;
            if (operandUsesReg(ins.value, id)) return true;
            if (operandUsesReg(ins.lhs, id)) return true;
            if (operandUsesReg(ins.rhs, id)) return true;
            if (operandUsesReg(ins.receiver, id)) return true;
            if (operandUsesReg(ins.index, id)) return true;
            if (ins.hasDest && ins.dest.id.value == id.value) return true;
            for (const auto& a : ins.args) {
                if (operandUsesReg(a, id)) return true;
            }
            return false;
        };
        /// 值是否被 terminator（返回值/条件/switch 值/边拷贝）使用。
        const auto terminatorUsesReg = [&](const RegTerminator& term, RegId id) {
            const auto successorUses = [&](const RegSuccessor& succ) {
                for (const auto& copy : succ.copies) {
                    if (operandUsesReg(copy.source, id)) return true;
                }
                return false;
            };
            if (auto* jump = std::get_if<RegJumpTerminator>(&term)) {
                return successorUses(jump->target);
            }
            if (auto* branch = std::get_if<RegBranchTerminator>(&term)) {
                return operandUsesReg(branch->condition, id) ||
                       successorUses(branch->trueTarget) || successorUses(branch->falseTarget);
            }
            if (auto* sw = std::get_if<RegSwitchTerminator>(&term)) {
                if (operandUsesReg(sw->value, id)) return true;
                for (const auto& c : sw->cases) {
                    if (successorUses(c.target)) return true;
                }
                return successorUses(sw->defaultTarget);
            }
            if (auto* ret = std::get_if<RegReturnTerminator>(&term)) {
                return ret->hasValue && operandUsesReg(ret->value, id);
            }
            if (auto* thr = std::get_if<RegThrowTerminator>(&term)) {
                return operandUsesReg(thr->value, id);
            }
            return false;
        };
        /// 值是否被本块异常边拷贝使用（live-out 到 handler）。
        const auto exceptionEdgesUseReg = [&](RegId id) {
            for (const auto& region : fn.exceptionRegions) {
                for (const auto& edge : region.edges) {
                    if (edge.protectedBlock != block.get()) continue;
                    for (const auto& copy : edge.copies) {
                        if (operandUsesReg(copy.source, id)) return true;
                    }
                }
            }
            return false;
        };
        const auto isFuseTransparent = [](const RegInstruction& ins) {
            switch (ins.kind) {
            case RegInstructionKind::Copy:
            case RegInstructionKind::Unary:
            case RegInstructionKind::Binary:
            case RegInstructionKind::Compare:
            case RegInstructionKind::Convert:
            // 字段/数组读写与 intrinsic 也允许：它们在 NewObjectA 前原位执行
            // （顺序保持，异常语义等价——分配对象未初始化不可见，异常时丢弃），
            // 只要不使用分配结果（instructionUsesReg 已检查）。典型收益：
            // new String(bytes, UTF_8)（alloc → getstatic Charset → <init>）从
            // AllocObject+解析 4 JNI 降为 NewObjectA 2 JNI。
            case RegInstructionKind::LoadField:
            case RegInstructionKind::StoreField:
            case RegInstructionKind::ArrayLoad:
            case RegInstructionKind::ArrayStore:
            case RegInstructionKind::Intrinsic:
                return true;
            default:
                return false;
            }
        };
        const auto emitInstructionWithCheck = [&](const RegInstruction& ins) -> bool {
            if (options_.commentLevel >= RegCCommentLevel::Detailed) {
                body.comment(regInstructionLine(ins).to_std_string());
            }
            if (!emitInstruction(body, ins, emitState, fn, block.get(), indySiteBase_,
                                 result.reason))
                return false;
            // 无异常区域（方法无 try/catch）时省略调用后 ExceptionCheck：pending
            // 异常直接泄漏到 JNI 边界由 JVM 处理（与 Java 语义及 os30 一致），省
            // 每次调用的 check 开销；有异常区域时保留（分发到 handler 必需）。
            if (ins.effect.mayThrow && !isPureRuntimeIntrinsic(ins) &&
                !activeRegions.empty()) {
                if (!emitExceptionDispatch(body,
                                           activeRegions,
                                           block.get(),
                                           fn.symbol.returnType,
                                           emitState,
                                           dispatchIndex,
                                           edgeTempIndex,
                                           result.reason))
                    return false;
            }
            return true;
        };
        for (size_t insIdx = 0; insIdx < block->instructions.size(); ++insIdx) {
            const RegInstruction& ins = block->instructions[insIdx];
            if (fusedNameHashEnd != nullptr) {
                // 融合跳过窗口：仅 getName/hashCode 被融合结果取代；窗口内
                // 其余指令原位发射（副作用与中间 dest 后续使用保留；getName/
                // hashCode 提前到 getClass 位置求值——Class 对象身份不可变，
                // 中间指令不触碰 getClass/getName dest，语义等价）。
                if (fusedNameHashSkip1 != nullptr && &ins == fusedNameHashSkip1) {
                    fusedNameHashSkip1 = nullptr;
                    continue;
                }
                if (&ins == fusedNameHashEnd) {
                    fusedNameHashEnd = nullptr;
                    continue;
                }
                if (!emitInstructionWithCheck(ins)) return result;
                continue;
            }
            if (pendingFuse.active) {
                if (&ins == pendingFuse.initCall) {
                    // 到达 <init>：发射融合 NewObjectA（分配结果延迟赋值）。
                    if (options_.commentLevel >= RegCCommentLevel::Detailed) {
                        body.comment("// fused allocate+init: " +
                                     pendingFuse.initCall->method.owner.to_std_string() + "." +
                                     pendingFuse.initCall->method.name.to_std_string() +
                                     pendingFuse.initCall->method.desc.to_std_string());
                    }
                    emitAllocateWithInit(body, emitState, *pendingFuse.alloc, ins,
                                         pendingFuse.allocCacheName, pendingFuse.fusedArgs);
                    pendingFuse.active = false;
                    if (ins.effect.mayThrow && !isPureRuntimeIntrinsic(ins) &&
                        !activeRegions.empty()) {
                        if (!emitExceptionDispatch(body,
                                                   activeRegions,
                                                   block.get(),
                                                   fn.symbol.returnType,
                                                   emitState,
                                                   dispatchIndex,
                                                   edgeTempIndex,
                                                   result.reason))
                            return result;
                    }
                    continue;
                }
                if (!emitInstructionWithCheck(ins)) return result;
                continue;
            }
            // getClass().getName().hashCode() 融合：receiver 身份键控双槽缓存
            // （IsSameObject 命中），1 个 JNI 替代 GetObjectClass + getName +
            // hashCode 的 3 个。链上中间值不得有其它使用（含 terminator 边拷贝
            // 与异常边），否则回退逐跳发射。
            if (ins.kind == RegInstructionKind::Call &&
                ins.method.invokeOpcode == Opcodes::INVOKEVIRTUAL &&
                ins.method.owner == "java/lang/Object" &&
                ins.method.name == "getClass" && ins.method.desc == "()Ljava/lang/Class;" &&
                ins.hasDest && emitState.cachePool != nullptr) {
                const RegInstruction* getNameIns = nullptr;
                const RegInstruction* hashIns    = nullptr;
                size_t                scan       = insIdx + 1;
                while (scan < block->instructions.size()) {
                    const RegInstruction& cand = block->instructions[scan];
                    if (cand.kind == RegInstructionKind::Call) {
                        if (getNameIns == nullptr &&
                            cand.method.invokeOpcode == Opcodes::INVOKEVIRTUAL &&
                            cand.method.owner == "java/lang/Class" &&
                            cand.method.name == "getName" &&
                            cand.method.desc == "()Ljava/lang/String;" && cand.hasDest &&
                            !cand.args.empty() && operandUsesReg(cand.args[0], ins.dest.id)) {
                            getNameIns = &cand;
                            scan++;
                            continue;
                        }
                        if (getNameIns != nullptr && hashIns == nullptr &&
                            cand.method.invokeOpcode == Opcodes::INVOKEVIRTUAL &&
                            cand.method.owner == "java/lang/String" &&
                            cand.method.name == "hashCode" && cand.method.desc == "()I" &&
                            cand.hasDest && !cand.args.empty() &&
                            operandUsesReg(cand.args[0], getNameIns->dest.id)) {
                            hashIns = &cand;
                            scan++;
                            continue;
                        }
                        break;
                    }
                    if (instructionUsesReg(cand, ins.dest.id)) break;
                    if (getNameIns != nullptr &&
                        instructionUsesReg(cand, getNameIns->dest.id)) {
                        break;
                    }
                    if (!isFuseTransparent(cand)) break;
                    scan++;
                }
                bool chainEscapes = false;
                if (hashIns != nullptr) {
                    for (size_t k = scan; k < block->instructions.size(); ++k) {
                        if (instructionUsesReg(block->instructions[k], ins.dest.id) ||
                            instructionUsesReg(block->instructions[k], getNameIns->dest.id)) {
                            chainEscapes = true;
                            break;
                        }
                    }
                    if (!chainEscapes) {
                        chainEscapes = terminatorUsesReg(block->terminator, ins.dest.id) ||
                                       terminatorUsesReg(block->terminator, getNameIns->dest.id) ||
                                       exceptionEdgesUseReg(ins.dest.id) ||
                                       exceptionEdgesUseReg(getNameIns->dest.id);
                    }
                }
                if (hashIns != nullptr && !chainEscapes) {
                    const ca::str::Utf8String site =
                        emitState.cachePool->classNameHashSite();
                    body.appendLineStart(regName(hashIns->dest.id))
                        .append(" = ")
                        .append(codegen::exprCall("mj2x_intrinsic_class_name_hash",
                                                  {"_mj2x_regc_current_context()",
                                                   operandExpr(ins.args[0]).to_std_string(),
                                                   "&" + site.to_std_string()}))
                        .append(";")
                        .newLine();
                    fusedNameHashEnd   = hashIns;
                    fusedNameHashSkip1 = getNameIns;
                    // 融合调用的 miss 路径经 JDK getName/hashCode，保留与被取代
                    // 调用一致的异常分发。
                    if (hashIns->effect.mayThrow && !activeRegions.empty()) {
                        if (!emitExceptionDispatch(body,
                                                   activeRegions,
                                                   block.get(),
                                                   fn.symbol.returnType,
                                                   emitState,
                                                   dispatchIndex,
                                                   edgeTempIndex,
                                                   result.reason))
                            return result;
                    }
                    continue;
                }
                if (!emitInstructionWithCheck(ins)) return result;
                continue;
            }
            if (ins.kind == RegInstructionKind::Allocate &&
                ins.allocationKind == SSAAllocationKind::Object) {
                // 前瞻扫描同块内可融合 <init>。
                size_t scan = insIdx + 1;
                while (scan < block->instructions.size()) {
                    const RegInstruction& cand = block->instructions[scan];
                    if (cand.kind == RegInstructionKind::Call &&
                        cand.method.name.to_std_string() == "<init>" && !cand.args.empty() &&
                        operandUsesReg(cand.args[0], ins.dest.id)) {
                        if (canFuseAllocateInit(emitState, ins, cand)) {
                            pendingFuse.active         = true;
                            pendingFuse.alloc          = &ins;
                            pendingFuse.initCall       = &cand;
                            pendingFuse.allocCacheName = addClassCache(emitState, ins.typeDesc);
                            // 必须清空：pendingFuse 是块级复用对象，上一融合的
                            // 参数表达式会残留（曾致参数错位取到 receiver）。
                            pendingFuse.fusedArgs.clear();
                            pendingFuse.fusedArgs.reserve(cand.args.size());
                            for (const auto& arg : cand.args) {
                                pendingFuse.fusedArgs.push_back(operandExpr(arg));
                            }
                        }
                        break;
                    }
                    if (instructionUsesReg(cand, ins.dest.id) || !isFuseTransparent(cand)) break;
                    scan++;
                }
                if (!pendingFuse.active) {
                    if (!emitInstructionWithCheck(ins)) return result;
                }
                continue;
            }
            if (!emitInstructionWithCheck(ins)) return result;
        }
        if (options_.commentLevel >= RegCCommentLevel::Detailed) {
            const ca::str::Utf8String termLines = regTerminatorLines(*block);
            std::string_view          sv(termLines);
            // regTerminatorLines 每行以 '\n' 结尾；逐行加 "// " 前缀作为注释，避免裸 IR 文本进入 C 流。
            size_t pos = 0;
            while (pos < sv.size()) {
                size_t nl = sv.find('\n', pos);
                if (nl == std::string_view::npos) nl = sv.size();
                body.comment(sv.substr(pos, nl - pos));
                pos = nl + 1;
            }
        }
        if (!emitTerminator(body,
                            block->terminator,
                            block.get(),
                            fn.symbol.returnType,
                            activeRegions,
                            emitState,
                            dispatchIndex,
                            edgeTempIndex,
                            result.reason)) {
            return result;
        }
        body.emitBlankLine();
    }
    body.dedent();

    CSourceStream out;
    for (const auto& declaration : emitState.staticMethodCacheDeclarations) {
        out.append(declaration);
    }
    for (const auto& declaration : emitState.staticFieldCacheDeclarations) {
        out.append(declaration);
    }
    for (const auto& declaration : emitState.classCacheDeclarations) {
        out.append(declaration);
    }
    if (!emitState.staticMethodCacheDeclarations.empty() ||
        !emitState.staticFieldCacheDeclarations.empty() ||
        !emitState.classCacheDeclarations.empty())
        out.append("\n");

    if (options_.commentLevel >= RegCCommentLevel::Summary) {
        out.append("// RegC method: ");
        out.append(codegen::escapeCommentIdent(
            ca::str::Utf8StringRef::from_string_view(fn.symbol.owner)));
        out.append(".");
        out.append(codegen::escapeCommentIdent(
            ca::str::Utf8StringRef::from_string_view(fn.symbol.name)));
        out.append(codegen::escapeCommentIdent(
            ca::str::Utf8StringRef::from_string_view(fn.symbol.desc)));
        out.append("\n");
    }

    out.append("static ");
    out.append(cType(fn.symbol.returnType));
    out.append(" ");
    out.append(result.functionName);
    out.append("(");
    for (size_t i = 0; i < fn.parameters.size(); ++i) {
        if (i) out.append(", ");
        out.append(cType(fn.parameters[i].type));
        out.append(" ");
        {
            const std::string paramName = regName(fn.parameters[i].id);
            out.append(paramName.data(), paramName.size());
        }
    }
    out.append(") {\n");
    out.indent();

    if (fn.symbol.returnType != SSAType::VOID) {
        out.declare_var(cType(fn.symbol.returnType), "_mj2x_regc_result",
                        defaultValue(fn.symbol.returnType));
    }

    for (const auto& [id, type] : regs) {
        bool isParameter = false;
        for (const auto& param : fn.parameters) {
            if (param.id.value == id) {
                isParameter = true;
                break;
            }
        }
        if (!isParameter) {
            out.declare_var(cType(type), regName(RegId{id}), "0");
        }
    }
    // Declare edge-copy temporaries based on the types actually captured during emit
    // (emitState.edgeTempTypes). This mirrors the emit path exactly, including the
    // exception-dispatch re-emission that a static CFG walk (collectEdgeCopyTempTypes)
    // would undercount when a protected block has multiple mayThrow instructions.
    for (size_t i = 0; i < emitState.edgeTempTypes.size(); ++i) {
        out.declare_var(cType(emitState.edgeTempTypes[i]), ca::str::format_std("edge_tmp_{}", i),
                        "0");
    }
    for (uint32_t owner : emitState.nativeStringBuilderOwners) {
        const auto objectIt = emitState.nativeObjects.find(owner);
        if (objectIt == emitState.nativeObjects.end()) {
            result.reason = "mutable string owner is missing native object state";
            return result;
        }
        out.declare_var(std::string(nativeObjectType(objectIt->second.kind)) + "*",
                        stringBuilderOwnerName(RegId{owner}).ref(), "NULL");
        out.declare_var("mj2x_ref", stringBuilderMaterializedName(RegId{owner}).ref(), "NULL");
    }
    if (!regs.empty()) out.emitBlankLine();

    // R6 前置：JNI local ref 引用管理。RegC 每个 native 方法在单一 JNI 帧内运行，
    // 产 object 的 helper（call_ref / class_literal / alloc / ref-field load 等）返回的
    // local ref 全程累积不释放，反射密集方法会撑爆 JNI 默认保证的 16 槽 local ref 表。
    // 用 PushLocalFrame(入口) / PopLocalFrame(单出口，promote 返回值) 批量释放，语义等价
    // StackC 的 trackLocalRef/clear，但零 C 数组、无 c2j stack anchor。缓存 class/method
    // 走 NewGlobalRef，不受帧影响。纯标量方法（无产 ref 操作）不发射，保留「零 JNI」形态。
    const ca::str::Utf8String bodySource = body.build();
    const bool producesRefs = bodyProducesLocalRefs(bodySource) || emitState.hasLocalFrame;
    // 帧豁免：无环 CFG 且产 ref 指令 ≤ 6 的函数，单次调用至多 6 个 local ref
    // 落入调用者/trampoline 帧（HotSpot 局部 handle 按需扩展，出口统一释放），
    // 省去每次调用 PushLocalFrame + PopLocalFrame 两个 JNI。稳态（resolved/
    // intrinsic 缓存命中）下这类叶子函数产 ref 为 0，被热循环直调时每迭代
    // 省 2 JNI。循环内产 ref 点随迭代累积，不豁免。
    const bool frameEligible =
        producesRefs && !fnCfgHasLoop(fn) && fnLocalRefCountIR(fn) <= 6;
    const bool needsLocalFrame = producesRefs && !frameEligible;
    if (needsLocalFrame) {
        out.iff("(*_mj2x_regc_env)->PushLocalFrame(_mj2x_regc_env, 16) != 0",
                [&](CSourceStream& u) {
                    if (fn.symbol.returnType == SSAType::VOID) u.ret();
                    else u.ret("_mj2x_regc_result");
                });
    }

    // 反侦测检查点注入（--runtime-protection）：FAKE 命中时返回默认值走收尾。
    // 必须放在 PushLocalFrame 之后（fakeReturn 的 goto _mj2x_regc_cleanup 会
    // PopLocalFrame，前置会破坏帧平衡触发 JVM fail-fast）与 _mj2x_regc_result
    // 声明之后（fakeReturn 引用它）。
    if (options_.runtimeProtection.enabled()) {
        std::string fakeReturn = "goto _mj2x_regc_cleanup;";
        if (fn.symbol.returnType != SSAType::VOID) {
            fakeReturn = std::string("_mj2x_regc_result = ") + defaultValue(fn.symbol.returnType) +
                         "; goto _mj2x_regc_cleanup;";
        }
        codegen::emitRuntimeProtectionCheckpoint(out, options_.runtimeProtection,
                                                 std::string("mj2x_rt_cp_") + result.functionName,
                                                 fakeReturn);
    }
    out.goto_(ca::str::format_std("block_b{}", fn.entry->id.value));
    out.emitBlankLine();
    out.emitRaw(bodySource.to_std_string());

    if (options_.commentLevel >= RegCCommentLevel::Summary) {
        out.comment("_mj2x_regc_cleanup: native object teardown + local frame pop");
    }
    out.label_("_mj2x_regc_cleanup");
    for (uint32_t owner : emitState.nativeStringBuilderOwners) {
        const auto  objectIt = emitState.nativeObjects.find(owner);
        const char* destroySymbol =
            objectIt != emitState.nativeObjects.end() &&
                    objectIt->second.kind == RegCIntrinsicNativeObjectKind::StringBuffer
                ? "mj2x_string_buffer_destroy"
                : "mj2x_string_builder_destroy";
        out.call(destroySymbol, {stringBuilderOwnerName(RegId{owner}).to_std_string()});
    }
    // PopLocalFrame 释放本帧全部 local ref；REF 返回值通过 result 参数 promote 到父帧，
    // 非 REF/VOID 传 NULL。PopLocalFrame 对 pending exception 安全（返回值可能为 NULL）。
    if (needsLocalFrame) {
        if (fn.symbol.returnType == SSAType::REF) {
            out.assign("_mj2x_regc_result",
                       "(mj2x_ref)(*_mj2x_regc_env)->PopLocalFrame("
                       "_mj2x_regc_env, (jobject)_mj2x_regc_result)");
        }
        else {
            out.emitLine("(*_mj2x_regc_env)->PopLocalFrame(_mj2x_regc_env, NULL);");
        }
    }
    if (fn.symbol.returnType == SSAType::VOID) out.ret();
    else out.ret("_mj2x_regc_result");

    out.dedent();
    out.append("}\n");

    const std::string sourceCode = out.build().to_std_string();
    // 文本级判定生成体是否读取 owner class 全局（indy 自定义 bootstrap helper
    // 调用点会引用）；trampoline 发射据此省略无谓的 GetObjectClass。
    result.usesOwnerClass = sourceCode.find("_mj2x_regc_owner_class") != std::string::npos;
    result.source = sourceCode;
    if (!validateRegCNoStackAnchors(result.source, result.reason)) {
        result.ok = false;
        return result;
    }
    result.ok = true;
    result.reason.clear();
    return result;
}

namespace {

/// 阶段3 helper 路径（现状 emitInvokeDynamic 主体，逐字保留）：站点取 target 后
/// invokeWithArguments。RuntimeHelper 站点与直派桩的守卫回退分支共用本函数——
/// 发射文本与波次二之前逐字节一致（零回归锚）。
bool emitIndyHelperCall(CSourceStream& out, const RegInstruction& ins,
                        const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                        std::string& reason)
{
    const ca::str::Utf8StringRef descRef = ca::str::Utf8StringRef::from_cstr(site.desc.c_str());
    auto                         argTypes = Type::getArgumentTypes(descRef);
    const int                    argCount = static_cast<int>(argTypes.size());
    if (argCount != static_cast<int>(ins.args.size())) {
        reason = "indy arg count mismatch";
        return false;
    }
    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();

    bool ok = true;
    out.block([&](CSourceStream& o) {
        o.declare_var("jclass", "_mj2x_indy_obj_cls",
                      "(jclass)_mj2x_regc_class_literal(_mj2x_regc_current_context(), "
                      "\"java/lang/Object\")");
        o.appendLineStart("jobjectArray _mj2x_indy_args = (jobjectArray)(*_mj2x_regc_env)"
                          "->NewObjectArray(_mj2x_regc_env, ")
            .append(ca::str::format_std("{}", argCount))
            .append(", _mj2x_indy_obj_cls, NULL);")
            .newLine();
        o.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
        for (int i = 0; i < argCount; ++i) {
            const ca::str::Utf8String expr = operandExpr(ins.args[i]);
            std::string               boxExpr;
            switch (ins.args[i].type) {
            case SSAType::REF:
                boxExpr = codegen::exprCast("jobject", codegen::exprParen(expr.to_std_string()));
                break;
            case SSAType::BOOL:
            case SSAType::I8:
            case SSAType::I16:
            case SSAType::CHAR:
            case SSAType::I32:
                boxExpr = codegen::exprCall("_mj2x_regc_box_i32",
                                            {codegen::exprCast(
                                                "int32_t",
                                                codegen::exprParen(expr.to_std_string()))});
                break;
            case SSAType::I64:
                boxExpr = codegen::exprCall("_mj2x_regc_box_i64",
                                            {codegen::exprCast(
                                                "int64_t",
                                                codegen::exprParen(expr.to_std_string()))});
                break;
            case SSAType::F32:
                boxExpr = codegen::exprCall("_mj2x_regc_box_f32",
                                            {codegen::exprCast(
                                                "float",
                                                codegen::exprParen(expr.to_std_string()))});
                break;
            case SSAType::F64:
                boxExpr = codegen::exprCall("_mj2x_regc_box_f64",
                                            {codegen::exprCast(
                                                "double",
                                                codegen::exprParen(expr.to_std_string()))});
                break;
            default:
                reason = "unsupported indy argument type";
                ok = false;
                return;
            }
            if (!ok) return;
            o.emitLine(codegen::exprCall("(*_mj2x_regc_env)->SetObjectArrayElement",
                                         {"_mj2x_regc_env", "_mj2x_indy_args",
                                          ca::str::format_std("{}", i), boxExpr}) +
                       ";");
        }
        if (!ok) return;
        o.appendLineStart("jobject _mj2x_indy_target = _mj2x_regc_indy_bsm_")
            .append(ca::str::format_std("{}", globalSiteIndex))
            .append("(_mj2x_regc_owner_class);")
            .newLine();
        o.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
        o.emitLine("if (_mj2x_indy_target == NULL) { goto _mj2x_regc_cleanup; }");
        o.emitLine("jclass _mj2x_indy_mh_cls = (jclass)_mj2x_regc_class_literal("
                   "_mj2x_regc_current_context(), \"java/lang/invoke/MethodHandle\");");
        o.emitLine("jmethodID _mj2x_indy_invoke_mid = (*_mj2x_regc_env)->GetMethodID("
                   "_mj2x_regc_env, _mj2x_indy_mh_cls, \"invokeWithArguments\", "
                   "\"([Ljava/lang/Object;)Ljava/lang/Object;\");");
        o.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
        o.emitLine("jobject _mj2x_indy_result = (jobject)(*_mj2x_regc_env)->CallObjectMethod("
                   "_mj2x_regc_env, _mj2x_indy_target, _mj2x_indy_invoke_mid, _mj2x_indy_args);");
        o.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
        if (ins.hasDest) {
            std::string resultExpr;
            switch (ins.dest.type) {
            case SSAType::REF:
                resultExpr = codegen::exprCast("mj2x_ref", "_mj2x_indy_result");
                break;
            case SSAType::BOOL:
            case SSAType::I8:
            case SSAType::I16:
            case SSAType::CHAR:
            case SSAType::I32:
                resultExpr = codegen::exprCall("_mj2x_regc_unbox_i32", {"_mj2x_indy_result"});
                break;
            case SSAType::I64:
                resultExpr = codegen::exprCall("_mj2x_regc_unbox_i64", {"_mj2x_indy_result"});
                break;
            case SSAType::F32:
                resultExpr = codegen::exprCall("_mj2x_regc_unbox_f32", {"_mj2x_indy_result"});
                break;
            case SSAType::F64:
                resultExpr = codegen::exprCall("_mj2x_regc_unbox_f64", {"_mj2x_indy_result"});
                break;
            default:
                reason = "unsupported indy result type";
                ok = false;
                return;
            }
            if (!ok) return;
            o.assign(destName, resultExpr);
        }
        else {
            o.emitLine("(void)_mj2x_indy_result;");
        }
    });
    return ok;
}

/// 站点直派缓存的守卫条件（§4 单分支守卫）：state==READY 直达快路；未就绪时调
/// finalize 发射的解析函数（首调用解析 cls/fid/mid 成功置 READY，失败置 HELPER 并
/// ExceptionClear，单调回退既有 helper 路径，不存在第三态）。解析竞态良性：并发
/// 首调用各自解析出相同句柄（与既有 indy target 缓存同一权衡）。
std::string indyDirectGuardCondition(size_t globalSiteIndex)
{
    const std::string idx = ca::str::format_std("{}", globalSiteIndex);
    return "_mj2x_indy_direct_state_" + idx + " == 1 || _mj2x_indy_direct_resolve_" + idx + "()";
}

/// F1（DirectFieldAccess）直派发射：站点调用直接发射为字段访问（设计文档 §6 P2）。
/// NPE/异常语义：实例访问 receiver 为 NULL 时显式抛 NPE（复现 invokeWithArguments
/// 的 null receiver 行为）；字段访问本身的异常经 ExceptionCheck 进既有 cleanup
/// 异常分发（与 helper 路径一致）。字段 JNI 变体按字段描述符选择（判定已保证
/// 站点描述符与字段形态精确一致，寄存器类型匹配）。
bool emitIndyDirectFieldCall(CSourceStream& out, const RegInstruction& ins,
                             const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                             const RegFunction& fn, const RegBlock* block, std::string& reason)
{
    const IndyDirectField& field = *site.directField;

    // 发射期载荷自洽复核（非判定）：参数个数与读/写形态一致。
    const size_t expectedArgs = field.isStatic ? (field.isWrite ? 1u : 0u)
                                               : (field.isWrite ? 2u : 1u);
    if (ins.args.size() != expectedArgs) {
        reason = "indy direct field arg count mismatch";
        return false;
    }
    if (!field.isWrite && !ins.hasDest) {
        reason = "indy direct field read requires dest";
        return false;
    }

    const ca::str::Utf8StringRef descRef =
        ca::str::Utf8StringRef::from_cstr(field.fieldDesc.c_str());
    const char        fieldTag = descRef.data() != nullptr && descRef.byte_length() > 0
                                     ? static_cast<char>(descRef.data()[0])
                                     : 'L';
    const char*       family   = jniCallFamily(fieldTag);
    const std::string idx      = ca::str::format_std("{}", globalSiteIndex);
    const std::string clsVar   = "_mj2x_indy_direct_cls_" + idx;
    const std::string fidVar   = "_mj2x_indy_direct_fid_" + idx;
    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();

    bool helperOk = true;
    out.iff(indyDirectGuardCondition(globalSiteIndex),
            [&](CSourceStream& o) {
                std::string recvVar;
                // 守卫抛出后的分流依据：站点指令所在块是否落在异常保护区间内
                //（与 monitor enter/exit 守卫同口径）。
                const bool blockProtected = !activeExceptionRegions(fn, block).empty();
                if (!field.isStatic) {
                    recvVar = "_mj2x_indy_direct_recv_" + idx;
                    o.declare_var("jobject", recvVar,
                                  codegen::exprCast("jobject",
                                                    codegen::exprParen(
                                                        operandExpr(ins.args[0]).to_std_string())));
                }
                // 字段访问体：实例形态放进 null 守卫的 else 分支——受保护路径
                // throw_null 落空后直接进指令后异常分发，绝不能以 NULL receiver
                // 递进 JNI；静态形态无 receiver，直接发射。
                auto emitAccess = [&](CSourceStream& u) {
                    if (!field.isWrite) {
                        const std::string access =
                            field.isStatic
                                ? std::string("(*_mj2x_regc_env)->GetStatic") + family +
                                      "Field(_mj2x_regc_env, " + clsVar + ", " + fidVar + ")"
                                : std::string("(*_mj2x_regc_env)->Get") + family +
                                      "Field(_mj2x_regc_env, " + recvVar + ", " + fidVar + ")";
                        u.assign(destName,
                                 jniResultCast(fieldTag, ins.dest.type).to_std_string() + access);
                    }
                    else {
                        const RegOperand& valueOperand = field.isStatic ? ins.args[0] : ins.args[1];
                        const std::string valueExpr =
                            std::string(jniValueCast(fieldTag)) + "(" +
                            operandExpr(valueOperand).to_std_string() + ")";
                        const std::string access =
                            field.isStatic
                                ? std::string("(*_mj2x_regc_env)->SetStatic") + family +
                                      "Field(_mj2x_regc_env, " + clsVar + ", " + fidVar + ", " +
                                      valueExpr + ")"
                                : std::string("(*_mj2x_regc_env)->Set") + family +
                                      "Field(_mj2x_regc_env, " + recvVar + ", " + fidVar + ", " +
                                      valueExpr + ")";
                        u.emitLine(access + ";");
                    }
                    u.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
                };
                if (!field.isStatic) {
                    // JVMS getfield/putfield：receiver NULL 时于当前 pc 抛 NPE，
                    // 按异常表路由到方法自身 handler。未受保护块 goto cleanup 直接
                    // 传播；受保护块落空进指令后异常分发（issue #958：原实现
                    // 无条件 goto cleanup 跳过分发，try 内 catch NPE 接不住）。
                    o.iff(recvVar + " == NULL",
                          [&](CSourceStream& u) {
                              u.emitLine("_mj2x_regc_throw_null(\"indy direct field access\");");
                              if (!blockProtected) u.goto_("_mj2x_regc_cleanup");
                          },
                          emitAccess);
                }
                else {
                    emitAccess(o);
                }
            },
            [&](CSourceStream& o) {
                helperOk = emitIndyHelperCall(o, ins, site, globalSiteIndex, reason);
            });
    return helperOk;
}

/// F2（RecordObjectMethods）getter 直派发射：访问器 invokestatic/virtual 直调。
/// receiver 形态与访问器 tag 由判定载荷给出；参数经 jvalue 数组 marshalling，返回
/// 按访问器描述符的 JNI 变体取回（等价既有 cached call 面）。无可直派访问器时
/// （javac equals/hashCode/toString 等 ObjectMethods 内建语义）整体落既有 helper
/// 路径——与现状逐字节一致，零回归。
bool emitIndyDirectRecordCall(CSourceStream& out, const RegInstruction& ins,
                              const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                              const RegFunction& fn, const RegBlock* block, std::string& reason)
{
    const IndyRecordMethods& record = *site.recordMethods;
    if (!record.accessor.has_value()) {
        return emitIndyHelperCall(out, ins, site, globalSiteIndex, reason);
    }
    const IndyRecordMethods::Accessor& accessor = *record.accessor;

    const ca::str::Utf8StringRef descRef =
        ca::str::Utf8StringRef::from_cstr(accessor.desc.c_str());
    const std::vector<char> paramTags     = descriptorParamTags(descRef);
    const char              returnTag     = descriptorReturnTag(descRef);
    const bool              staticAccessor = accessor.tag == Opcodes::H_INVOKESTATIC;

    // 发射期载荷自洽复核（非判定）：参数个数与访问器形态一致。
    const size_t expectedArgs = paramTags.size() + (staticAccessor ? 0u : 1u);
    if (ins.args.size() != expectedArgs) {
        reason = "indy direct record accessor arg count mismatch";
        return false;
    }

    const std::string idx    = ca::str::format_std("{}", globalSiteIndex);
    const std::string clsVar = "_mj2x_indy_direct_cls_" + idx;
    const std::string midVar = "_mj2x_indy_direct_mid_" + idx;
    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();

    bool helperOk = true;
    out.iff(indyDirectGuardCondition(globalSiteIndex),
            [&](CSourceStream& o) {
                size_t      firstArg = 0;
                std::string recvVar;
                // 守卫抛出后的分流依据（与 F1 字段面同口径）。
                const bool blockProtected = !activeExceptionRegions(fn, block).empty();
                if (!staticAccessor) {
                    recvVar = "_mj2x_indy_direct_recv_" + idx;
                    o.declare_var("jobject", recvVar,
                                  codegen::exprCast("jobject",
                                                    codegen::exprParen(
                                                        operandExpr(ins.args[0]).to_std_string())));
                    firstArg = 1;
                }
                const std::string vaVar = "_mj2x_indy_direct_va_" + idx;
                // 访问器调用体：实例形态放进 null 守卫的 else 分支——受保护路径
                // throw_null 落空后直接进指令后异常分发，绝不能以 NULL receiver
                // 递进 JNI；静态访问器无 receiver，直接发射。
                auto emitAccessorCall = [&](CSourceStream& u) {
                    if (!paramTags.empty()) {
                        u.declare_var("jvalue",
                                      ca::str::format_std("{}[{}]", vaVar, paramTags.size()));
                        for (size_t i = 0; i < paramTags.size(); ++i) {
                            u.assign(ca::str::format_std("{}[{}].{}", vaVar, i,
                                                         jvalueSlotName(paramTags[i])),
                                     std::string(jniValueCast(paramTags[i])) + "(" +
                                         operandExpr(ins.args[firstArg + i]).to_std_string() +
                                         ")");
                        }
                    }
                    const std::string call =
                        std::string("(*_mj2x_regc_env)->Call") + (staticAccessor ? "Static" : "") +
                        jniCallFamily(returnTag) + "MethodA(_mj2x_regc_env, " +
                        (staticAccessor ? clsVar : recvVar) + ", " + midVar + ", " +
                        (paramTags.empty() ? std::string("NULL") : vaVar) + ")";
                    if (ins.hasDest) {
                        u.assign(destName,
                                 jniResultCast(returnTag, ins.dest.type).to_std_string() + call);
                    }
                    else {
                        u.emitLine(call + ";");
                    }
                    u.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
                };
                if (!staticAccessor) {
                    // JVMS invokevirtual 系：receiver NULL 时于当前 pc 抛 NPE，按
                    // 异常表路由到方法自身 handler。未受保护块 goto cleanup 直接
                    // 传播；受保护块落空进指令后异常分发（issue #958：原实现
                    // 无条件 goto cleanup 跳过分发，try 内 catch NPE 接不住）。
                    o.iff(recvVar + " == NULL",
                          [&](CSourceStream& u) {
                              u.emitLine("_mj2x_regc_throw_null(\"indy direct accessor call\");");
                              if (!blockProtected) u.goto_("_mj2x_regc_cleanup");
                          },
                          emitAccessorCall);
                }
                else {
                    emitAccessorCall(o);
                }
            },
            [&](CSourceStream& o) {
                helperOk = emitIndyHelperCall(o, ins, site, globalSiteIndex, reason);
            });
    return helperOk;
}

}   // namespace

/// invokedynamic 站点直派发射（设计文档 §6 P2）：F1 字段读/写、F2 record 访问器。
/// build-time 判定形态且载荷 engage 时发单分支守卫直派桩；其余（RuntimeHelper /
/// StaticExpanded / 载荷未 engage）全部落回现状 helper 路径。fn/block 供直派
/// 面的 null receiver 守卫做块保护分流（issue #958）。
bool emitIndyDirectCall(CSourceStream& out, const RegInstruction& ins,
                        const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                        const RegFunction& fn, const RegBlock* block, std::string& reason)
{
    if (site.form == IndyDispatchForm::DirectFieldAccess && site.directField.has_value()) {
        return emitIndyDirectFieldCall(out, ins, site, globalSiteIndex, fn, block, reason);
    }
    if (site.form == IndyDispatchForm::RecordObjectMethods && site.recordMethods.has_value()) {
        return emitIndyDirectRecordCall(out, ins, site, globalSiteIndex, fn, block, reason);
    }
    return emitIndyHelperCall(out, ins, site, globalSiteIndex, reason);
}

/// 发射一个 invokedynamic 站点调用：入口按站点派发形态分派（P2），直派形态走
/// emitIndyDirectCall，其余走既有 helper 路径（发射文本与现状逐字节一致）。
bool emitInvokeDynamic(CSourceStream& out, const RegInstruction& ins,
                       const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                       const RegFunction& fn, const RegBlock* block, std::string& reason)
{
    return emitIndyDirectCall(out, ins, site, globalSiteIndex, fn, block, reason);
}

bool validateRegCNoStackAnchors(const std::string& source, std::string& reason)
{
    // 锚点必须是 RegC 不会产出的标识符形态。自然语言片段（如早期的
    // "Cannot invoke"）会误伤含异常文案字符串常量的合法方法——受保护代码
    // 里 "Cannot invoke method ..." 之类的消息极常见，门禁会静默拒绝整个
    // 方法的 native 化。
    const char* anchors[] = {
        "cstack",
        "stack[",
        "union jvalue",
        " sp ",
        "sp--",
        "sp++",
        "cclasses[",
        "cmethods[",
    };
    for (const char* anchor : anchors) {
        if (source.find(anchor) != std::string::npos) {
            reason = std::string("RegC output contains stack-machine anchor: ") + anchor;
            return false;
        }
    }
    reason.clear();
    return true;
}

}   // namespace mj2x
