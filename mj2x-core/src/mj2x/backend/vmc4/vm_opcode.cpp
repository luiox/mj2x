#include "mj2x/backend/vmc4/vm_opcode.hpp"

namespace mj2x {

const char* vmOpcodeName(VmOpcode op)
{
    switch (op) {
    case VmOpcode::Halt: return "HALT";
    case VmOpcode::Copy: return "COPY";
    case VmOpcode::Unary: return "UNARY";
    case VmOpcode::Binary: return "BINARY";
    case VmOpcode::Compare: return "COMPARE";
    case VmOpcode::Convert: return "CONVERT";
    case VmOpcode::Jump: return "JUMP";
    case VmOpcode::Branch: return "BRANCH";
    case VmOpcode::Return: return "RETURN";
    case VmOpcode::LoadField: return "LOAD_FIELD";
    case VmOpcode::StoreField: return "STORE_FIELD";
    case VmOpcode::ArrayLoad: return "ARRAY_LOAD";
    case VmOpcode::ArrayStore: return "ARRAY_STORE";
    case VmOpcode::Call: return "CALL";
    case VmOpcode::Allocate: return "ALLOCATE";
    case VmOpcode::Intrinsic: return "INTRINSIC";
    case VmOpcode::Barrier: return "BARRIER";
    case VmOpcode::Throw: return "THROW";
    case VmOpcode::Switch: return "SWITCH";
    case VmOpcode::SuperInc: return "SUPER_INC";
    case VmOpcode::SuperDec: return "SUPER_DEC";
    }
    return "?";
}

const char* vmDispatcherName(VmDispatcherKind kind)
{
    switch (kind) {
    case VmDispatcherKind::SwitchTable: return "switch_table";
    case VmDispatcherKind::FuncPtrTable: return "funcptr_table";
    case VmDispatcherKind::ComputedGoto: return "computed_goto";
    case VmDispatcherKind::IfNest: return "if_nest";
    case VmDispatcherKind::StateDispatcher: return "state_dispatcher";
    }
    return "?";
}

}   // namespace mj2x
