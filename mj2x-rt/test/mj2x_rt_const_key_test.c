#include "llvmir/mj2x_llvmir_runtime.h"
#include "regc/mj2x_regc_crt.h"

#include <assert.h>
#include <stdint.h>
#include <stddef.h>

// 立即数隐藏 per-site 密钥派生契约（对标 jvmtp obfuscation.constants）。
// applyConstantHiding 把整型立即数 C 改写为 E ^ const_key(site)：
// E = C ^ K 编译期物化，K 运行时派生。本测试守四件事：
// 1. 确定性：同 site 同 key（可复现构建不变量）；
// 2. 双端一致：regc 与 llvm 两端同常量同产物（任一端漂移即使 E ^ K != C，
//    而 C++ 侧 splitmix64（ca::random::SplitMix64 正典）与本实现的逐位一致
//    由 E2E（真实编译 + 运行生成工程）守；
// 3. 站点区分：抽样站点两两不同 key（同 key 多站点可被关联分析）；
// 4. 非退化：key != 0（key 为 0 时 E == C，立即数原样暴露）；
// 另守 XOR 往返：E ^ K == C（编译期/运行时分工的语义基础）。
void test_const_key_derivation(void)
{
    static const int64_t sites[] = {0,    1,     2,          3,
                                    7,    42,    255,        1000,
                                    -1,   -2,    -1000003,   INT64_MIN,
                                    INT64_MAX,
                                    0x123456789ABCDEFLL,    -0x0FEDCBA987654321LL};
    const size_t count = sizeof(sites) / sizeof(sites[0]);

    for (size_t i = 0; i < count; ++i) {
        const int64_t site = sites[i];
        const int64_t k1   = mj2x_regc_const_key(site);
        const int64_t k2   = mj2x_regc_const_key(site);
        const int64_t kl   = mj2x_llvm_const_key(site);
        assert(k1 == k2);       /* 确定性 */
        assert(k1 == kl);       /* 双端一致 */
        assert(k1 != 0);        /* 非退化 */
    }

    /* 抽样站点两两不同（站点区分）。 */
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            assert(mj2x_regc_const_key(sites[i]) != mj2x_regc_const_key(sites[j]));
        }
    }

    /* 相邻站点区分（局部计数器 0,1,2,... 的主战场）。 */
    for (int64_t site = 0; site < 256; ++site) {
        assert(mj2x_regc_const_key(site) != mj2x_regc_const_key(site + 1));
    }

    /* XOR 往返：E ^ K == C，且 E != C（立即数确实被隐藏）。 */
    const int64_t c = 0x0123456789ABCDEFLL;
    const int64_t e = c ^ mj2x_regc_const_key(9);
    assert((e ^ mj2x_regc_const_key(9)) == c);
    assert(e != c);

    /* I32 口径：符号扩展到 64 位后异或，截断回 32 位仍还原（pass 侧
       Convert 截断路线的运行时基础）。 */
    const int32_t c32 = -123456789;
    const int64_t c64 = (int64_t)c32;
    const int64_t e64 = c64 ^ mj2x_llvm_const_key(4);
    assert((int32_t)(e64 ^ mj2x_llvm_const_key(4)) == c32);
}
