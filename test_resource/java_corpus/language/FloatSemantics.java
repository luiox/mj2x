// 语料：浮点语义——f2i/d2i 截断与钳制、NaN 三值比较（fcmpl/fcmpg）、±0.0、
// Infinity、double→float 精度收缩、Math 面语义。输出确定性。
public class FloatSemantics {
    public static void main(String[] args) {
        // f2i/d2i：截断 + 越界钳制到 MIN/MAX，NaN → 0
        double[] ds = {1.9, -1.9, 1e300, -1e300, Double.NaN, 0.99, -0.99,
                       Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY};
        for (double d : ds) System.out.println("d2i " + d + " -> " + (int) d + " / " + (long) d);
        float[] fs = {1.9f, -1.9f, 3.4e38f, Float.NaN, Float.POSITIVE_INFINITY};
        for (float f : fs) System.out.println("f2i " + f + " -> " + (int) f + " / " + (long) f);

        // NaN 三值比较：NaN 参与任何有序比较为 false（FCMPL/FCMPG 同为 0 分支）
        double nan = Double.NaN, one = 1.0;
        System.out.println("nan<  " + (nan < one) + " nan>= " + (nan >= one));
        System.out.println("nan>  " + (nan > one) + " nan<= " + (nan <= one));
        System.out.println("nan== " + (nan == nan) + " nan!= " + (nan != nan));
        System.out.println("fnan< " + (Float.NaN < 1.0f) + " g-branch " + (one < Float.NaN));

        // ±0.0 与 Infinity
        double p0 = 0.0, n0 = -0.0;
        System.out.println("pm0 eq=" + (p0 == n0) + " div=" + (1.0 / n0) + " " + (1.0 / p0));
        System.out.println("inf arithmetic: " + (Double.POSITIVE_INFINITY - 1e308) + " "
                           + (Double.POSITIVE_INFINITY - Double.POSITIVE_INFINITY));
        System.out.println("fneg=" + (-0.0f) + " dneg=" + (-0.0));

        // 精度收缩与宽化
        double big = 1.0 / 3.0;
        float shrunk = (float) big;
        System.out.println("shrink=" + (shrunk == (float) (1.0 / 3.0)) + " " + (double) shrunk);
        long wide = (long) 1e18;
        System.out.println("wide=" + wide);
        float f = 16777217f;   // 2^24+1 无法精确表示
        System.out.println("f24=" + (f == 16777216f));

        // Math 面（intrinsic 直调覆盖点）
        System.out.println("floor/ceil/round " + Math.floor(2.7) + " " + Math.ceil(-2.1) + " "
                           + Math.round(2.5) + " " + Math.round(-2.5));
        System.out.println("abs " + Math.abs(-0.0) + " " + Math.abs(Integer.MIN_VALUE));
        System.out.println("sqrt/pow " + Math.sqrt(2.0) + " " + Math.pow(2.0, 0.5));
        System.out.println("minmax " + Math.min(-0.0f, 0.0f) + " " + Math.max(Float.NaN, 1.0f));

        // 除零浮点语义（无异常，得 Infinity/NaN）
        double dz = 1.0 / 0.0, nz = -1.0 / 0.0, nn = 0.0 / 0.0;
        System.out.println("fdiv0 " + dz + " " + nz + " " + nn);
        float fz = 0.0f; System.out.println("f0/0=" + (fz / fz));
    }
}
