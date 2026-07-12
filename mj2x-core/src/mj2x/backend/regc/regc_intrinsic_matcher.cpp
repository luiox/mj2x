#include "regc_intrinsic_matcher.hpp"

#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;

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
    call.owner        = ins.method.owner;
    call.name         = ins.method.name;
    call.desc         = ins.method.desc;
    call.invokeOpcode = ins.method.invokeOpcode;
    call.argCount     = ins.args.size();
    return call;
}

}   // namespace mj2x
