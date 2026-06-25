// mj2x SSA IR — value 体系。
// SSAValue = 一个虚拟寄存器（定义一次，引用多次）。详见 doc/IR_DESIGN.md §4。
#pragma once

#include "ssa_ids.hpp"
#include "ssa_type.hpp"

#include <cstdint>
#include <string>

namespace mj2x {

// value 类型 tag（RTTI 禁用，用 kind 替代 dynamic_cast 判值类型）。
enum class SSAValueKind : uint8_t {
    Parameter, BlockArg, InstructionResult,
    IntLiteral, FloatLiteral, NullLiteral, Opaque,
};

// 抽象基类。所有"操作数"的根。一个 SSAValue 在 function 内 id 唯一。
struct SSAValue {
    SSAValueKind valueKind;
    SSAValueId id;
    SSAType type = SSAType::UNKNOWN;

    SSAValue() : valueKind(SSAValueKind::Opaque) {}
    SSAValue(SSAValueKind vk, SSAValueId id_, SSAType t) : valueKind(vk), id(id_), type(t) {}
    virtual ~SSAValue() = default;
};

// 方法形参。入口可见。
struct SSAParameter : SSAValue {
    uint32_t index = 0;   // 第几个形参
    SSAParameter() : SSAValue(SSAValueKind::Parameter, SSAValueId{}, SSAType::UNKNOWN) {}
    SSAParameter(SSAValueId id_, SSAType t, uint32_t idx)
        : SSAValue(SSAValueKind::Parameter, id_, t), index(idx) {}
};

// 块入口参数 = 隐式 φ。每个前驱跳进来时显式传一个值给它。详见 doc/IR_DESIGN.md §6。
struct SSABlockArg : SSAValue {
    int slot = 0;          // 对应的 local slot 索引（reaching-def 决定）
    SSABlockArg() : SSAValue(SSAValueKind::BlockArg, SSAValueId{}, SSAType::UNKNOWN) {}
    SSABlockArg(SSAValueId id_, SSAType t, int s)
        : SSAValue(SSAValueKind::BlockArg, id_, t), slot(s) {}
};

// 指令的输出。唯一由某条 SSAInstruction 定义。
struct SSAInstructionResult : SSAValue {
    SSAInstructionResult() : SSAValue(SSAValueKind::InstructionResult, SSAValueId{}, SSAType::UNKNOWN) {}
    SSAInstructionResult(SSAValueId id_, SSAType t)
        : SSAValue(SSAValueKind::InstructionResult, id_, t) {}
};

// 整型常量（ICONST/BIPUSH/LDC int/long）。
struct SSAIntLiteral : SSAValue {
    int64_t value = 0;
    SSAIntLiteral() : SSAValue(SSAValueKind::IntLiteral, SSAValueId{}, SSAType::UNKNOWN) {}
    SSAIntLiteral(int64_t v, SSAType t)
        : SSAValue(SSAValueKind::IntLiteral, SSAValueId{}, t), value(v) {}
    // literal 不占正式 value id 槽位（去重/内联），id 留空；dump 时按 value 印。
};

// 浮点常量（FCONST/DCONST/LDC float/double）。
struct SSAFloatLiteral : SSAValue {
    double value = 0.0;
    SSAFloatLiteral() : SSAValue(SSAValueKind::FloatLiteral, SSAValueId{}, SSAType::UNKNOWN) {}
    SSAFloatLiteral(double v, SSAType t)
        : SSAValue(SSAValueKind::FloatLiteral, SSAValueId{}, t), value(v) {}
};

// ACONST_NULL。
struct SSANullLiteral : SSAValue {
    SSANullLiteral() : SSAValue(SSAValueKind::NullLiteral, SSAValueId{}, SSAType::REF) {}
};

// 兜底/未建模值（如 LDC String/Type/Handle，R1 先不细化）。防止构建崩。
struct SSAOpaqueValue : SSAValue {
    std::string tag;       // 调试用（如 "ldc_string", "undef_local_3"）
    SSAOpaqueValue() : SSAValue(SSAValueKind::Opaque, SSAValueId{}, SSAType::UNKNOWN) {}
    SSAOpaqueValue(SSAValueId id_, SSAType t, std::string tag_)
        : SSAValue(SSAValueKind::Opaque, id_, t), tag(std::move(tag_)) {}
};

} // namespace mj2x
