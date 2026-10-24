// 语料：参数数量边界——16/32/40 参数静态方法。解释器 argBuf[32] 边界：
// ≤32 在面内；>32 时编码器 per-method fail-closed（方法保留字节码），
// 类仍正确执行——两种形态下输出都必须与 baseline 一致。
public class ManyArgs {
    static int sum16(int a, int b, int c, int d, int e, int f, int g, int h,
                     int i, int j, int k, int l, int m, int n, int o, int p) {
        return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p;
    }

    static long sum32(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8,
                      int b1, int b2, int b3, int b4, int b5, int b6, int b7, int b8,
                      int c1, int c2, int c3, int c4, int c5, int c6, int c7, int c8,
                      long d1, long d2, long d3, long d4, long d5, long d6, long d7, long d8) {
        return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8
               + b1 + b2 + b3 + b4 + b5 + b6 + b7 + b8
               + c1 + c2 + c3 + c4 + c5 + c6 + c7 + c8
               + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8;
    }

    static long sum40(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8,
                      int b1, int b2, int b3, int b4, int b5, int b6, int b7, int b8,
                      int c1, int c2, int c3, int c4, int c5, int c6, int c7, int c8,
                      int e1, int e2, int e3, int e4, int e5, int e6, int e7, int e8,
                      long d1, long d2, long d3, long d4, long d5, long d6, long d7, long d8) {
        return sum32(a1, a2, a3, a4, a5, a6, a7, a8,
                     b1, b2, b3, b4, b5, b6, b7, b8,
                     c1, c2, c3, c4, c5, c6, c7, c8,
                     d1, d2, d3, d4, d5, d6, d7, d8) + (e1 + e2 + e3 + e4 + e5 + e6 + e7 + e8);
    }

    public static void main(String[] args) {
        System.out.println("sum16 " + sum16(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16));
        System.out.println("sum32 " + sum32(1, 2, 3, 4, 5, 6, 7, 8,
                                            9, 10, 11, 12, 13, 14, 15, 16,
                                            17, 18, 19, 20, 21, 22, 23, 24,
                                            1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L));
        System.out.println("sum40 " + sum40(1, 2, 3, 4, 5, 6, 7, 8,
                                            9, 10, 11, 12, 13, 14, 15, 16,
                                            17, 18, 19, 20, 21, 22, 23, 24,
                                            25, 26, 27, 28, 29, 30, 31, 32,
                                            1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L));
        // 边界值参数（负数 / 极值）
        System.out.println("sum16-neg " + sum16(-1, -2, -3, -4, -5, -6, -7, -8,
                                                0, 0, 0, 0, 0, 0, Integer.MAX_VALUE, Integer.MIN_VALUE));
    }
}
