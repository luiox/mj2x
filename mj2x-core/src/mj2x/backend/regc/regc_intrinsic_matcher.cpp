#include "regc_intrinsic_matcher.hpp"

#include "mj2x/core/jvm_opcodes.hpp"
#include "mj2x/core/jvm_type.hpp"

namespace mj2x {


bool RegCIntrinsicCall::isStatic() const
{
    return invokeOpcode == Opcodes::INVOKESTATIC;
}

std::string RegCIntrinsicCall::displayName() const
{
    return owner + "." + name + desc;
}

RegCIntrinsicCall RegCIntrinsicCallMatcher::fromInstruction(const RegInstruction& ins)
{
    RegCIntrinsicCall call;
    call.owner        = ins.method.owner.to_std_string();
    call.name         = ins.method.name.to_std_string();
    call.desc         = ins.method.desc.to_std_string();
    call.invokeOpcode = ins.method.invokeOpcode;
    call.argCount     = ins.args.size();
    return call;
}

}   // namespace mj2x
