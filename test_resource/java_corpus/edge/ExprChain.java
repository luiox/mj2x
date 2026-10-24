// 语料：超长表达式链——60+ 操作数的混合表达式（除法防常量折叠），SSA 值编号
// 密集递增（寄存器分配/定长化压力面），嵌套括号深度 20+。
public class ExprChain {
    static int chain(int seed) {
        // 线性链：每步用上一步结果（强依赖链，SSA 深链）
        int v = seed;
        v = v + 1; v = v * 3; v = v - 7; v = v / 2; v = v ^ 0x5A;
        v = v + 13; v = v * 5; v = v - 3; v = v / 3; v = v | 0x0F0;
        v = v + 101; v = v * 7; v = v - 11; v = v / 5; v = v & 0xFFFF;
        v = v + 1009; v = v * 13; v = v - 17; v = v / 7; v = v ^ 0x00FF;
        return v;
    }

    static long wideChain(long seed) {
        long v = seed;
        v = v * 6364136223846793005L + 1442695040888963407L;
        v = v ^ (v >>> 33);
        v = v * 0xff51afd7ed558ccdL;
        v = v ^ (v >>> 33);
        v = v * 0xc4ceb9fe1a85ec53L;
        v = v ^ (v >>> 33);
        return v;
    }

    public static void main(String[] args) {
        // 宽表达式：60 操作数求和（单语句 SSA 源值爆炸）
        int s1 = 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10
                 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20
                 + 21 + 22 + 23 + 24 + 25 + 26 + 27 + 28 + 29 + 30
                 + 31 + 32 + 33 + 34 + 35 + 36 + 37 + 38 + 39 + 40
                 + 41 + 42 + 43 + 44 + 45 + 46 + 47 + 48 + 49 + 50
                 + 51 + 52 + 53 + 54 + 55 + 56 + 57 + 58 + 59 + 60;
        System.out.println("sum60=" + s1);

        // 深嵌套括号（24 层，字面展开保证源码级嵌套深度）
        int nested = 1;
        nested = ((((nested + 1) * 2) + 3) * 2 + ((nested + 4) * 2) + 5) * 2
                 + ((((nested + 6) * 2) + 7) * 2 + ((nested + 8) * 2) + 9) * 2
                 + ((((nested + 10) * 2) + 11) * 2 + ((nested + 12) * 2) + 13) * 2
                 + ((((nested + 14) * 2) + 15) * 2 + ((nested + 16) * 2) + 17) * 2;
        System.out.println("nested=" + nested);

        // 依赖链循环（链 + 回边）
        long acc = 0;
        for (int i = 1; i <= 50; i++) {
            acc = acc * 31 + chain(i);
        }
        System.out.println("chain-acc=" + acc);

        // long 混合链（splitmix64 形态，双槽压力）
        long mix = wideChain(42);
        System.out.println("mix=" + Long.toHexString(mix));
        long acc2 = 0;
        for (int i = 0; i < 20; i++) acc2 ^= wideChain(acc2 + i);
        System.out.println("mix-chain=" + Long.toHexString(acc2));

        // 混合类型大表达式（int/long/double 提升矩阵）
        int base = 7;
        double mixed = base / 2 + base * 3L / 4 - base * 1.5 + (base % 3) * 2.5
                       - base / 3.0 * 4 + (base ^ 5) * 1.25 + 6.75e2 / base;
        System.out.println("mixed=" + mixed);
    }
}
