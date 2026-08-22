#include "common/mj2x_rt_protect.h"

#include <assert.h>
#include <stdint.h>

// 由 mj2x_regc_crt_test.c 的 main 统一调用（单 runner 惯例）。

// R3 探测移植的纯逻辑单测（解码器与 mj2x-runtime/antidebug 移植源语义一致）。
// 平台探测（PEB/NtQuery/GetThreadContext/maps）需真实进程环境，由 e2e 的
// --runtime-protection 编译链覆盖；此处只验证判定函数。

static void test_nt_global_flag_suspicious(void)
{
    // 调试堆三标志位（TAIL|FREE|PARAM CHECK = 0x10|0x20|0x40）任一命中即可疑。
    assert(mj2x_rt_nt_global_flag_suspicious(0u) == 0);
    assert(mj2x_rt_nt_global_flag_suspicious(0x70u) == 1);
    assert(mj2x_rt_nt_global_flag_suspicious(0x10u) == 1);
    assert(mj2x_rt_nt_global_flag_suspicious(0x20u) == 1);
    assert(mj2x_rt_nt_global_flag_suspicious(0x40u) == 1);
    // 掩码外标志位不触发（如 FLG_STOP_ON_EXCEPTION=0x01 属正常域）。
    assert(mj2x_rt_nt_global_flag_suspicious(0x0Fu) == 0);
    assert(mj2x_rt_nt_global_flag_suspicious(0x01u) == 0);
}

static void test_hardware_breakpoint_enabled(void)
{
    // 无地址：即便 Dr7 全使能也不命中。
    assert(mj2x_rt_hardware_breakpoint_enabled(0, 0, 0, 0, 0xFFu) == 0);
    // 有地址但 Dr7 使能位为空：不命中（地址寄存器残留是常见误报源）。
    assert(mj2x_rt_hardware_breakpoint_enabled((uintptr_t)0xDEADBEEF, 0, 0, 0, 0u) == 0);
    // 地址 + 使能：命中（Dr7 低 8 位 L/G0-3 任一非零）。
    assert(mj2x_rt_hardware_breakpoint_enabled((uintptr_t)0xDEADBEEF, 0, 0, 0, 0x01u) == 1);
    assert(mj2x_rt_hardware_breakpoint_enabled(0, 0, 0, (uintptr_t)0x7FFE0000, 0xAAu) == 1);
    // 仅高字节置位（保留域）：低 8 位为空不命中。
    assert(mj2x_rt_hardware_breakpoint_enabled((uintptr_t)0x1000, 0, 0, 0, 0x100u) == 0);
}

void mj2x_rt_protect_probe_test_run(void)
{
    test_nt_global_flag_suspicious();
    test_hardware_breakpoint_enabled();

}
