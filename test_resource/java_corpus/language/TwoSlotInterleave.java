// 语料：双槽（2-slot）值密集交错——long/double 局部变量槽位配对、双槽值在循环
// 回边携带、混合窄化提升、double/long 互转语义（d2l/l2d/f2l/l2f 舍入）。
public class TwoSlotInterleave {
    public static void main(String[] args) {
        // 双槽交错局部（槽位分配压力形态）
        long la = 1, lb = 2;
        double da = 0.5, db = 1.5;
        int i = 3;
        float fa = 2.5f;
        long lc = 4;
        for (int k = 0; k < 5; k++) {
            la = la * 3 + i;          // 回边携带 long
            da = da * 1.5 + k;        // 回边携带 double
            lb = lb + la;
            db = db + da / 2;
            lc = lc ^ (k << 1);
            fa = fa + k;
        }
        System.out.println("loop " + la + " " + lb + " " + da + " " + db + " " + lc + " " + fa);

        // 转换语义（舍入/截断）
        double d1 = 1.9, d2 = -1.9, d3 = 0.5, d4 = -0.5;
        System.out.println("d2l " + (long) d1 + " " + (long) d2 + " " + (long) d3 + " " + (long) d4);
        float f1 = 8.9e9f;
        System.out.println("f2l " + (long) f1 + " f2i " + (int) f1);
        System.out.println("l2d " + (double) Long.MAX_VALUE + " " + (double) Long.MIN_VALUE);
        System.out.println("l2f " + (float) 9007199254740993L + " " + (float) 16777217);
        System.out.println("i2f " + (float) 16777217 + " i2d " + (double) 123456789);

        // long 算术边界（idiv/lrem 已在 ArithEdges；此处 long 移位与位段）
        long bits = 0xABCDEF0123456789L;
        System.out.println("bits " + Long.toHexString(bits >>> 16) + " "
                           + Long.toHexString(bits << 16) + " " + Long.toHexString(bits & 0xFFFFL));
        System.out.println("neg " + Long.toHexString(-bits));

        // double/float 混合表达式（f2d/d2f 转换链）
        float ff = 1.1f;
        double dd = ff * 3;              // f2d 后乘
        float back = (float) dd;
        System.out.println("roundtrip " + dd + " " + back + " " + (dd == back));
        double precise = 0.1 + 0.2;
        System.out.println("0.1+0.2 " + precise + " " + (precise == 0.3));

        // 双槽返回值链（lreturn/dreturn 寄存器对）
        System.out.println("chain " + half(Long.MAX_VALUE) + " " + avg(new double[] {1, 2, 3, 4}));
    }

    static long half(long v) { return v / 2; }
    static double avg(double[] xs) {
        double s = 0;
        for (double x : xs) s += x;
        return s / xs.length;
    }
}
