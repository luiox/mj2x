#include "mj2x/backend/indy_bootstrap_plan.hpp"


#include "mj2x/core/jvm_opcodes.hpp"
#include "mj2x/core/jvm_type.hpp"
#include "libca/str/utf8_string_arena.hpp"

namespace mj2x {

std::vector<IndyBsmSlot> planIndyBsmSlots(const std::string& bsmDesc, size_t bsmArgCount)
{
    std::vector<IndyBsmSlot> slots;
    // 空描述符不再特判：mj2x::Type 门面（批 1 libjbc 迁移）经 jbc::type 结构
    // 校验前置，空/截断描述符确定性抛 invalid_argument——旧栈
    // Type::getArgumentTypes 空串越界读的 flake 面已根治（J-6）。
    const auto bsmArgTypes =
        Type::getArgumentTypes(ca::str::Utf8StringRef::from_cstr(bsmDesc.c_str()));
    // bsmArgIndex 语义（对齐切换前三后端原判定，bsmArgs 不含 Lookup/name/type
    // 三件套——它们是 invokedynamic 隐式实参）：全程 = 静态实参序数（0 起）。
    // Varargs 槽：起点 = 当前序数，数量 = 静态实参总数 - 起点（原式
    // `bsmArgIndex < size ? size - bsmArgIndex : 0` 逐字同构）。
    size_t staticOrdinal = 0;
    for (size_t i = 0; i < bsmArgTypes.size(); ++i) {
        if (i == 0) {
            slots.push_back(IndyBsmSlot{IndyBsmSlotKind::Lookup, 0, 0, 0});
            continue;
        }
        if (i == 1) {
            slots.push_back(IndyBsmSlot{IndyBsmSlotKind::SiteName, 0, 0, 0});
            continue;
        }
        if (i == 2) {
            slots.push_back(IndyBsmSlot{IndyBsmSlotKind::SiteType, 0, 0, 0});
            continue;
        }
        const auto& formal = bsmArgTypes[i];
        // 尾随 [Ljava/lang/Object; = varargs 打包槽（三后端原判定逐字同构：
        // ARRAY 排序 + 描述符全等）。
        ca::str::Utf8StringArena  scratch;
        const bool isTrailingObjectArray = i + 1 == bsmArgTypes.size() &&
                                           formal.getSort() == Type::ARRAY &&
                                           formal.getDescriptor(scratch).to_std_string() ==
                                               "[Ljava/lang/Object;";
        if (isTrailingObjectArray) {
            IndyBsmSlot slot{};
            slot.kind        = IndyBsmSlotKind::Varargs;
            slot.bsmArgIndex = staticOrdinal;
            slot.varargCount = staticOrdinal < bsmArgCount ? bsmArgCount - staticOrdinal : 0;
            slots.push_back(slot);
            continue;
        }
        if (staticOrdinal >= bsmArgCount) {
            slots.push_back(IndyBsmSlot{IndyBsmSlotKind::Null, 0, 0, 0});
            continue;
        }
        IndyBsmSlot slot{};
        slot.kind        = IndyBsmSlotKind::StaticArg;
        slot.bsmArgIndex = staticOrdinal;
        slot.formalSort  = static_cast<int>(formal.getSort());
        slots.push_back(slot);
        ++staticOrdinal;
    }
    return slots;
}

IndyHandleFind indyHandleFindFor(int handleTag)
{
    static constexpr const char* kFindVirtualDesc =
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
        "Ljava/lang/invoke/MethodHandle;";
    switch (handleTag) {
    case Opcodes::H_GETFIELD:
        return {"findGetter",
                "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                "Ljava/lang/invoke/MethodHandle;",
                IndyHandleArgShape::NameFieldType, true};
    case Opcodes::H_GETSTATIC:
        return {"findStaticGetter",
                "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                "Ljava/lang/invoke/MethodHandle;",
                IndyHandleArgShape::NameFieldType, true};
    case Opcodes::H_PUTFIELD:
        return {"findSetter",
                "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                "Ljava/lang/invoke/MethodHandle;",
                IndyHandleArgShape::NameFieldType, true};
    case Opcodes::H_PUTSTATIC:
        return {"findStaticSetter",
                "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                "Ljava/lang/invoke/MethodHandle;",
                IndyHandleArgShape::NameFieldType, true};
    case Opcodes::H_INVOKESTATIC:
        return {"findStatic", kFindVirtualDesc, IndyHandleArgShape::NameType, true};
    case Opcodes::H_INVOKESPECIAL:
        return {"findSpecial",
                "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/"
                "Class;)Ljava/lang/invoke/MethodHandle;",
                IndyHandleArgShape::NameTypeOwner, true};
    case Opcodes::H_NEWINVOKESPECIAL:
        return {"findConstructor",
                "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)"
                "Ljava/lang/invoke/MethodHandle;",
                IndyHandleArgShape::TypeOnly, false};
    case Opcodes::H_INVOKEINTERFACE:
    case Opcodes::H_INVOKEVIRTUAL:
    default:
        return {"findVirtual", kFindVirtualDesc, IndyHandleArgShape::NameType, true};
    }
}

IndyBsmArgKind classifyIndyBsmArg(const ca::core::Any& value)
{
    if (value.as<std::string>() != nullptr) return IndyBsmArgKind::String;
    if (value.as<ca::str::Utf8StringRef>() != nullptr) return IndyBsmArgKind::String;
    if (const auto* type = value.as<mj2x::Type>()) {
        return type->getSort() == Type::METHOD ? IndyBsmArgKind::MethodType
                                                                : IndyBsmArgKind::Class;
    }
    if (value.as<mj2x::Handle>() != nullptr) return IndyBsmArgKind::Handle;
    if (value.as<ca::i32>() != nullptr) return IndyBsmArgKind::Int;
    if (value.as<ca::i64>() != nullptr) return IndyBsmArgKind::Long;
    if (value.as<float>() != nullptr) return IndyBsmArgKind::Float;
    if (value.as<double>() != nullptr) return IndyBsmArgKind::Double;
    return IndyBsmArgKind::Null;
}

const char* indyBoxedClassNameForDescHead(char descHead)
{
    switch (descHead) {
    case 'Z': return "java/lang/Boolean";
    case 'B': return "java/lang/Byte";
    case 'C': return "java/lang/Character";
    case 'S': return "java/lang/Short";
    case 'I': return "java/lang/Integer";
    case 'J': return "java/lang/Long";
    case 'F': return "java/lang/Float";
    case 'D': return "java/lang/Double";
    default: return "java/lang/Object";
    }
}

}   // namespace mj2x
