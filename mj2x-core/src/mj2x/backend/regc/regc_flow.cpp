/// @file regc_flow.cpp
/// @brief RegC 发射器指令流发射（regc_emitter_internal.hpp 的内部接口）。
///
/// @details
/// 寄存器收集（collectRegister/collectRegisters）、指令支持性检查、指令/终结符/
/// 异常分发/边拷贝发射（emitInstruction/emitTerminator/emitExceptionDispatch/
/// emitEdgeCopies）与函数级 helper prelude。从原 regc_emitter.cpp 纯搬移，
/// 生成代码零差异。
#include "regc_emitter_internal.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/codegen/c_expr.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"

namespace mj2x {

using namespace jbytecode::core;

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
    if (ins.binaryOp == SSABinaryOp::UShr) {
        CSourceStream os;
        os.append("(");
        os.append(cType(type));
        os.append(")((");
        os.append(unsignedCType(type));
        os.append(")");
        os.append(lhs);
        os.append(" >> ");
        os.append(rhs);
        os.append(")");
        return os.build();
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
    std::vector<int> tempIds;
    tempIds.reserve(copies.size());
    for (size_t i = 0; i < copies.size(); ++i) {
        const RegCNativeObjectState* objectState = nativeObjectFor(state, copies[i].source);
        if (objectState != nullptr && !isMutableStringKind(objectState->kind)) {
            reason = "native object escaped through CFG edge copy";
            return false;
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
            .appendInt(tempId)
            .append(" = ")
            .append(codegen::exprCast(cType(copies[i].dest.type),
                                      codegen::exprParen(operandExpr(copies[i].source).to_std_string())))
            .append(";")
            .newLine();
    }
    for (size_t i = 0; i < copies.size(); ++i) {
        const std::string destName = regName(copies[i].dest.id);
        out.assign(destName, "edge_tmp_" + std::to_string(tempIds[i]));
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

void emitHelperPrelude(ca::str::Utf8StringBuilder& out)
{
    static const char* const kRegCHelperPreludeDeclarations[] = {
        "static mj2x_regc_context* _mj2x_regc_current_context(void);\n",
        "static float _mj2x_regc_f32_from_bits(uint32_t bits);\n",
        "static double _mj2x_regc_f64_from_bits(uint64_t bits);\n",
        "static mj2x_ref _mj2x_regc_alloc_object(const char* type_desc);\n",
        "static mj2x_ref _mj2x_regc_alloc_object_cached(mj2x_regc_class_cache* cache);\n",
        "static mj2x_ref _mj2x_regc_alloc_array(const char* type_desc, int32_t length);\n",
        "static mj2x_ref _mj2x_regc_alloc_ref_array_cached(mj2x_regc_class_cache* "
        "component_cache, int32_t length);\n",
        "static mj2x_ref _mj2x_regc_alloc_multi_array(const char* type_desc, int32_t dim_count, "
        "int32_t* dims);\n",
        "static int32_t _mj2x_regc_array_length(mj2x_ref array_ref);\n",
        "static mj2x_ref _mj2x_regc_checkcast(mj2x_ref value, const char* type_desc);\n",
        "static mj2x_ref _mj2x_regc_checkcast_cached(mj2x_ref value, "
        "mj2x_regc_class_cache* cache);\n",
        "static int32_t _mj2x_regc_instanceof(mj2x_ref value, const char* type_desc);\n",
        "static int32_t _mj2x_regc_instanceof_cached(mj2x_ref value, "
        "mj2x_regc_class_cache* cache);\n",
        "static void _mj2x_regc_monitor_enter(mj2x_ref value);\n",
        "static void _mj2x_regc_monitor_exit(mj2x_ref value);\n",
        "static int32_t _mj2x_regc_exception_check(void);\n",
        "static mj2x_ref _mj2x_regc_exception_occurred(void);\n",
        "static void _mj2x_regc_exception_clear(void);\n",
        "static int32_t _mj2x_regc_exception_matches(mj2x_ref value, const char* catch_type);\n",
        "static void _mj2x_regc_throw(mj2x_ref value);\n",
        "static bool _mj2x_regc_load_bool_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static int8_t _mj2x_regc_load_i8_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static uint16_t _mj2x_regc_load_char_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static int16_t _mj2x_regc_load_i16_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static int32_t _mj2x_regc_load_i32_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static int64_t _mj2x_regc_load_i64_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static float _mj2x_regc_load_f32_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static double _mj2x_regc_load_f64_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static mj2x_ref _mj2x_regc_load_ref_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc);\n",
        "static bool _mj2x_regc_load_cached_bool_field(mj2x_regc_static_field_cache* cache);\n",
        "static int8_t _mj2x_regc_load_cached_i8_field(mj2x_regc_static_field_cache* cache);\n",
        "static uint16_t _mj2x_regc_load_cached_char_field(mj2x_regc_static_field_cache* cache);\n",
        "static int16_t _mj2x_regc_load_cached_i16_field(mj2x_regc_static_field_cache* cache);\n",
        "static int32_t _mj2x_regc_load_cached_i32_field(mj2x_regc_static_field_cache* cache);\n",
        "static int64_t _mj2x_regc_load_cached_i64_field(mj2x_regc_static_field_cache* cache);\n",
        "static float _mj2x_regc_load_cached_f32_field(mj2x_regc_static_field_cache* cache);\n",
        "static double _mj2x_regc_load_cached_f64_field(mj2x_regc_static_field_cache* cache);\n",
        "static mj2x_ref _mj2x_regc_load_cached_ref_field(mj2x_regc_static_field_cache* cache);\n",
        "static bool _mj2x_regc_load_cached_instance_bool_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static int8_t _mj2x_regc_load_cached_instance_i8_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static uint16_t _mj2x_regc_load_cached_instance_char_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static int16_t _mj2x_regc_load_cached_instance_i16_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static int32_t _mj2x_regc_load_cached_instance_i32_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static int64_t _mj2x_regc_load_cached_instance_i64_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static float _mj2x_regc_load_cached_instance_f32_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static double _mj2x_regc_load_cached_instance_f64_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static mj2x_ref _mj2x_regc_load_cached_instance_ref_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver);\n",
        "static void _mj2x_regc_store_bool_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, bool value);\n",
        "static void _mj2x_regc_store_i8_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, int8_t value);\n",
        "static void _mj2x_regc_store_char_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, uint16_t value);\n",
        "static void _mj2x_regc_store_i16_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, int16_t value);\n",
        "static void _mj2x_regc_store_i32_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, int32_t value);\n",
        "static void _mj2x_regc_store_i64_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, int64_t value);\n",
        "static void _mj2x_regc_store_f32_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, float value);\n",
        "static void _mj2x_regc_store_f64_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, double value);\n",
        "static void _mj2x_regc_store_ref_field(int32_t is_static, mj2x_ref receiver, const "
        "char* owner, const char* name, const char* desc, mj2x_ref value);\n",
        "static void _mj2x_regc_store_cached_bool_field(mj2x_regc_static_field_cache* cache, "
        "bool value);\n",
        "static void _mj2x_regc_store_cached_i8_field(mj2x_regc_static_field_cache* cache, "
        "int8_t value);\n",
        "static void _mj2x_regc_store_cached_char_field(mj2x_regc_static_field_cache* cache, "
        "uint16_t value);\n",
        "static void _mj2x_regc_store_cached_i16_field(mj2x_regc_static_field_cache* cache, "
        "int16_t value);\n",
        "static void _mj2x_regc_store_cached_i32_field(mj2x_regc_static_field_cache* cache, "
        "int32_t value);\n",
        "static void _mj2x_regc_store_cached_i64_field(mj2x_regc_static_field_cache* cache, "
        "int64_t value);\n",
        "static void _mj2x_regc_store_cached_f32_field(mj2x_regc_static_field_cache* cache, "
        "float value);\n",
        "static void _mj2x_regc_store_cached_f64_field(mj2x_regc_static_field_cache* cache, "
        "double value);\n",
        "static void _mj2x_regc_store_cached_ref_field(mj2x_regc_static_field_cache* cache, "
        "mj2x_ref value);\n",
        "static void _mj2x_regc_store_cached_instance_bool_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, bool value);\n",
        "static void _mj2x_regc_store_cached_instance_i8_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int8_t value);\n",
        "static void _mj2x_regc_store_cached_instance_char_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, uint16_t value);\n",
        "static void _mj2x_regc_store_cached_instance_i16_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int16_t value);\n",
        "static void _mj2x_regc_store_cached_instance_i32_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int32_t value);\n",
        "static void _mj2x_regc_store_cached_instance_i64_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, int64_t value);\n",
        "static void _mj2x_regc_store_cached_instance_f32_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, float value);\n",
        "static void _mj2x_regc_store_cached_instance_f64_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, double value);\n",
        "static void _mj2x_regc_store_cached_instance_ref_field("
        "mj2x_regc_instance_field_cache* cache, mj2x_ref receiver, mj2x_ref value);\n",
        "static int8_t _mj2x_regc_load_i8_array(mj2x_ref array_ref, int32_t index);\n",
        "static int16_t _mj2x_regc_load_i16_array(mj2x_ref array_ref, int32_t index);\n",
        "static uint16_t _mj2x_regc_load_char_array(mj2x_ref array_ref, int32_t index);\n",
        "static int32_t _mj2x_regc_load_i32_array(mj2x_ref array_ref, int32_t index);\n",
        "static int64_t _mj2x_regc_load_i64_array(mj2x_ref array_ref, int32_t index);\n",
        "static float _mj2x_regc_load_f32_array(mj2x_ref array_ref, int32_t index);\n",
        "static double _mj2x_regc_load_f64_array(mj2x_ref array_ref, int32_t index);\n",
        "static mj2x_ref _mj2x_regc_load_ref_array(mj2x_ref array_ref, int32_t index);\n",
        "static void _mj2x_regc_store_i8_array(mj2x_ref array_ref, int32_t index, int8_t "
        "value);\n",
        "static void _mj2x_regc_store_i16_array(mj2x_ref array_ref, int32_t index, int16_t "
        "value);\n",
        "static void _mj2x_regc_store_char_array(mj2x_ref array_ref, int32_t index, uint16_t "
        "value);\n",
        "static void _mj2x_regc_store_i32_array(mj2x_ref array_ref, int32_t index, int32_t "
        "value);\n",
        "static void _mj2x_regc_store_i64_array(mj2x_ref array_ref, int32_t index, int64_t "
        "value);\n",
        "static void _mj2x_regc_store_f32_array(mj2x_ref array_ref, int32_t index, float "
        "value);\n",
        "static void _mj2x_regc_store_f64_array(mj2x_ref array_ref, int32_t index, double "
        "value);\n",
        "static void _mj2x_regc_store_ref_array(mj2x_ref array_ref, int32_t index, mj2x_ref "
        "value);\n",
        "static int32_t _mj2x_regc_call_i32(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static int64_t _mj2x_regc_call_i64(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static float _mj2x_regc_call_f32(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static double _mj2x_regc_call_f64(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static mj2x_ref _mj2x_regc_call_ref(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static void _mj2x_regc_call_void(const char* owner, const char* name, const char* "
        "desc, int32_t is_static, int32_t arg_count, ...);\n",
        "static int32_t _mj2x_regc_call_cached_i32(mj2x_regc_static_method_cache* cache, "
        "int32_t arg_count, ...);\n",
        "static int64_t _mj2x_regc_call_cached_i64(mj2x_regc_static_method_cache* cache, "
        "int32_t arg_count, ...);\n",
        "static float _mj2x_regc_call_cached_f32(mj2x_regc_static_method_cache* cache, int32_t "
        "arg_count, ...);\n",
        "static double _mj2x_regc_call_cached_f64(mj2x_regc_static_method_cache* cache, int32_t "
        "arg_count, ...);\n",
        "static mj2x_ref _mj2x_regc_call_cached_ref(mj2x_regc_static_method_cache* cache, "
        "int32_t arg_count, ...);\n",
        "static void _mj2x_regc_call_cached_void(mj2x_regc_static_method_cache* cache, int32_t "
        "arg_count, ...);\n",
        "static int32_t _mj2x_regc_call_cached_instance_i32("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static int64_t _mj2x_regc_call_cached_instance_i64("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static float _mj2x_regc_call_cached_instance_f32("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static double _mj2x_regc_call_cached_instance_f64("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static mj2x_ref _mj2x_regc_call_cached_instance_ref("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
        "static void _mj2x_regc_call_cached_instance_void("
        "mj2x_regc_instance_method_cache* cache, int32_t arg_count, ...);\n",
    };
    for (const char* declaration : kRegCHelperPreludeDeclarations) {
        out.append(declaration);
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
        "_mj2x_regc_load_ref_field",
        "_mj2x_regc_load_cached_ref_field",
        "_mj2x_regc_load_ref_array",
    };
    for (const char* marker : kMarkers) {
        if (body.contains(ca::str::Utf8StringRef::from_cstr(marker))) return true;
    }
    return false;
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
        u.declare_var("mj2x_ref", "ex_" + std::to_string(index),
                      "_mj2x_regc_exception_occurred()");
            u.call("_mj2x_regc_exception_clear", {});
        // Match in exception-table order. A matched handler receives the caught
        // exception through its synthetic exception register before the jump.
        for (const auto* region : regions) {
            u.iff("_mj2x_regc_exception_matches(ex_" + std::to_string(index) + ", " +
                      cStringLiteral(region->caughtType) + ")",
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
                               "ex_" + std::to_string(index));
                      v.goto_("block_b" + std::to_string(region->handler->id.value));
                  });
            if (!copiesOk) return;
        }
        // 若没有任何 handler 接受挂起的异常，保持 Java 传播语义。
        u.call("_mj2x_regc_throw", {"ex_" + std::to_string(index)});
        emitDefaultReturnAfterException(u, returnType);
    });
    return copiesOk;
}

bool emitInstruction(CSourceStream& out, const RegInstruction& ins,
                     RegCFunctionEmitState& state, const RegFunction& fn, size_t indySiteBase,
                     std::string& reason)
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
            const ca::str::Utf8String cacheName = addClassCache(state, ins.referenceTypeDesc);
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
            out.assign(destName, codegen::exprCast(cType(ins.targetType),
                                                   codegen::exprParen(
                                                       operandExpr(ins.value).to_std_string())));
        }
        return true;
    case RegInstructionKind::LoadField:
        if (!rejectNativeObjectEscape(state, ins.receiver, "field load receiver", reason))
            return false;
        if (ins.field.isStatic) {
            const ca::str::Utf8String cacheName = addStaticFieldCache(state, ins);
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
            const ca::str::Utf8String cacheName = addInstanceFieldCache(state, ins);
            const std::string         destName = regName(ins.dest.id);
            out.appendLineStart(destName).append(" = _mj2x_regc_load_");
            if (!cacheName.empty()) {
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
                    const ca::str::Utf8String cacheName = addStaticFieldCache(state, ins);
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
                    const ca::str::Utf8String cacheName = addInstanceFieldCache(state, ins);
                    u.appendLineStart("_mj2x_regc_store_");
                    if (!cacheName.empty()) {
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
        const ca::str::Utf8String cacheName =
            (staticCall && !selfCall) ? addStaticMethodCache(state, ins) : ca::str::Utf8String();
        const ca::str::Utf8String instanceCacheName =
            (!staticCall && !selfCall) ? addInstanceMethodCache(state, ins) : ca::str::Utf8String();
        const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();
        const bool dynamicInstance = !staticCall && !selfCall && !instanceCacheName.empty();
        const bool useResolved = dynamicInstance && state.resolvedDispatch && !state.traceCalls;
        const bool inlineCached =
            !selfCall && !useResolved && (staticCall || !instanceCacheName.empty()) &&
            !(state.traceCalls && !staticCall && !ins.hasDest);
        const auto emitCall = [&](CSourceStream& u) {
            if (useResolved) {
                emitResolvedInstanceCall(u,
                                         ins,
                                         state.cachePool->resolvedCallSite(ins.method.owner,
                                                                           ins.method.name,
                                                                           ins.method.desc),
                                         argumentExpressions);
            }
            else if (inlineCached) {
                emitCachedCall(u, ins, staticCall, staticCall ? cacheName : instanceCacheName,
                               argumentExpressions);
            }
            else if (ins.hasDest) {
                u.appendLineStart(destName).append(" = ");
                if (selfCall) {
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
                    else if (!instanceCacheName.empty()) {
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
                if (selfCall) {
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
                    else if (!instanceCacheName.empty()) {
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
            else {
                const std::string allocDestName = regName(ins.dest.id);
                if (state.cachePool != nullptr) {
                    const ca::str::Utf8String cacheName = addClassCache(state, ins.typeDesc);
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
                const ca::str::Utf8String cacheName = addClassCache(
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
            out.block([&](CSourceStream& u) {
                u.appendLineStart("int32_t dims[")
                    .appendInt(static_cast<ca::i64>(ins.args.size()))
                    .append("];")
                    .newLine();
                for (size_t i = 0; i < ins.args.size(); ++i) {
                    u.appendLineStart("dims[")
                        .appendInt(static_cast<ca::i64>(i))
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
                        .appendInt(static_cast<ca::i64>(componentDescs.size()))
                        .append("] = {");
                    for (size_t i = 0; i < componentDescs.size(); ++i) {
                        if (i != 0) u.append(", ");
                        const ca::str::Utf8String cacheName = addClassCache(
                            state,
                            ca::str::Utf8StringRef::from_string_view(componentDescs[i]));
                        u.append("&").append(cacheName);
                    }
                    u.append("};").newLine();
                    u.appendLineStart(regName(ins.dest.id))
                        .append(" = _mj2x_regc_alloc_multi_array_cached(")
                        .append(cStringLiteral(ins.typeDesc))
                        .append(", ")
                        .appendInt(static_cast<ca::i64>(ins.args.size()))
                        .append(", dims, component_caches);")
                        .newLine();
                }
                else {
                    u.appendLineStart(regName(ins.dest.id))
                        .append(" = _mj2x_regc_alloc_multi_array(")
                        .append(cStringLiteral(ins.typeDesc))
                        .append(", ")
                        .appendInt(static_cast<ca::i64>(ins.args.size()))
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
                out, ins, fn.indySites[localIdx], indySiteBase + localIdx, reason);
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
            const ca::str::Utf8String cacheName = addClassCache(
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
            out.appendLineStart("_mj2x_regc_monitor_enter(")
                .append(ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]))
                .append(");")
                .newLine();
            return true;
        }
        if (ins.intrinsicName == "monitor.exit") {
            if (!rejectNativeObjectEscape(state,
                                          ins.args.empty() ? RegOperand::nullRef() : ins.args[0],
                                          "monitor.exit",
                                          reason))
                return false;
            out.appendLineStart("_mj2x_regc_monitor_exit(")
                .append(ins.args.empty() ? ca::str::Utf8String("NULL") : operandExpr(ins.args[0]))
                .append(");")
                .newLine();
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
                    u.goto_("block_b" + std::to_string(t.target.block->id.value));
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
                            u.goto_("block_b" + std::to_string(t.trueTarget.block->id.value));
                        },
                        [&](CSourceStream& u) {
                            if (!emitEdgeCopies(u, t.falseTarget.copies, state, edgeTempIndex,
                                                reason)) {
                                copiesOk = false;
                                return;
                            }
                            u.goto_("block_b" + std::to_string(t.falseTarget.block->id.value));
                        });
                return copiesOk;
            }
            else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                if (!rejectNativeObjectEscape(state, t.value, "switch value", reason)) return false;
                bool copiesOk = true;
                out.switch_(operandExpr(t.value).to_std_string(), [&](CSourceStream& u) {
                    for (const auto& c : t.cases) {
                        u.case_(std::to_string(c.key), [&](CSourceStream& v) {
                            if (!emitEdgeCopies(v, c.target.copies, state, edgeTempIndex, reason)) {
                                copiesOk = false;
                                return;
                            }
                            v.goto_("block_b" + std::to_string(c.target.block->id.value));
                        });
                        if (!copiesOk) return;
                    }
                    u.case_("default", [&](CSourceStream& v) {
                        if (!emitEdgeCopies(v, t.defaultTarget.copies, state, edgeTempIndex,
                                            reason)) {
                            copiesOk = false;
                            return;
                        }
                        v.goto_("block_b" + std::to_string(t.defaultTarget.block->id.value));
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
