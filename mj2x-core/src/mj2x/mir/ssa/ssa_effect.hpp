// mj2x SSA IR — 副作用描述（L4 passes 用，R1 填保守值）。
// 详见 doc/IR_DESIGN.md §7。
#pragma once

namespace mj2x {

struct SSAEffect
{
    bool mayThrow            = false;
    bool readsMemory         = false;
    bool writesMemory        = false;
    bool readsExternalState  = false;
    bool writesExternalState = false;
    bool callsExternal       = false;
    bool resolvesExternal    = false;
    bool isBarrier           = false;

    SSAEffect withMayThrow(bool v = true) const
    {
        SSAEffect e = *this;
        e.mayThrow  = v;
        return e;
    }

    static SSAEffect pure() { return SSAEffect{}; }
    static SSAEffect readMemory()
    {
        SSAEffect e;
        e.readsMemory = true;
        return e;
    }
    static SSAEffect writeMemory()
    {
        SSAEffect e;
        e.writesMemory = true;
        return e;
    }
    static SSAEffect externalCall()
    {
        SSAEffect e;
        e.callsExternal       = true;
        e.readsExternalState  = true;
        e.writesExternalState = true;
        e.mayThrow            = true;
        return e;
    }
    static SSAEffect barrier()
    {
        SSAEffect e;
        e.isBarrier = true;
        return e;
    }
};

}   // namespace mj2x
