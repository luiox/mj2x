/// @file regc_flow.cpp
/// @brief RegC 发射器指令流发射（regc_emitter_internal.hpp 的内部接口）。
///
/// @details
/// 寄存器收集（collectRegister/collectRegisters）、指令支持性检查、指令/终结符/
/// 异常分发/边拷贝发射（emitInstruction/emitTerminator/emitExceptionDispatch/
/// emitEdgeCopies）与函数级 helper prelude。从原 regc_emitter.cpp 纯搬移，
/// 生成代码零差异。
#include "regc_emitter_internal.hpp"

#include "mj2x/mir/passes/const_key_derivation.hpp"

#include "mj2x/core/jvm_opcodes.hpp"
#include "mj2x/core/jvm_type.hpp"
#include "libca/str/format.hpp"
#include "mj2x/codegen/c_expr.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/jvm_semantics.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace mj2x {


using codegen::cStringLiteral;
using codegen::CSourceStream;

namespace {

void collectCopies(std::map<uint32_t, SSAType>& regs, const std::vector<RegCopy>& copies)
{
    for (const auto& copy : copies) collectRegister(regs, copy.dest);
}

void collectExceptionRegisters(std::map<uint32_t, SSAType>&           regs,
                               const std::vector<RegExceptionRegion>& regions)
{
    for (const auto& region : regions) {
        collectRegister(regs, region.exceptionValue);
        for (const auto& edge : region.edges) collectCopies(regs, edge.copies);
    }
}

void collectTerminatorRegisters(std::map<uint32_t, SSAType>& regs, const RegTerminator& terminator)
{
    std::visit(
        [&](const auto& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                collectCopies(regs, t.target.copies);
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                collectCopies(regs, t.trueTarget.copies);
                collectCopies(regs, t.falseTarget.copies);
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                for (const auto& c : t.cases) collectCopies(regs, c.target.copies);
                collectCopies(regs, t.defaultTarget.copies);
            }
        },
        terminator);
}

bool isSupportedInstruction(const RegInstruction& ins, std::string& reason)
{
    switch (ins.kind) {
    case RegInstructionKind::Copy:
    case RegInstructionKind::Unary:
    case RegInstructionKind::Binary:
    case RegInstructionKind::Compare:
    case RegInstructionKind::Convert:
    case RegInstructionKind::LoadField:
    case RegInstructionKind::StoreField:
    case RegInstructionKind::ArrayLoad:
    case RegInstructionKind::ArrayStore:
    case RegInstructionKind::Call:
    case RegInstructionKind::Allocate:
    case RegInstructionKind::Intrinsic:
    case RegInstructionKind::Barrier: return true;
    }
    reason = "unknown RegIR instruction kind";
    return false;
}

ca::str::Utf8String binaryExpr(const RegInstruction& ins)
{
    const ca::str::Utf8String lhs = operandExpr(ins.lhs);
    const ca::str::Utf8String rhs = operandExpr(ins.rhs);
    const SSAType             type = ins.dest.type;

    if (isIntegerLike(type) &&
        (ins.binaryOp == SSABinaryOp::Div || ins.binaryOp == SSABinaryOp::Rem)) {
        const char* op    = ins.binaryOp == SSABinaryOp::Div ? "div" : "rem";
        const char* width = type == SSAType::I64 ? "i64" : "i32";
        CSourceStream os;
        os.append("mj2x_regc_");
        os.append(op);
        os.append("_");
        os.append(width);
        os.append("(_mj2x_regc_current_context(), ");
        os.append(lhs);
        os.append(", ");
        os.append(rhs);
        os.append(")");
        return os.build();
    }
    if (isIntegerLike(type)) {
        // JVM 移位计数按位宽掩码（int &31 / long &63，JLS 15.19）：超界计数是
        // 定义行为，C 裸移位是 UB。掩码值来自 codegen/jvm_semantics.hpp 共享
        // 决策（g13.shift_out_of_range 回归守门）。
        const bool isShift = ins.binaryOp == SSABinaryOp::Shl ||
                             ins.binaryOp == SSABinaryOp::Shr ||
                             ins.binaryOp == SSABinaryOp::UShr;
        if (isShift) {
            const int mask = codegen::jvmShiftMask(type);
            if (ins.binaryOp == SSABinaryOp::UShr) {
                CSourceStream os;
                os.append("(");
                os.append(cType(type));
                os.append(")((");
                os.append(unsignedCType(type));
                os.append(")");
                os.append(lhs);
                os.append(" >> (");
                os.append(rhs);
                os.append(" & ");
                os.append(ca::str::format_std("{}", mask));
                os.append("))");
                return os.build();
            }
            CSourceStream os;
            os.append("(");
            os.append(cType(type));
            os.append(")(");
            if (ins.binaryOp == SSABinaryOp::Shl) {
                os.append("(");
                os.append(unsignedCType(type));
                os.append(")");
                os.append(lhs);
                os.append(" << (");
                os.append(rhs);
                os.append(" & ");
                os.append(ca::str::format_std("{}", mask));
                os.append(")");
            }
            else {
                os.append(lhs);
                os.append(" >> (");
                os.append(rhs);
                os.append(" & ");
                os.append(ca::str::format_std("{}", mask));
                os.append(")");
            }
            os.append(")");
            return os.build();
        }
    }
    if (isIntegerLike(type) &&
        (ins.binaryOp == SSABinaryOp::Add || ins.binaryOp == SSABinaryOp::Sub ||
         ins.binaryOp == SSABinaryOp::Mul)) {
        CSourceStream os;
        os.append("(");
        os.append(cType(type));
        os.append(")((");
        os.append(unsignedCType(type));
        os.append(")");
        os.append(lhs);
        os.append(" ");
        os.append(binaryOp(ins.binaryOp));
        os.append(" (");
        os.append(unsignedCType(type));
        os.append(")");
        os.append(rhs);
        os.append(")");
        return os.build();
    }
    if ((type == SSAType::F32 || type == SSAType::F64) && ins.binaryOp == SSABinaryOp::Rem) {
        CSourceStream os;
        os.append(type == SSAType::F32 ? "fmodf" : "fmod");
        os.append("(");
        os.append(lhs);
        os.append(", ");
        os.append(rhs);
        os.append(")");
        return os.build();
    }
    CSourceStream os;
    os.append("(");
    os.append(cType(type));
    os.append(")(");
    os.append(lhs);
    os.append(" ");
    os.append(binaryOp(ins.binaryOp));
    os.append(" ");
    os.append(rhs);
    os.append(")");
    return os.build();
}

bool emitEdgeCopies(CSourceStream& out, const std::vector<RegCopy>& copies,
                    RegCFunctionEmitState& state, int& edgeTempIndex, std::string& reason)
{
    if (copies.empty()) return true;

    // Edge copies are parallel phi transfers.  Stage sources in temporaries so
    // cycles such as r1<-r2 and r2<-r1 remain well-defined for later RegC work.
    std::vector<int>      tempIds;
    tempIds.reserve(copies.size());
    // 需要按值补发 struct 拷贝的边拷贝下标（POD struct 专用）。
    std::vector<size_t>   podStructEdgeIndices;
    for (size_t i = 0; i < copies.size(); ++i) {
        const RegCNativeObjectState* objectState = nativeObjectFor(state, copies[i].source);
        if (objectState != nullptr && !isMutableStringKind(objectState->kind) &&
            objectState->kind != RegCIntrinsicNativeObjectKind::PodStruct) {
            reason = "native object escaped through CFG edge copy";
            return false;
        }
        // POD struct 是值语义，跨 CFG 边传递拷贝的是结构体本身，放行。临时变量
        // 与索引记账照常参与（否则 tempIds 与第二个循环错位），结构体拷贝在两个
        // 循环之后补发。
        if (objectState != nullptr &&
            objectState->kind == RegCIntrinsicNativeObjectKind::PodStruct) {
            podStructEdgeIndices.push_back(i);
        }
        const int tempId = edgeTempIndex++;
        tempIds.push_back(tempId);
        // Record the actual type per emitted temporary so the declaration block
        // declares exactly the temporaries that are used. A block with N mayThrow
        // instructions re-emits its exception edge copies N times, so the emit count
        // can exceed the static CFG edge count that collectEdgeCopyTempTypes sees.
        if (static_cast<int>(state.edgeTempTypes.size()) <= tempId) {
            state.edgeTempTypes.resize(static_cast<size_t>(tempId) + 1, copies[i].dest.type);
        }
        out.appendLineStart("edge_tmp_")
            .append(ca::str::format_std("{}", tempId))
            .append(" = ")
            .append(codegen::exprCast(cType(copies[i].dest.type),
                                      codegen::exprParen(operandExpr(copies[i].source).to_std_string())))
            .append(";")
            .newLine();
    }
    for (size_t i = 0; i < copies.size(); ++i) {
        const std::string destName = regName(copies[i].dest.id);
        out.assign(destName, ca::str::format_std("edge_tmp_{}", tempIds[i]));
    }
    // POD struct 的按值拷贝：临时变量通道只搬了 NULL 引用，结构体本体在这里补。
    for (size_t i : podStructEdgeIndices) {
        podStructEdgeCopy(out, state, copies[i]);
    }
    return true;
}

bool protectsBlock(const RegExceptionRegion& region, const RegBlock* block)
{
    return std::find(region.protectedBlocks.begin(), region.protectedBlocks.end(), block) !=
           region.protectedBlocks.end();
}

void emitDefaultReturnAfterException(CSourceStream& out, SSAType returnType)
{
    if (returnType == SSAType::VOID) {
        out.goto_("_mj2x_regc_cleanup");
    }
    else {
        out.assign("_mj2x_regc_result", defaultValue(returnType));
        out.goto_("_mj2x_regc_cleanup");
    }
}

}   // namespace

bool collectRegister(std::map<uint32_t, SSAType>& regs, RegValue value)
{
    auto [it, inserted] = regs.emplace(value.id.value, value.type);
    return inserted || it->second == value.type;
}

bool collectRegisters(const RegFunction& fn, std::map<uint32_t, SSAType>& regs, std::string& reason)
{
    for (const auto& parameter : fn.parameters) collectRegister(regs, parameter);
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.hasDest && !collectRegister(regs, ins.dest)) {
                reason = "conflicting RegIR destination type";
                return false;
            }
            if (ins.kind == RegInstructionKind::Copy) collectRegister(regs, ins.copy.dest);
        }
        collectTerminatorRegisters(regs, block->terminator);
    }
    collectExceptionRegisters(regs, fn.exceptionRegions);
    return true;
}

void emitHelperPrelude(ca::str::Utf8StringBuilder& out,
                       const RegCIntrinsicRegistry* intrinsicRegistry)
{
    // 历史的手工前向声明数组（issue #781）已删除：全部 runtime helper 的定义
    // 由 emitRuntimeHelpers 在方法体之前生成，声明冗余；多文件模式下声明由
    // splitRewriteStatic 从 runtime 段自动提取，无需手工列表。仅保留 intrinsic
    // 声明（由注册表驱动，随新增 intrinsic 自动更新）。用户自定义 intrinsic
    // registry 传入时从该实例发射：同一 emitDeclarations 覆盖 builtins 固定
    // 声明并追加用户 symbol 的 extern 声明（passReceiver 条目含 receiver 参数）。
    if (intrinsicRegistry != nullptr) {
        intrinsicRegistry->emitDeclarations(out);
        return;
    }
    RegCIntrinsicRegistry::builtins().emitDeclarations(out);
}

std::vector<const RegExceptionRegion*> activeExceptionRegions(const RegFunction& fn,
                                                              const RegBlock*    block)
{
    std::vector<const RegExceptionRegion*> active;
    for (const auto& region : fn.exceptionRegions) {
        if (protectsBlock(region, block)) active.push_back(&region);
    }
    return active;
}

/// 判断已发射的方法体是否含产生 JNI local ref 的操作，用于 gate PushLocalFrame/
/// PopLocalFrame 帧管理。命中任一产 ref helper 即需要帧；纯标量/纯字段算术方法
/// 不含这些标记，保留「零 JNI 帧开销」形态。marker 覆盖：方法调用（call_*，含
/// cached / instance / ref / void——void 调用也可能内部 FindClass 泄漏 class ref）、
/// 对象/数组分配、class/string 字面量、ref 字段与 ref 数组读取。
bool bodyProducesLocalRefs(const ca::str::Utf8StringRef& body)
{
    static const char* kMarkers[] = {
        "_mj2x_regc_call_",          // 所有方法调用（含 static FindClass 路径）
        "_mj2x_regc_alloc_",         // alloc_object / alloc_array / alloc_multi_array
        "_mj2x_regc_class_literal",  // Foo.class -> FindClass
        "_mj2x_intrinsic_string_literal",
        "_mj2x_regc_box_",           // 小值缓存装箱（miss 路径产 local ref）
        "_mj2x_regc_load_ref_field",
        "_mj2x_regc_load_cached_ref_field",
        "_mj2x_regc_load_ref_array",
    };
    for (const char* marker : kMarkers) {
        if (body.contains(ca::str::Utf8StringRef::from_cstr(marker))) return true;
    }
    return false;
}

/// 判定「私有实例方法 + this receiver」调用点：JVM 语义下 private 方法无覆写、
/// 精确分派到声明类实现，因此可跳过 resolved dispatch 的运行时类检查
/// （GetObjectClass + IsSameObject，2 JNI/调用）。已转换的 private 实现 →
/// 直接调用 regc 函数（0 JNI）；未转换 → cached instance 调用（GetMethodID
/// 缓存 + CallXxxMethodA）。classIndex 缺失或方法未找到时保守返回 false。
bool isPrivateThisCall(const RegCFunctionEmitState& state, const RegInstruction& ins,
                       const RegFunction& fn)
{
    if (ins.args.empty() || fn.parameters.empty()) return false;
    const RegOperand& receiver = ins.args[0];
    if (receiver.kind != RegOperandKind::Register) return false;
    if (receiver.reg.value != fn.parameters[0].id.value) return false;
    if (state.classIndex == nullptr) return false;
    CompilerClass* cls = state.classIndex->getClass(ins.method.owner.to_std_string());
    if (cls == nullptr || cls->dom == nullptr) return false;
    const std::string name = ins.method.name.to_std_string();
    const std::string desc = ins.method.desc.to_std_string();
    for (const auto& mn : cls->methods) {
        if (mn.name == name && mn.desc == desc) {
            return (mn.access & Opcodes::ACC_PRIVATE) != 0;
        }
    }
    return false;
}

/// 收集 value 在 block 内的可能精确类（分配点传播，receiver 静态绑定用）。
///
/// @details
/// 沿 SSA def 回溯：Allocate(Object) 给出精确类；Copy 穿透；块入口值沿前驱
/// 边拷贝（RegSuccessor.copies）递归，多前驱取并集；其余来源（调用返回、
/// 字段/数组 load、转换等）与参数视为未知（返回 false）。回边环来源跳过
/// （环内值类型由非回边来源决定，不贡献新信息）。返回 true 表示至少一个
/// 来源给出精确类；out 为去重后的类描述符列表（内部名）。
static bool receiverExactTypes(const RegFunction& fn, const RegBlock* block, const RegId& value,
                               std::vector<std::string>& out,
                               std::set<std::pair<uint32_t, uint32_t>>& visited)
{
    if (block == nullptr) return false;
    const auto key = std::make_pair(block->id.value, value.value);
    if (!visited.insert(key).second) {
        // 回边环：该来源不贡献新信息（环内值类型由非回边来源决定），跳过。
        return true;
    }
    // 1) 块内 def（SSA 每值一次定义）。
    for (const auto& ins : block->instructions) {
        if (!ins.hasDest || ins.dest.id != value) continue;
        if (ins.kind == RegInstructionKind::Allocate) {
            if (ins.allocationKind != SSAAllocationKind::Object || ins.typeDesc.is_empty()) {
                return false;
            }
            const std::string desc = ins.typeDesc.to_std_string();
            if (std::find(out.begin(), out.end(), desc) == out.end()) out.push_back(desc);
            return true;
        }
        if (ins.kind == RegInstructionKind::Copy) {
            if (ins.copy.source.kind != RegOperandKind::Register) return false;
            return receiverExactTypes(fn, block, ins.copy.source.reg, out, visited);
        }
        return false;   // 调用返回/字段/数组 load/转换等 → 未知
    }
    // 2) 块入口值：参数 → 未知；否则沿前驱边拷贝回溯。
    for (const auto& param : fn.parameters) {
        if (param.id == value) return false;
    }
    bool foundAny = false;
    for (const auto& pred : fn.blocks) {
        std::vector<const RegSuccessor*> successors;
        std::visit(
            [&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    successors.push_back(&t.target);
                }
                else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    successors.push_back(&t.trueTarget);
                    successors.push_back(&t.falseTarget);
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    for (const auto& c : t.cases) successors.push_back(&c.target);
                    successors.push_back(&t.defaultTarget);
                }
            },
            pred->terminator);
        for (const auto* succ : successors) {
            if (succ->block != block) continue;
            const RegCopy* match = nullptr;
            for (const auto& c : succ->copies) {
                if (c.dest.id == value) {
                    match = &c;
                    break;
                }
            }
            if (match == nullptr) return false;   // 无提供该值的 pred → 保守未知
            if (match->source.kind != RegOperandKind::Register) return false;
            if (!receiverExactTypes(fn, pred.get(), match->source.reg, out, visited)) return false;
            foundAny = true;
        }
    }
    return foundAny;
}

/// 尝试发射「receiver 精确类型静态绑定」调用：receiver 的 SSA def 可证为
/// 单一分配点类（receiverExactTypes）且该类的目标方法已转换时，跳过 resolved
/// dispatch 的运行时类检查（双槽 IsInstanceOf，平均 1.5-2 JNI/调用），直接
/// 调用 regc_* 函数体（0 JNI，保留 null 检查以符合 JVM 动态调用语义）。
/// 未命中任何条件返回 false，由调用方走原 resolved dispatch 路径。
static bool tryEmitStaticBoundCall(CSourceStream& out, const RegInstruction& ins,
                                   RegCFunctionEmitState& state, const RegFunction& fn,
                                   const RegBlock* block,
                                   const std::vector<ca::str::Utf8String>& expressions)
{
    // 与 tryEmitDirectCall 一致：按档位判定（CodeOptions.directCallTier），
    // 保持 JVM 栈帧语义（getStackTrace/StackWalker/反射，issue #814）。
    // 静态绑定调用点是同类外的静态目标，故 sameClass=false。
    if (!allowsDirectCall(state.directCallTier, false, false)) return false;
    if (ins.args.empty()) return false;
    const RegOperand& receiver = ins.args[0];
    if (receiver.kind != RegOperandKind::Register) return false;
    if (state.convertedFunctions == nullptr) return false;
    // 参数不得携带 native 对象（与 privateThis 直调一致）。
    for (size_t i = 1; i < ins.args.size(); ++i) {
        if (nativeObjectFor(state, ins.args[i]) != nullptr) return false;
    }
    std::vector<std::string>               types;
    std::set<std::pair<uint32_t, uint32_t>> visited;
    if (!receiverExactTypes(fn, block, receiver.reg, types, visited) || types.size() != 1) {
        return false;
    }
    const std::string owner = types[0];
    const std::string name  = ins.method.name.to_std_string();
    const std::string desc  = ins.method.desc.to_std_string();
    const std::string key   = methodKey(owner, name, desc);
    if (state.indyFunctions != nullptr && state.indyFunctions->count(key) != 0) return false;
    const auto direct = state.convertedFunctions->find(key);
    if (direct == state.convertedFunctions->end() || direct->second.empty()) return false;

    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();
    // 前向声明：目标函数定义可能在本调用点之后（同类方法转换顺序不保证）。
    // 声明登记到 TU 头部注册表——块作用域 static 函数声明违反 C11 6.7.1p6
    // （GCC 拒绝，MSVC 放行），见 declareDirectCallTarget。
    declareDirectCallTarget(state, out, ins, direct->second);
    out.block([&](CSourceStream& o) {
        o.emitLine("jobject _mj2x_regc_rcv = " +
                   codegen::exprCast("jobject",
                                     codegen::exprParen(expressions[0].to_std_string())) +
                   ";");
        o.iff(codegen::exprBinop("==", "_mj2x_regc_rcv", "NULL"),
              [&](CSourceStream& s) {
                  s.emitLine("_mj2x_regc_throw_null(\"instance method call\");");
                  if (ins.hasDest) {
                      s.emitLine(destName + " = " + std::string(defaultValue(ins.dest.type)) + ";");
                  }
              },
              [&](CSourceStream& s) {
                  // 实参：receiver 用检查过的 _mj2x_regc_rcv（null 已排除），
                  // 其余按调用点表达式。
                  s.appendLineStart();
                  if (ins.hasDest) s.append(destName).append(" = ");
                  s.append(direct->second)
                      .append("((mj2x_ref)_mj2x_regc_rcv");
                  for (size_t i = 1; i < ins.args.size(); ++i) {
                      s.append(", ")
                          .append(codegen::exprCast(cType(ins.args[i].type),
                                                    codegen::exprParen(
                                                        expressions[i].to_std_string())));
                  }
                  s.append(");").newLine();
              });
    });
    return true;
}

bool emitExceptionDispatch(CSourceStream&                        out,
                           const std::vector<const RegExceptionRegion*>&     regions,
                           const RegBlock* block, SSAType returnType,
                           RegCFunctionEmitState& state, int& dispatchIndex,
                           int& edgeTempIndex, std::string& reason)
{
    const int index = dispatchIndex++;
    bool      copiesOk = true;
    out.iff("_mj2x_regc_exception_check()", [&](CSourceStream& u) {
        if (regions.empty()) {
            emitDefaultReturnAfterException(u, returnType);
            return;
        }
        u.declare_var("mj2x_ref", ca::str::format_std("ex_{}", index),
                      "_mj2x_regc_exception_occurred()");
            u.call("_mj2x_regc_exception_clear", {});
        // Match in exception-table order. A matched handler receives the caught
        // exception through its synthetic exception register before the jump.
        for (const auto* region : regions) {
            u.iff(ca::str::format_std("_mj2x_regc_exception_matches(ex_{}, {})", index,
                                      cStringLiteral(region->caughtType)),
                  [&](CSourceStream& v) {
                      for (const auto& edge : region->edges) {
                          if (edge.protectedBlock != block) continue;
                          if (!emitEdgeCopies(v, edge.copies, state, edgeTempIndex, reason)) {
                              copiesOk = false;
                              return;
                          }
                          break;
                      }
                      v.assign(regName(region->exceptionValue.id),
                               ca::str::format_std("ex_{}", index));
                      v.goto_(ca::str::format_std("block_b{}", region->handler->id.value));
                  });
            if (!copiesOk) return;
        }
        // 若没有任何 handler 接受挂起的异常，保持 Java 传播语义。
        u.call("_mj2x_regc_throw", {ca::str::format_std("ex_{}", index)});
        emitDefaultReturnAfterException(u, returnType);
    });
    return copiesOk;
}

bool emitInstruction(CSourceStream& out, const RegInstruction& ins,
                     RegCFunctionEmitState& state, const RegFunction& fn, const RegBlock* block,
                     size_t indySiteBase, std::string& reason)
{
    if (!isSupportedInstruction(ins, reason)) return false;

    switch (ins.kind) {
    case RegInstructionKind::Copy:
        if (const auto* objectState = nativeObjectFor(state, ins.copy.source)) {
            if (isMutableStringKind(objectState->kind)) {
                const std::string destName = regName(ins.copy.dest.id);
                out.assign(destName, operandExpr(ins.copy.source).to_std_string());
                state.nativeObjects[ins.copy.dest.id.value] =
                    RegCNativeObjectState{objectState->kind, destName};
            }
            else {
                const ca::str::Utf8String storageName = nativeObjectStorageName(ins.copy.dest.id);
                out.appendLineStart(nativeObjectType(objectState->kind))
                    .append(" ")
                    .append(storageName)
                    .append(" = ")
                    .append(objectState->storageName)
                    .append(";")
                    .newLine();
                const std::string destName = regName(ins.copy.dest.id);
                out.assign(destName, "NULL");
                state.nativeObjects[ins.copy.dest.id.value] =
                    RegCNativeObjectState{objectState->kind, storageName.to_std_string()};
            }
        }
        else {
            state.nativeObjects.erase(ins.copy.dest.id.value);
            const std::string destName = regName(ins.copy.dest.id);
            out.assign(destName, codegen::exprCast(cType(ins.copy.dest.type),
                                                   codegen::exprParen(
                                                       operandExpr(ins.copy.source).to_std_string())));
        }
        return true;
    case RegInstructionKind::Unary:
    {
        const std::string destName = regName(ins.dest.id);
        out.assign(destName,
                   codegen::exprCast(cType(ins.dest.type),
                                     "(-" + operandExpr(ins.value).to_std_string() + ")"));
        return true;
    }
    case RegInstructionKind::Binary:
    {
        const std::string destName = regName(ins.dest.id);
        out.assign(destName, binaryExpr(ins).to_std_string());
        return true;
    }
    case RegInstructionKind::Compare:
        // REF 比较（acmp / 与 null 比较）：JNI local ref 是句柄，同一对象的两个不同
        // local ref 句柄指针不相等，裸 void* 比较会误判。必须用 IsSameObject 判定对象
        // 同一性（对 NULL 也正确：IsSameObject(x, NULL) 当且仅当 x 为 null 时为真）。
        // acmp 在字节码只有 eq/ne 两种谓词。
        if (ins.lhs.type == SSAType::REF || ins.rhs.type == SSAType::REF) {
            const std::string destName = regName(ins.dest.id);
            const std::string sameObject = codegen::exprCall(
                "(*_mj2x_regc_env)->IsSameObject",
                {"_mj2x_regc_env", codegen::exprCast("jobject", operandExpr(ins.lhs).to_std_string()),
                 codegen::exprCast("jobject", operandExpr(ins.rhs).to_std_string())});
            out.assign(destName,
                       codegen::exprParen((ins.comparePredicate == SSAComparePredicate::Ne ? "!" : "") +
                                          sameObject));
            return true;
        }
    {
        const std::string destName = regName(ins.dest.id);
        out.assign(destName, codegen::exprParen(
                                 codegen::exprBinop(compareOp(ins.comparePredicate),
                                                    operandExpr(ins.lhs).to_std_string(),
                                                    operandExpr(ins.rhs).to_std_string())));
        return true;
    }
    case RegInstructionKind::Convert:
        if (!rejectNativeObjectEscape(state, ins.value, "reference conversion", reason))
            return false;
        if (ins.convertKind == SSAConvertKind::ReferenceCast) {
            const ca::str::Utf8StringRef cacheName = addClassCache(state, ins.referenceTypeDesc);
            const std::string         destName = regName(ins.dest.id);
            out.appendLineStart(destName)
                .append(" = _mj2x_regc_checkcast_cached(")
                .append(operandExpr(ins.value))
                .append(", &")
                .append(cacheName)
                .append(");")
                .newLine();
        }
        else {
            const std::string destName = regName(ins.dest.id);
            // 浮点→整型走 CRT helper（#1027 D2）：C 裸 cast 对 NaN/越界是 UB，
            // JVM f2i/f2l/d2i/d2l 语义（JLS 5.1.3）是 NaN→0、向零截断、超界饱和；
            // StackC（内联饱和块）/VMC4（mj2x_vm_f2i*）/LLVM（fptosi.sat）同语义。
            const SSAType sourceType = ins.value.type;
            const bool    sourceFloat = sourceType == SSAType::F32 || sourceType == SSAType::F64;
            const bool    targetInt   = ins.targetType == SSAType::I32 ||
                                     ins.targetType == SSAType::I64;
            if (sourceFloat && targetInt) {
                out.assign(destName,
                           codegen::exprCall(
                               ins.targetType == SSAType::I32 ? "mj2x_regc_f2i32"
                                                              : "mj2x_regc_f2i64",
                               {codegen::exprCast("double",
                                                  operandExpr(ins.value).to_std_string())}));
            }
            else {
                out.assign(destName, codegen::exprCast(cType(ins.targetType),
                                                       codegen::exprParen(
                                                           operandExpr(ins.value).to_std_string())));
            }
        }
        return true;
    case RegInstructionKind::LoadField:
        if (emitPodStructFieldLoad(out, state, ins)) return true;
        if (!rejectNativeObjectEscape(state, ins.receiver, "field load receiver", reason))
            return false;
        if (ins.field.isStatic) {
            const ca::str::Utf8StringRef cacheName = addStaticFieldCache(state, ins);
            const std::string         destName = regName(ins.dest.id);
            out.appendLineStart(destName)
                .append(" = _mj2x_regc_load_cached_")
                .append(fieldHelperSuffix(ins.field.desc, ins.dest.type))
                .append("_field(&")
                .append(cacheName)
                .append(");")
                .newLine();
        }
        else {
            const ca::str::Utf8StringRef cacheName = addInstanceFieldCache(state, ins);
            const std::string         destName = regName(ins.dest.id);
            out.appendLineStart(destName).append(" = _mj2x_regc_load_");
            if (!cacheName.is_empty()) {
                out.append("cached_instance_")
                    .append(fieldHelperSuffix(ins.field.desc, ins.dest.type))
                    .append("_field(&")
                    .append(cacheName)
                    .append(", ")
                    .append(operandExpr(ins.receiver))
                    .append(");")
                    .newLine();
            }
            else {
                out.append(fieldHelperSuffix(ins.field.desc, ins.dest.type))
                    .append("_field(0, ")
                    .append(operandExpr(ins.receiver))
                    .append(", ")
                    .append(cStringLiteral(ins.field.owner))
                    .append(", ")
                    .append(cStringLiteral(ins.field.name))
                    .append(", ")
                    .append(cStringLiteral(ins.field.desc))
                    .append(");")
                    .newLine();
            }
        }
        return true;
    case RegInstructionKind::StoreField:
        if (emitPodStructFieldStore(out, state, ins, reason)) return true;
        if (!rejectNativeObjectEscape(state, ins.receiver, "field store receiver", reason))
            return false;
        {
            ca::str::Utf8String valueExpression;
            const bool          materialized =
                emitMaterializedStringBuilder(out, state, ins.value, valueExpression);
            if (!materialized &&
                !rejectNativeObjectEscape(state, ins.value, "field store value", reason)) {
                return false;
            }
            const auto emitStore = [&](CSourceStream& u) {
                if (ins.field.isStatic) {
                    const ca::str::Utf8StringRef cacheName = addStaticFieldCache(state, ins);
                    u.appendLineStart("_mj2x_regc_store_cached_")
                        .append(fieldHelperSuffix(ins.field.desc, ins.value.type))
                        .append("_field(&")
                        .append(cacheName)
                        .append(", ")
                        .append(valueExpression)
                        .append(");")
                        .newLine();
                }
                else {
                    const ca::str::Utf8StringRef cacheName = addInstanceFieldCache(state, ins);
                    u.appendLineStart("_mj2x_regc_store_");
                    if (!cacheName.is_empty()) {
                        u.append("cached_instance_")
                            .append(fieldHelperSuffix(ins.field.desc, ins.value.type))
                            .append("_field(&")
                            .append(cacheName)
                            .append(", ")
                            .append(operandExpr(ins.receiver))
                            .append(", ")
                            .append(valueExpression)
                            .append(");")
                            .newLine();
                    }
                    else {
                        u.append(fieldHelperSuffix(ins.field.desc, ins.value.type))
                            .append("_field(0, ")
                            .append(operandExpr(ins.receiver))
                            .append(", ")
                            .append(cStringLiteral(ins.field.owner))
                            .append(", ")
                            .append(cStringLiteral(ins.field.name))
                            .append(", ")
                            .append(cStringLiteral(ins.field.desc))
                            .append(", ")
                            .append(valueExpression)
                            .append(");")
                            .newLine();
                    }
                }
            };
            if (materialized) out.iff("!_mj2x_regc_exception_check()", emitStore);
            else emitStore(out);
            return true;
        }
    case RegInstructionKind::ArrayLoad:
        if (!rejectNativeObjectEscape(state, ins.receiver, "array load receiver", reason))
            return false;
    {
        const std::string destName = regName(ins.dest.id);
        out.appendLineStart(destName)
            .append(" = _mj2x_regc_load_")
            .append(arrayHelperSuffix(ins.elementType))
            .append("_array(")
            .append(operandExpr(ins.receiver))
            .append(", ")
            .append(operandExpr(ins.index))
            .append(");")
            .newLine();
        return true;
    }
    case RegInstructionKind::ArrayStore:
        if (!rejectNativeObjectEscape(state, ins.receiver, "array store receiver", reason))
            return false;
        {
            ca::str::Utf8String valueExpression;
            const bool          materialized =
                emitMaterializedStringBuilder(out, state, ins.value, valueExpression);
            if (!materialized &&
                !rejectNativeObjectEscape(state, ins.value, "array store value", reason)) {
                return false;
            }
            const auto emitStore = [&](CSourceStream& u) {
                u.appendLineStart("_mj2x_regc_store_")
                    .append(arrayHelperSuffix(ins.elementType))
                    .append("_array(")
                    .append(operandExpr(ins.receiver))
                    .append(", ")
                    .append(operandExpr(ins.index))
                    .append(", ")
                    .append(valueExpression)
                    .append(");")
                    .newLine();
            };
            if (materialized) out.iff("!_mj2x_regc_exception_check()", emitStore);
            else emitStore(out);
            return true;
        }
    case RegInstructionKind::Call:
    {
        // POD struct 的构造器调用整条跳过：struct 已由 `= {0}` 零初始化，构造器
        // 体的字段赋值也走 struct 成员路径。不跳过的话 receiver 是 native 对象，
        // 会被 rejectNativeObjectEscape 判为逃逸、整函数 fail-closed。
        if (!ins.args.empty() && ins.method.name == "<init>" &&
            podStructStorageFor(state, ins.args[0]) != nullptr) {
            if (state.commentLevel != codegen::RegCCommentLevel::None) {
                out.appendLineStart("/* POD struct construction elided: ")
                    .append(ins.method.owner)
                    .append(".<init> */")
                    .newLine();
            }
            if (ins.hasDest) out.assign(regName(ins.dest.id), defaultValue(ins.dest.type));
            return true;
        }
        // native-to-native 直接调用优先（确定性分派 + 已转换目标），未命中再走
        // intrinsic/通用 JNI 路径。
        if (tryEmitDirectCall(out, ins, state, state.classIndex, state.convertedFunctions,
                              state.indyFunctions)) {
            return true;
        }
        // JDK intrinsic recognition goes through the registry so fallback
        // and unsupported reasons remain explicit as coverage grows.
        {
            RegCIntrinsicDecision decision = emitIntrinsicCall(out, ins, state);
            if (decision.kind == RegCIntrinsicDecisionKind::Emit) return true;
            if (decision.kind == RegCIntrinsicDecisionKind::Unsupported) {
                reason = decision.reason;
                return false;
            }
        }
        std::vector<ca::str::Utf8String> argumentExpressions;
        argumentExpressions.reserve(ins.args.size());
        bool hasMaterializedArgument = false;
        for (const auto& arg : ins.args) {
            ca::str::Utf8String expression;
            const bool          materialized =
                emitMaterializedStringBuilder(out, state, arg, expression);
            if (!materialized && nativeObjectFor(state, arg) != nullptr) {
                reason = "native object escaped through generic call " +
                         ins.method.owner.to_std_string() + "." + ins.method.name.to_std_string();
                return false;
            }
            hasMaterializedArgument = hasMaterializedArgument || materialized;
            argumentExpressions.push_back(std::move(expression));
        }
        const bool              staticCall = isStaticCall(ins);
        const bool              selfCall   = isSelfStaticCall(state, ins);
        // 自递归直调：Off 档下走 cached JNI，需要 static method cache（原逻辑
        // selfCall 无 cache）。自递归必同类，IntraClass 档起即直调。
        const bool              selfCallDirect =
            allowsDirectCall(state.directCallTier, true, false);
        const bool              selfCallJni = selfCall && !selfCallDirect;
        const ca::str::Utf8StringRef cacheName =
            (staticCall && (!selfCall || selfCallJni)) ? addStaticMethodCache(state, ins)
                                                       : ca::str::Utf8StringRef{};
        const ca::str::Utf8StringRef instanceCacheName =
            (!staticCall && !selfCall) ? addInstanceMethodCache(state, ins)
                                       : ca::str::Utf8StringRef{};
        const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();
        const bool dynamicInstance = !staticCall && !selfCall && !instanceCacheName.is_empty();
        const bool privateThis =
            dynamicInstance && isPrivateThisCall(state, ins, fn) && !state.traceCalls;
        // JDK/库类调用点：方法 owner 不在编译输入（isLibrary），receiver 不可能是
        // 已转换类 → resolved 的 GetObjectClass + IsSameObject 类检查是纯开销
        // （2 JNI/调用）。cached instance 调用（GetMethodID 缓存 + CallXxxMethodA）
        // 对任意 receiver 语义正确（JNI 虚分派按实际对象类找覆写），直接走 cached。
        const bool libraryOwner = [&]() {
            if (state.classIndex == nullptr) return false;
            CompilerClass* cls = state.classIndex->getClass(ins.method.owner.to_std_string());
            // 不在编译索引（JDK/第三方，index 通常只含编译输入）或标记为库的类：
            // receiver 不可能是已转换类 → cached（owner 解析）语义正确且省 JNI。
            return cls == nullptr || cls->isLibrary;
        }();
        const bool useResolved = dynamicInstance && state.resolvedDispatch && !state.traceCalls &&
                                 !privateThis && !libraryOwner &&
                                 // resolved 分派按 receiver 实际类解析 + 虚调用，
                                 // INVOKESPECIAL 需要 owner 声明 + CallNonvirtual。
                                 ins.method.invokeOpcode != Opcodes::INVOKESPECIAL;
        const bool inlineCached =
            !selfCall && !useResolved && !privateThis && (staticCall || !instanceCacheName.is_empty()) &&
            (!(state.traceCalls && !staticCall && !ins.hasDest) ||
             // INVOKESPECIAL 一律走内联 cached 路径（emitCachedCall 的
             // CallNonvirtual 发射）；va-arg 家族只做虚分派，不接手 special。
             (!staticCall && ins.method.invokeOpcode == Opcodes::INVOKESPECIAL));
        const auto emitCall = [&](CSourceStream& u) {
            if (useResolved) {
                // receiver 精确类型静态绑定优先：receiver 的 SSA def 可证为
                // 单一分配点类且目标方法已转换时，跳过运行时类检查直接调用
                // regc 函数体（0 JNI，替代双槽 IsInstanceOf 的 1.5-2 JNI）。
                if (tryEmitStaticBoundCall(u, ins, state, fn, block, argumentExpressions)) {
                    return;
                }
                emitResolvedInstanceCall(u,
                                         ins,
                                         state.cachePool->resolvedCallSite(ins.method.owner,
                                                                           ins.method.name,
                                                                           ins.method.desc),
                                         argumentExpressions,
                                         state.hasLocalFrame);
            }
            else if (privateThis) {
                // 私有方法 + this：无覆写、精确分派。已转换实现 → 直接调用
                // regc 函数（0 JNI，参数 = this + 实参）；未转换 → cached
                // instance 调用（省 resolved 的 GetObjectClass + IsSameObject）。
                // convertedFunctions 可为空（tryEmitDirectCall /
                // tryEmitStaticBoundCall 同样判空），空时走 cached 调用。
                const std::string key = methodKey(ins.method.owner.to_std_string(),
                                                  ins.method.name.to_std_string(),
                                                  ins.method.desc.to_std_string());
                bool        directAvailable = false;
                std::string directName;
                if (state.convertedFunctions != nullptr) {
                    const auto direct = state.convertedFunctions->find(key);
                    if (direct != state.convertedFunctions->end()) {
                        directAvailable = true;
                        directName      = direct->second;
                    }
                }
                // privateThis 的 receiver 就是 this，目标 owner 即当前类——
                // 栈失真限制在类内，IntraClass 档起允许直调。
                const bool sameOwner = ins.method.owner == state.symbol.owner;
                bool       safeDirect = allowsDirectCall(state.directCallTier, sameOwner, false);
                if (state.indyFunctions != nullptr &&
                    state.indyFunctions->count(key) != 0) {
                    safeDirect = false;
                }
                for (const auto& arg : ins.args) {
                    if (nativeObjectFor(state, arg) != nullptr) {
                        safeDirect = false;
                        break;
                    }
                }
                if (safeDirect && directAvailable) {
                    // 前向声明：目标函数定义可能在本调用点之后（同类方法转换
                    // 顺序不保证），无声明时 C 隐式 int() 声明与 REF 返回值冲突
                    // （C2040）。声明登记到 TU 头部注册表——块作用域 static
                    // 函数声明违反 C11 6.7.1p6（GCC 拒绝，MSVC 放行），见
                    // declareDirectCallTarget。
                    declareDirectCallTarget(state, u, ins, directName);
                    if (ins.hasDest) u.appendLineStart(destName).append(" = ");
                    u.append(directName)
                        .append("(")
                        .append(directCallArgs(ins.args, argumentExpressions))
                        .append(");")
                        .newLine();
                }
                else {
                    // private 无覆写：按声明 owner 解析即可（免每调用 GetObjectClass）。
                    emitCachedCall(u, state, ins, false, instanceCacheName, argumentExpressions,
                                   true);
                }
            }
            else if (inlineCached) {
                emitCachedCall(u, state, ins, staticCall,
                               staticCall ? cacheName : instanceCacheName,
                               argumentExpressions, libraryOwner && !staticCall);
            }
            else if (ins.hasDest) {
                u.appendLineStart(destName).append(" = ");
                if (selfCall && selfCallDirect) {
                    u.append(state.functionName)
                        .append("(")
                        .append(directCallArgs(ins.args, argumentExpressions))
                        .append(");")
                        .newLine();
                }
                else {
                    u.append("_mj2x_regc_call_");
                    if (staticCall) {
                        u.append("cached_")
                            .append(helperSuffix(ins.dest.type))
                            .append("(&")
                            .append(cacheName)
                            .append(", ")
                            .append(callArgs(ins.args, argumentExpressions))
                            .append(");")
                            .newLine();
                    }
                    else if (!instanceCacheName.is_empty()) {
                        u.append("cached_instance_")
                            .append(helperSuffix(ins.dest.type))
                            .append("(&")
                            .append(instanceCacheName)
                            .append(", ")
                            .append(callArgs(ins.args, argumentExpressions))
                            .append(");")
                            .newLine();
                    }
                    else {
                        u.append(helperSuffix(ins.dest.type))
                            .append("(")
                            .append(cStringLiteral(ins.method.owner))
                            .append(", ")
                            .append(cStringLiteral(ins.method.name))
                            .append(", ")
                            .append(cStringLiteral(ins.method.desc))
                            .append(", 0, ")
                            .append(callArgs(ins.args, argumentExpressions))
                            .append(");")
                            .newLine();
                    }
                }
            }
            else {
                u.appendLineStart();
                if (selfCall && selfCallDirect) {
                    u.append(state.functionName)
                        .append("(")
                        .append(directCallArgs(ins.args, argumentExpressions))
                        .append(");")
                        .newLine();
                }
                else {
                    u.append("_mj2x_regc_call_");
                    if (staticCall) {
                        u.append("cached_void(&")
                            .append(cacheName)
                            .append(", ")
                            .append(callArgs(ins.args, argumentExpressions))
                            .append(");")
                            .newLine();
                    }
                    else if (!instanceCacheName.is_empty()) {
                        u.append("cached_instance_void(&")
                            .append(instanceCacheName)
                            .append(", ")
                            .append(callArgs(ins.args, argumentExpressions))
                            .append(");")
                            .newLine();
                    }
                    else {
                        u.append("void(")
                            .append(cStringLiteral(ins.method.owner))
                            .append(", ")
                            .append(cStringLiteral(ins.method.name))
                            .append(", ")
                            .append(cStringLiteral(ins.method.desc))
                            .append(", 0, ")
                            .append(callArgs(ins.args, argumentExpressions))
                            .append(");")
                            .newLine();
                    }
                }
            }
        };
        if (hasMaterializedArgument) out.iff("!_mj2x_regc_exception_check()", emitCall);
        else emitCall(out);
        return true;
    }
    case RegInstructionKind::Allocate:
        if (ins.allocationKind == SSAAllocationKind::Object) {
            if (ins.typeDesc == "java/util/Random") {
                out.appendLineStart("mj2x_random ")
                    .append(nativeObjectStorageName(ins.dest.id))
                    .append(" = {0};")
                    .newLine();
                const std::string destRandomName = regName(ins.dest.id);
                out.assign(destRandomName, "NULL");
                state.nativeObjects[ins.dest.id.value] = RegCNativeObjectState{
                    RegCIntrinsicNativeObjectKind::Random,
                    nativeObjectStorageName(ins.dest.id).to_std_string()};
            }
            else if ((ins.typeDesc == "java/lang/StringBuilder" ||
                      ins.typeDesc == "java/lang/StringBuffer") &&
                     state.nativeStringBuilderRegisters.count(ins.dest.id.value) != 0) {
                const auto nativeIt = state.nativeObjects.find(ins.dest.id.value);
                if (nativeIt == state.nativeObjects.end()) {
                    reason = "mutable string allocation is missing native state plan";
                    return false;
                }
                const std::string sbDestName = regName(ins.dest.id);
                out.assign(sbDestName, "(void*)&" + stringBuilderOwnerName(ins.dest.id).to_std_string());
                state.nativeObjects[ins.dest.id.value] =
                    RegCNativeObjectState{nativeIt->second.kind, sbDestName};
            }
            else if (!state.podStructRoots.empty() &&
                     isPodClassOwner(state.classIndex, ins.typeDesc)) {
                // POD struct 降级：对象不存在于 JVM 堆，改成一个 C struct 局部
                // 变量。REF 寄存器置 NULL——后续若有非字段使用，
                // rejectNativeObjectEscape 会把它挡回去（保守 fail-closed）。
                const std::string structTypeName = podStructTypeName(ins.typeDesc);
                // 变量已在函数作用域声明，这里只做零初始化 + 指针绑定（复合字面量）。
                out.appendLineStart(podStructValueName(ins.dest.id))
                    .append(" = (")
                    .append(structTypeName)
                    .append("){0};")
                    .newLine();
                out.appendLineStart(podStructPtrName(ins.dest.id))
                    .append(" = &")
                    .append(podStructValueName(ins.dest.id))
                    .append(";")
                    .newLine();
                const std::string destName = regName(ins.dest.id);
                out.assign(destName, "NULL");
                state.nativeObjects[ins.dest.id.value] =
                    RegCNativeObjectState{RegCIntrinsicNativeObjectKind::PodStruct,
                                         podStructPtrName(ins.dest.id), structTypeName};
            }
            else {
                const std::string allocDestName = regName(ins.dest.id);
                if (state.cachePool != nullptr) {
                    const ca::str::Utf8StringRef cacheName = addClassCache(state, ins.typeDesc);
                    out.appendLineStart(allocDestName)
                        .append(" = _mj2x_regc_alloc_object_cached(&")
                        .append(cacheName)
                        .append(");")
                        .newLine();
                }
                else {
                    out.appendLineStart(allocDestName)
                        .append(" = _mj2x_regc_alloc_object(")
                        .append(cStringLiteral(ins.typeDesc))
                        .append(");")
                        .newLine();
                }
            }
        }
        else if (ins.allocationKind == SSAAllocationKind::Array) {
            const RegOperand length =
                ins.args.empty() ? RegOperand::fromInt(0, SSAType::I32) : ins.args[0];
            const std::string allocArrayDest = regName(ins.dest.id);
            if (state.cachePool != nullptr && isReferenceArrayDesc(ins.typeDesc)) {
                const ca::str::Utf8StringRef cacheName = addClassCache(
                    state,
                    ca::str::Utf8StringRef::from_string_view(arrayComponentDesc(ins.typeDesc)));
                out.appendLineStart(allocArrayDest)
                    .append(" = _mj2x_regc_alloc_ref_array_cached(&")
                    .append(cacheName)
                    .append(", ")
                    .append(operandExpr(length))
                    .append(");")
                    .newLine();
            }
            else {
                out.appendLineStart(allocArrayDest)
                    .append(" = _mj2x_regc_alloc_array(")
                    .append(cStringLiteral(ins.typeDesc))
                    .append(", ")
                    .append(operandExpr(length))
                    .append(");")
                    .newLine();
            }
        }
        else {
            // MULTIANEWARRAY 至少一维；空维会发射 `int32_t dims[0]`（C 禁止
            // 零长数组）。上游 IR 应保证，发射端防御。
            if (ins.args.empty()) {
                reason = "multi-array allocation requires at least one dimension";
                return false;
            }
            out.block([&](CSourceStream& u) {
                u.appendLineStart("int32_t dims[")
                    .append(ca::str::format_std("{}", ins.args.size()))
                    .append("];")
                    .newLine();
                for (size_t i = 0; i < ins.args.size(); ++i) {
                    u.appendLineStart("dims[")
                        .append(ca::str::format_std("{}", i))
                        .append("] = ")
                        .append(operandExpr(ins.args[i]))
                        .append(";")
                        .newLine();
                }
                const std::vector<std::string> componentDescs =
                    state.cachePool != nullptr
                        ? multiArrayComponentDescs(ins.typeDesc, ins.args.size())
                        : std::vector<std::string>{};
                if (!componentDescs.empty()) {
                    u.appendLineStart("mj2x_regc_class_cache* component_caches[")
                        .append(ca::str::format_std("{}", componentDescs.size()))
                        .append("] = {");
                    for (size_t i = 0; i < componentDescs.size(); ++i) {
                        if (i != 0) u.append(", ");
                        const ca::str::Utf8StringRef cacheName = addClassCache(
                            state,
                            ca::str::Utf8StringRef::from_string_view(componentDescs[i]));
                        u.append("&").append(cacheName);
                    }
                    u.append("};").newLine();
                    u.appendLineStart(regName(ins.dest.id))
                        .append(" = _mj2x_regc_alloc_multi_array_cached(")
                        .append(cStringLiteral(ins.typeDesc))
                        .append(", ")
                        .append(ca::str::format_std("{}", ins.args.size()))
                        .append(", dims, component_caches);")
                        .newLine();
                }
                else {
                    u.appendLineStart(regName(ins.dest.id))
                        .append(" = _mj2x_regc_alloc_multi_array(")
                        .append(cStringLiteral(ins.typeDesc))
                        .append(", ")
                        .append(ca::str::format_std("{}", ins.args.size()))
                        .append(", dims);")
                        .newLine();
                }
            });
        }
        return true;
    case RegInstructionKind::Intrinsic:
        if (ins.intrinsicName.rfind("indy:", 0) == 0) {
            // 自定义 bootstrap invokedynamic（阶段3）：站点元数据在 RegFunction::indySites，
            // 方法内的局部索引在文件级表中偏移 indySiteBase_。方法体只负责收集参数并调用
            // 文件级 bootstrap helper（finalize 生成），返回目标 MethodHandle 后
            // invokeWithArguments 完成实际调用。
            size_t localIdx = 0;
            try {
                localIdx = static_cast<size_t>(std::stoull(ins.intrinsicName.substr(5)));
            }
            catch (const std::exception&) {
                reason = "invalid indy site index in intrinsic name";
                return false;
            }
            if (localIdx >= fn.indySites.size()) {
                reason = "indy site index out of range";
                return false;
            }
            return emitInvokeDynamic(
                out, ins, fn.indySites[localIdx], indySiteBase + localIdx, fn, block, reason);
        }
        if (ins.intrinsicName == "array.length") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "array.length",
                                          reason))
                return false;
            const std::string destName = regName(ins.dest.id);
            out.appendLineStart(destName)
                .append(" = _mj2x_regc_array_length(")
                .append(ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]))
                .append(");")
                .newLine();
            return true;
        }
        if (ins.intrinsicName.rfind("instanceof:", 0) == 0) {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "instanceof",
                                          reason))
                return false;
            const ca::str::Utf8StringRef cacheName = addClassCache(
                state, ca::str::Utf8StringRef::from_string_view(ins.intrinsicName.substr(11)));
            const std::string destName = regName(ins.dest.id);
            out.appendLineStart(destName)
                .append(" = _mj2x_regc_instanceof_cached(")
                .append(ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]))
                .append(", &")
                .append(cacheName)
                .append(");")
                .newLine();
            return true;
        }
        if (ins.intrinsicName == "monitor.enter") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "monitor.enter",
                                          reason))
                return false;
            // JVM 语义（JVMS monitorenter）：对象为 NULL 时抛 NPE。JNI
            // MonitorEnter/Exit 对 NULL jobject 行为未定义（HotSpot 按"调用方
            // 保证非空"处理），先守卫再调用——与 StackC emitMonitor /
            // VMC4 monitor intrinsic helper / LLVM mj2x_llvm_monitor_* 对齐。
            // 抛出后必须立即终止当前指令路径（JVM 在 monitor 指令抛异常时按
            // 当前 pc unwind，绝不继续执行后续指令）：未受保护块 goto cleanup
            // 直接传播；受保护块落入指令后的异常分发（路由 handler）。若
            // fall-through 继续执行，会踩进 sync-finally 的 monitorexit——javac
            // 对 finally handler 还会发 [handler,handlerEnd)→handler 的自覆盖
            // 异常表项，guard 再抛 → 再分发 → 自环死循环。
            {
                const bool blockProtected = !activeExceptionRegions(fn, block).empty();
                const ca::str::Utf8String monExpr =
                    ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]);
                out.iff(monExpr.to_std_string() + " == NULL",
                        [&](CSourceStream& u) {
                            u.emitLine("_mj2x_regc_throw_null(\"monitor\");");
                            if (!blockProtected) u.goto_("_mj2x_regc_cleanup");
                        },
                        [&](CSourceStream& u) {
                            u.appendLineStart("_mj2x_regc_monitor_enter(")
                                .append(monExpr)
                                .append(");")
                                .newLine();
                        });
            }
            return true;
        }
        if (ins.intrinsicName == "monitor.exit") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "monitor.exit",
                                          reason))
                return false;
            {
                const bool blockProtected = !activeExceptionRegions(fn, block).empty();
                const ca::str::Utf8String monExpr =
                    ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]);
                out.iff(monExpr.to_std_string() + " == NULL",
                        [&](CSourceStream& u) {
                            u.emitLine("_mj2x_regc_throw_null(\"monitor\");");
                            if (!blockProtected) u.goto_("_mj2x_regc_cleanup");
                        },
                        [&](CSourceStream& u) {
                            u.appendLineStart("_mj2x_regc_monitor_exit(")
                                .append(monExpr)
                                .append(");")
                                .newLine();
                        });
            }
            return true;
        }
        // JVM 的 LCMP/FCMPG/FCMPL/DCMPG/DCMPL 三值比较（-1/0/1），由 ssa_builder 的
        // lowerCmp 翻译为 cmp.nan_greater（FCMPG/DCMPG）或 cmp.nan_less（LCMP/FCMPL/DCMPL）。
        // 操作数类型来自 args[0]：浮点（F32/F64）需 NaN-aware 语义，整型（I64，LCMP）无 NaN。
        if (ins.intrinsicName == "cmp.nan_greater" || ins.intrinsicName == "cmp.nan_less") {
            if (ins.args.size() < 2) {
                reason = "cmp.nan_* intrinsic requires two operands";
                return false;
            }
            const ca::str::Utf8String lhs = operandExpr(ins.args[0]);
            const ca::str::Utf8String rhs = operandExpr(ins.args[1]);
            const SSAType             type = ins.args[0].type;
            const bool nanGreater = ins.intrinsicName == "cmp.nan_greater";
            // (lhs > rhs) - (lhs < rhs) 产出三值 -1/0/1，两种类型通用。
            CSourceStream threeWayBuilder;
            threeWayBuilder.append("((");
            threeWayBuilder.append(lhs);
            threeWayBuilder.append(" > ");
            threeWayBuilder.append(rhs);
            threeWayBuilder.append(") - (");
            threeWayBuilder.append(lhs);
            threeWayBuilder.append(" < ");
            threeWayBuilder.append(rhs);
            threeWayBuilder.append("))");
            const ca::str::Utf8String threeWay = threeWayBuilder.build();
            const std::string         destName = regName(ins.dest.id);
            if (ssaIsFloat(type)) {
                // 浮点：任一为 NaN 时按 nanGreater 取 1（dcmpg/fcmpg）或 -1（dcmpl/fcmpl）。
                const char* nanValue = nanGreater ? "1" : "-1";
                out.assign(destName,
                           codegen::exprTernary(
                               codegen::exprParen(codegen::exprBinop(
                                   "||",
                                   codegen::exprCall("isnan", {lhs.to_std_string()}),
                                   codegen::exprCall("isnan", {rhs.to_std_string()}))),
                               nanValue, threeWay.to_std_string()));
            }
            else {
                // 整型（LCMP）：long 不可能为 NaN，直接三值比较。
                out.assign(destName, threeWay.to_std_string());
            }
            return true;
        }
        if (ins.intrinsicName.rfind(mj2x::kConstKeyIntrinsicPrefix, 0) == 0) {
            // 立即数隐藏 per-site 密钥（对标 jvmtp obfuscation.constants）：
            // RegIR 合成 Intrinsic → CRT 不透明调用（独立 TU + volatile 读，
            // LTO 不可折叠）。site 解析与 indy: 同款纪律（stoull + 范围外拒收）。
            size_t site = 0;
            try {
                site = static_cast<size_t>(std::stoull(ins.intrinsicName.substr(10)));
            }
            catch (const std::exception&) {
                reason = "invalid const.key site index in intrinsic name";
                return false;
            }
            if (!ins.hasDest || ins.dest.type != SSAType::I64 || !ins.args.empty()) {
                reason = "const.key intrinsic must produce one i64 value with no operands";
                return false;
            }
            out.appendLineStart(regName(ins.dest.id))
                .append(" = mj2x_regc_const_key(")
                .append(ca::str::format_std("{}", site))
                .append(");")
                .newLine();
            return true;
        }
        reason = "unsupported RegC intrinsic: " + ins.intrinsicName;
        return false;
    case RegInstructionKind::Barrier:
        out.appendLineStart("mj2x_regc_barrier(_mj2x_regc_current_context(), ")
            .append(cStringLiteral(ins.barrierReason))
            .append(");")
            .newLine();
        return true;
    default: break;
    }

    reason = "unreachable RegC instruction path";
    return false;
}

bool emitTerminator(CSourceStream& out, const RegTerminator& terminator,
                    const RegBlock* block, SSAType returnType,
                    const std::vector<const RegExceptionRegion*>& activeRegions,
                    RegCFunctionEmitState& state, int& dispatchIndex, int& edgeTempIndex,
                    std::string& reason)
{
    return std::visit(
        [&](const auto& t) -> bool {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                bool copiesOk = true;
                out.block([&](CSourceStream& u) {
                    if (!emitEdgeCopies(u, t.target.copies, state, edgeTempIndex, reason)) {
                        copiesOk = false;
                        return;
                    }
                    u.goto_(ca::str::format_std("block_b{}", t.target.block->id.value));
                });
                return copiesOk;
            }
            else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                if (!rejectNativeObjectEscape(state, t.condition, "branch condition", reason))
                    return false;
                bool copiesOk = true;
                out.iff(operandExpr(t.condition).to_std_string(),
                        [&](CSourceStream& u) {
                            if (!emitEdgeCopies(u, t.trueTarget.copies, state, edgeTempIndex,
                                                reason)) {
                                copiesOk = false;
                                return;
                            }
                            u.goto_(ca::str::format_std("block_b{}", t.trueTarget.block->id.value));
                        },
                        [&](CSourceStream& u) {
                            if (!emitEdgeCopies(u, t.falseTarget.copies, state, edgeTempIndex,
                                                reason)) {
                                copiesOk = false;
                                return;
                            }
                            u.goto_(ca::str::format_std("block_b{}", t.falseTarget.block->id.value));
                        });
                return copiesOk;
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                if (!rejectNativeObjectEscape(state, t.value, "switch value", reason)) return false;
                bool copiesOk = true;
                // computed jumps（--regc-computed-jumps）：key -> 目标块的映射
                // 挪进数据表，分派改走运行时解析出的块索引。产物里不再有
                // `case <字面量>: goto block_bX` 这种直接可读的映射。
                std::string cjIndex;
                if (state.computedJumps && t.cases.size() >= 2) {
                    // 拿不到 TU 头部注册表（key 表无处安放）时退回字面量 case，
                    // 不整方法 fail-closed——本项是混淆增益，不该让方法编译不了。
                    emitComputedJumpDispatch(out, state, t, cjIndex, reason);
                }
                const bool         useIndex = !cjIndex.empty();
                const std::string  dispatchExpr =
                    useIndex ? cjIndex : operandExpr(t.value).to_std_string();
                // 走索引时分派的 case 标签是 0..N-1（N = case 数），default 兜底。
                const auto caseLabel = [&](size_t i, int32_t key) {
                    return useIndex ? ca::str::format_std("{}", i)
                                    : ca::str::format_std("{}", key);
                };
                out.switch_(dispatchExpr, [&](CSourceStream& u) {
                    for (size_t i = 0; i < t.cases.size(); ++i) {
                        const auto& c = t.cases[i];
                        u.case_(caseLabel(i, c.key), [&](CSourceStream& v) {
                            if (!emitEdgeCopies(v, c.target.copies, state, edgeTempIndex, reason)) {
                                copiesOk = false;
                                return;
                            }
                            v.goto_(ca::str::format_std("block_b{}", c.target.block->id.value));
                        });
                        if (!copiesOk) return;
                    }
                    u.case_("default", [&](CSourceStream& v) {
                        if (!emitEdgeCopies(v, t.defaultTarget.copies, state, edgeTempIndex,
                                            reason)) {
                            copiesOk = false;
                            return;
                        }
                        v.goto_(ca::str::format_std("block_b{}", t.defaultTarget.block->id.value));
                    });
                });
                return copiesOk;
            }
            else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                if (t.hasValue) {
                    const RegCNativeObjectState* objectState = nativeObjectFor(state, t.value);
                    if (objectState != nullptr && isMutableStringKind(objectState->kind)) {
                        ca::str::Utf8String materializedExpression;
                        emitMaterializedStringBuilder(out, state, t.value, materializedExpression);
                        if (!emitExceptionDispatch(out, activeRegions, block, returnType, state,
                                                   dispatchIndex, edgeTempIndex, reason))
                            return false;
                        out.assign("_mj2x_regc_result", materializedExpression.to_std_string());
                    }
                    else {
                        if (objectState != nullptr) {
                            reason = "native object escaped through method return";
                            return false;
                        }
                        out.assign("_mj2x_regc_result", operandExpr(t.value).to_std_string());
                    }
                }
                out.goto_("_mj2x_regc_cleanup");
                return true;
            }
            else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                if (!rejectNativeObjectEscape(state, t.value, "throw", reason)) return false;
                out.call("_mj2x_regc_throw", {operandExpr(t.value).to_std_string()});
                if (!emitExceptionDispatch(out, activeRegions, block, returnType, state,
                                           dispatchIndex, edgeTempIndex, reason))
                    return false;
                if (returnType != SSAType::VOID) {
                    out.assign("_mj2x_regc_result", defaultValue(returnType));
                }
                out.goto_("_mj2x_regc_cleanup");
                return true;
            }
            else if constexpr (std::is_same_v<T, RegUnreachableTerminator>) {
                if (returnType != SSAType::VOID) {
                    out.assign("_mj2x_regc_result", defaultValue(returnType));
                }
                out.goto_("_mj2x_regc_cleanup");
                return true;
            }
            else {
                reason = "unknown RegIR terminator";
                return false;
            }
        },
        terminator);
}

}   // namespace mj2x
