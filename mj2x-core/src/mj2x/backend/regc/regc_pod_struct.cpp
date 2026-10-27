// regc_pod_struct.cpp
//
// POD 类降级为 C struct（--regc-pod-struct，issue #17）。
//
// Java 对象在 native 侧是一个 jobject + 每次字段访问两次 JNI
// （Get/SetXxxField）。若某个类的实例字段全是原生类型、且实例在函数内不逃逸，
// 这个对象就不需要真的存在于 JVM 堆上：它可以是一个 C struct，字段访问变成
// 结构体成员读写，零 JNI，字段名也从产物里消失。
//
// 逃逸判定复用 StringBuilder/StringBuffer native lowering 的同一套形状
// （RegIdUnionFind 归并寄存器根 + 逐指令记逃逸 + 逃逸根整根回退），只是这里
// 允许的"安全使用"更窄：只有实例字段读写与 <init> 的 receiver。

#include "mj2x/backend/regc/regc_emitter_internal.hpp"

#include "mj2x/core/jvm_opcodes.hpp"
#include "mj2x/core/jvm_type.hpp"

namespace mj2x {
namespace {


bool isPrimitiveFieldDesc(std::string_view desc)
{
    if (desc.empty()) return false;
    switch (desc[0]) {
    case 'Z':
    case 'B':
    case 'C':
    case 'S':
    case 'I':
    case 'J':
    case 'F':
    case 'D': return true;
    default: return false;
    }
}

std::string sanitizeOwner(const std::string& owner)
{
    std::string out;
    for (char ch : owner) {
        out += (ch == '/' || ch == '.' || ch == '$') ? '_' : ch;
    }
    return out;
}

}   // namespace

bool isPodClassOwner(CompilerIndex* classIndex, const ca::str::Utf8StringRef& owner)
{
    if (classIndex == nullptr) return false;
    CompilerClass* clazz = classIndex->getClass(owner.to_std_string());
    if (clazz == nullptr || clazz->dom == nullptr) return false;
    // 库类拿不到字段全貌，无法判定。
    if (clazz->isLibrary) return false;

    // 继承/接口意味着实例可能被当成别的类型用，字段布局不再等于本类字段表。
    if (!clazz->dom->interfaces.empty()) return false;
    if (clazz->dom->superName() != "java/lang/Object") return false;

    // 所有实例字段必须是原生类型。静态字段走 JVM 语义，不参与 struct 布局。
    bool hasInstanceField = false;
    for (const auto& field : clazz->fields) {
        if ((field.access & Opcodes::ACC_STATIC) != 0) continue;
        hasInstanceField = true;
        if (!isPrimitiveFieldDesc(field.desc)) return false;
    }
    return hasInstanceField;
}

std::string podStructTypeName(const ca::str::Utf8StringRef& owner)
{
    return "pod_" + sanitizeOwner(owner.to_std_string());
}

/// @brief POD struct 实例的**指针别名**变量名。
///
/// @details
/// 别名必须是指针：同一个对象在 RegIR 里可以有多个寄存器（phi / 边拷贝），
/// 值语义的 struct 拷贝会让它们变成互不影响的独立副本，与 Java 的引用语义
/// 不等价（实测产物算错结果）。指针拷贝才是别名。
std::string podStructPtrName(RegId id)
{
    return "podp_r" + std::to_string(id.value);
}

/// @brief 分配点的 backing struct 变量名（指针别名指向它）。
std::string podStructValueName(RegId id)
{
    return "podv_r" + std::to_string(id.value);
}

std::string podStructMemberName(std::string_view name)
{
    // 字段名可能是 Java 标识符之外的形状（合成字段带 $），统一成 C 标识符。
    return sanitizeOwner(std::string(name));
}

std::vector<RegCPodStructMember> podStructMembers(CompilerIndex* classIndex,
                                                 const std::string& owner)
{
    std::vector<RegCPodStructMember> members;
    if (classIndex == nullptr) return members;
    CompilerClass* clazz = classIndex->getClass(owner);
    if (clazz == nullptr || clazz->dom == nullptr) return members;
    for (const auto& field : clazz->fields) {
        if ((field.access & Opcodes::ACC_STATIC) != 0) continue;
        if (!isPrimitiveFieldDesc(field.desc)) continue;
        const char tag = field.desc[0];
        const char* type = "int32_t";
        switch (tag) {
        case 'Z': type = "int32_t"; break;   // JVM 布尔按 int 传，与槽宽对齐
        case 'B': type = "int8_t"; break;
        case 'C': type = "uint16_t"; break;
        case 'S': type = "int16_t"; break;
        case 'I': type = "int32_t"; break;
        case 'J': type = "int64_t"; break;
        case 'F': type = "float"; break;
        case 'D': type = "double"; break;
        default: continue;
        }
        members.push_back(RegCPodStructMember{podStructMemberName(field.name), type});
    }
    return members;
}

RegCPodStructPlan analyzePodStructs(const RegFunction& fn, CompilerIndex* classIndex)
{
    RegCPodStructPlan plan;
    if (classIndex == nullptr) return plan;

    RegIdUnionFind                             ids;
    std::vector<std::pair<uint32_t, std::string>> allocations;   // (寄存器, owner)

    for (const auto& parameter : fn.parameters) {
        if (parameter.type == SSAType::REF) ids.add(parameter.id.value);
    }

    // 第一遍：建并查集（Copy 归并同一对象的多个寄存器）+ 收集 POD 分配点。
    // 终结器的边拷贝也要归并：循环回边会让同一个对象出现在多个块的不同寄存器
    // 里，不归并就无法算出"哪些寄存器是同一个 POD 实例"。
    auto connectTerminatorCopies = [&](const RegTerminator& terminator) {
        std::visit(
            [&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                // 边拷贝挂在跳转目标上（RegSuccessor::copies），不是终结器直接字段。
                auto connect = [&](const std::vector<RegCopy>& copies) {
                    for (const auto& copy : copies) {
                        if (copy.source.kind == RegOperandKind::Register &&
                            copy.source.type == SSAType::REF) {
                            ids.unite(copy.dest.id.value, copy.source.reg.value);
                        }
                    }
                };
                if constexpr (std::is_same_v<T, RegJumpTerminator>) {
                    connect(t.target.copies);
                }
                else if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    connect(t.trueTarget.copies);
                    connect(t.falseTarget.copies);
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    for (const auto& item : t.cases) connect(item.target.copies);
                    connect(t.defaultTarget.copies);
                }
            },
            terminator);
    };

    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            if (ins.hasDest && ins.dest.type == SSAType::REF) ids.add(ins.dest.id.value);
            if (ins.kind == RegInstructionKind::Copy &&
                ins.copy.source.kind == RegOperandKind::Register &&
                ins.copy.source.type == SSAType::REF) {
                ids.unite(ins.copy.dest.id.value, ins.copy.source.reg.value);
            }
            else if (ins.kind == RegInstructionKind::Allocate &&
                     ins.allocationKind == SSAAllocationKind::Object &&
                     isPodClassOwner(classIndex, ins.typeDesc)) {
                allocations.emplace_back(ins.dest.id.value, ins.typeDesc.to_std_string());
                ids.add(ins.dest.id.value);
            }
        }
        connectTerminatorCopies(block->terminator);
    }
    if (allocations.empty()) return plan;

    std::set<uint32_t>              podRoots;
    std::map<uint32_t, std::string> ownerByRoot;
    for (const auto& [reg, owner] : allocations) {
        const uint32_t root = ids.find(reg);
        podRoots.insert(root);
        ownerByRoot[root] = owner;
    }

    // 返回 -1 表示"不是 POD struct 根"。
    auto rootOf = [&](const RegOperand& operand) -> long long {
        if (operand.kind != RegOperandKind::Register || operand.type != SSAType::REF) return -1;
        const uint32_t root = ids.find(operand.reg.value);
        return podRoots.count(root) != 0 ? static_cast<long long>(root) : -1;
    };
    auto escape = [&](const RegOperand& operand) {
        const long long root = rootOf(operand);
        if (root >= 0) plan.escapedRoots.insert(static_cast<uint32_t>(root));
    };
    auto escapeAll = [&](const std::vector<RegOperand>& operands) {
        for (const auto& operand : operands) escape(operand);
    };

    // 第二遍：白名单之外的一切使用都算逃逸。
    //
    // 刻意保守：漏判逃逸 = 数据静默错乱（比崩溃更难查），宁可不降级。覆盖面
    // 小是本项的已知代价，见 doc/techniques/pod_struct.md。
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            switch (ins.kind) {
            case RegInstructionKind::LoadField:
                // 实例字段的 receiver 不逃逸；静态字段的 receiver 槽为空。
                if (ins.field.isStatic) return plan;   // 静态字段：整函数放弃降级
                break;
            case RegInstructionKind::StoreField:
                if (ins.field.isStatic) return plan;
                escape(ins.value);   // 存进字段的值若来自另一 POD 实例则逃逸
                break;
            case RegInstructionKind::Call:
                // 只放行 <init>（构造器体的字段初始化由 struct 零初始化吸收）。
                if (ins.method.name == "<init>") {
                    for (size_t i = 1; i < ins.args.size(); ++i) escape(ins.args[i]);
                }
                else {
                    escapeAll(ins.args);
                }
                break;
            case RegInstructionKind::Copy:
                // 源是 POD 根、且目标同为 REF：归并已在第一遍完成，不逃逸。
                break;
            default:
                // 其余指令：读到的任何 POD 寄存器一律逃逸。
                escape(ins.value);
                escape(ins.lhs);
                escape(ins.rhs);
                escape(ins.receiver);
                escape(ins.index);
                escapeAll(ins.args);
                break;
            }
        }

        // 终结器按类型取用到的 REF 操作数（与 regc_intrinsics.cpp 同形状）。
        std::visit(
            [&](const auto& t) {
                using T = std::decay_t<decltype(t)>;
                if constexpr (std::is_same_v<T, RegBranchTerminator>) {
                    escape(t.condition);
                }
                else if constexpr (std::is_same_v<T, RegSwitchTerminator>) {
                    escape(t.value);
                }
                else if constexpr (std::is_same_v<T, RegThrowTerminator>) {
                    escape(t.value);
                }
                else if constexpr (std::is_same_v<T, RegReturnTerminator>) {
                    if (t.hasValue) escape(t.value);
                }
            },
            block->terminator);
    }

    for (const uint32_t root : podRoots) {
        if (plan.escapedRoots.count(root) != 0) continue;
        plan.structRoots.insert(root);
        plan.ownerByRoot[root] = ownerByRoot[root];
    }
    if (plan.structRoots.empty()) return plan;

    // 展开成"全部寄存器"集合：结构体变量要在函数作用域预声明，需要知道每个
    // 会持有该实例的寄存器（含边拷贝的目标寄存器）。
    for (const auto& [id, parent] : ids.entries()) {
        const uint32_t root = ids.find(id);
        const auto     it   = plan.ownerByRoot.find(root);
        if (it == plan.ownerByRoot.end()) continue;
        plan.ownerByRegister[id] = it->second;
    }
    // 分配点：只有它们需要 backing struct 变量。
    for (const auto& [reg, owner] : allocations) {
        if (plan.ownerByRegister.count(reg) != 0) plan.allocationRegisters.insert(reg);
    }
    return plan;
}

const std::string* podStructStorageFor(const RegCFunctionEmitState& state,
                                      const RegOperand& operand)
{
    if (operand.kind != RegOperandKind::Register) return nullptr;
    const auto it = state.nativeObjects.find(operand.reg.value);
    if (it == state.nativeObjects.end()) return nullptr;
    if (it->second.kind != RegCIntrinsicNativeObjectKind::PodStruct) return nullptr;
    return &it->second.storageName;
}

void podStructEdgeCopy(CSourceStream& out, RegCFunctionEmitState& state, const RegCopy& copy)
{
    const RegCNativeObjectState* source = nullptr;
    if (copy.source.kind == RegOperandKind::Register) {
        const auto it = state.nativeObjects.find(copy.source.reg.value);
        if (it != state.nativeObjects.end() &&
            it->second.kind == RegCIntrinsicNativeObjectKind::PodStruct) {
            source = &it->second;
        }
    }
    if (source == nullptr) return;

    // 结构体按值拷贝：源与目标是两个独立的 C 变量，语义等价于"同一个对象的
    // 两个别名"——与 JNI 引用语义不同，但对象不逃逸时对外不可观测。
    // 别名拷贝 = 指针赋值（值语义拷贝会破坏 Java 的引用语义）。
    out.appendLineStart(podStructPtrName(copy.dest.id))
        .append(" = ")
        .append(podStructPtrName(copy.source.reg))
        .append(";")
        .newLine();
    out.assign(regName(copy.dest.id), "NULL");
    state.nativeObjects[copy.dest.id.value] =
        RegCNativeObjectState{RegCIntrinsicNativeObjectKind::PodStruct,
                              podStructPtrName(copy.dest.id), source->typeName};
}

bool emitPodStructFieldLoad(CSourceStream& out, RegCFunctionEmitState& state,
                            const RegInstruction& ins)
{
    if (ins.field.isStatic || !ins.hasDest) return false;
    const std::string* storage = podStructStorageFor(state, ins.receiver);
    if (storage == nullptr) return false;
    out.appendLineStart(regName(ins.dest.id))
        .append(" = ")
        .append(*storage)
        .append("->")
        .append(podStructMemberName(ins.field.name))
        .append(";")
        .newLine();
    return true;
}

bool emitPodStructFieldStore(CSourceStream& out, RegCFunctionEmitState& state,
                             const RegInstruction& ins, std::string& reason)
{
    if (ins.field.isStatic) return false;
    const std::string* storage = podStructStorageFor(state, ins.receiver);
    if (storage == nullptr) return false;

    ca::str::Utf8String valueExpression;
    const bool          materialized =
        emitMaterializedStringBuilder(out, state, ins.value, valueExpression);
    if (!materialized &&
        !rejectNativeObjectEscape(state, ins.value, "pod struct field store value", reason)) {
        // 存进 struct 的值本身是个 native 对象——不该发生（逃逸分析已把这种
        // 实例判为逃逸），落到原路径由它给出诊断。
        return false;
    }
    out.appendLineStart(*storage)
        .append("->")
        .append(podStructMemberName(ins.field.name))
        .append(" = ")
        .append(valueExpression)
        .append(";")
        .newLine();
    return true;
}

}   // namespace mj2x
