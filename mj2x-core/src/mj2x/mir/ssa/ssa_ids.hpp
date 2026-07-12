// mj2x SSA IR — id 分配。
// value id = 虚拟寄存器号（L5 直接映射成 C 局部变量 v<id>）。详见 doc/IR_DESIGN.md §9。
#pragma once

#include <cstdint>

namespace mj2x {

struct SSABlockId
{
    uint32_t value = 0;
    bool     operator==(const SSABlockId& o) const { return value == o.value; }
    bool     operator!=(const SSABlockId& o) const { return value != o.value; }
};

struct SSAValueId
{
    uint32_t value = 0;
    bool     operator==(const SSAValueId& o) const { return value == o.value; }
    bool     operator!=(const SSAValueId& o) const { return value != o.value; }
};

// 单线程构建期递增 id 发放器。id 稳定（构建后不变），方便 dump / 调试 / pass。
struct SSAIds
{
    uint32_t nextBlock = 0;
    uint32_t nextValue = 0;

    SSABlockId newBlockId() { return SSABlockId{nextBlock++}; }
    SSAValueId newValueId() { return SSAValueId{nextValue++}; }
};

}   // namespace mj2x
