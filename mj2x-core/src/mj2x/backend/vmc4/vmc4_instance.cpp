#include "mj2x/backend/vmc4/vmc4_instance.hpp"

#include <cstdio>
#include <string>

namespace mj2x {

VmInstance::VmInstance(const Vmc4Options& options)
    : dispatcherKind_(options.dispatcherKind)
    , seed_(options.seed)
{}

std::string VmInstance::instanceTag() const
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s_%016llx",
                  vmDispatcherName(dispatcherKind_),
                  static_cast<unsigned long long>(seed_));
    return buf;
}

bool VmInstance::isOpcodeSupported(VmOpcode op)
{
    switch (op) {
    case VmOpcode::Halt:
    case VmOpcode::Copy:
    case VmOpcode::Unary:
    case VmOpcode::Binary:
    case VmOpcode::Compare:
    case VmOpcode::Convert:
    case VmOpcode::Jump:
    case VmOpcode::Branch:
    case VmOpcode::Return: return true;
    default: return false;
    }
}

}   // namespace mj2x
