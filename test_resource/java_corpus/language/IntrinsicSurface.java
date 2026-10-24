// 语料：intrinsic 命中面——Math/wrapper 严格语义方法（invokestatic 直调面）、
// array.length、instanceof、checkcast、NaN 比较等 SSA 生成 intrinsic 的密度调用。
public class IntrinsicSurface {
    public static void main(String[] args) {
        // Math 严格语义（1U=严格数学生成 intrinsic 直调，禁优化）
        System.out.println("abs " + Math.abs(-3) + " " + Math.abs(-3L) + " " + Math.abs(-3.5)
                           + " " + Math.abs(-3.5f));
        System.out.println("minmax " + Math.min(2, 3) + " " + Math.max(2L, 3L) + " "
                           + Math.min(0.5f, 0.25f) + " " + Math.max(0.5, 0.25));
        System.out.println("rounds " + Math.round(2.5f) + " " + Math.round(2.5) + " "
                           + Math.rint(2.5) + " " + Math.rint(3.5));
        System.out.println("sqrt " + Math.sqrt(9.0) + " " + Math.sqrt(-1.0));
        System.out.println("pow " + Math.pow(2.0, 10.0) + " " + Math.pow(0.0, 0.0));
        System.out.println("explog " + Math.exp(1.0) + " " + Math.log(Math.E) + " "
                           + Math.log10(1000.0));
        System.out.println("trig " + Math.sin(0.0) + " " + Math.cos(0.0) + " " + Math.tan(0.0)
                           + " " + Math.atan(1.0));
        System.out.println("signum " + Math.signum(-2.5) + " " + Math.signum(0.0) + " "
                           + Math.signum(Float.NEGATIVE_INFINITY));
        System.out.println("cbrt/hypot " + Math.cbrt(27.0) + " " + Math.hypot(3, 4));

        // StrictMath（独立 intrinsic 面，结果与 Math 允许不同位型但自身确定）
        System.out.println("strict " + StrictMath.abs(-5) + " " + StrictMath.floor(2.7));

        // wrapper 严格语义（拆箱路径）
        Integer xi = Integer.valueOf(-15);
        System.out.println("wrap " + xi.intValue() + " " + Long.valueOf(-7L).longValue() + " "
                           + Double.valueOf(2.5).doubleValue() + " " + Float.valueOf(1.5f).floatValue());
        System.out.println("wcmp " + Integer.max(3, 9) + " " + Integer.min(3, 9) + " "
                           + Long.signum(-9L) + " " + Integer.signum(9) + " "
                           + Integer.signum(0));
        System.out.println("wbits " + Integer.highestOneBit(100) + " " + Integer.lowestOneBit(100)
                           + " " + Integer.bitCount(255) + " " + Long.numberOfTrailingZeros(8));
        System.out.println("wrot " + Integer.rotateLeft(0x80000001, 2) + " "
                           + Integer.rotateRight(0x80000001, 1) + " "
                           + Long.rotateLeft(1L, 63));
        System.out.println("wparse " + Integer.reverse(1) + " " + Long.reverseBytes(1L)
                           + " " + Integer.toHexString(-1));

        // instanceof/checkcast 密集（含数组类型与接口）
        Object[] xs = {new int[3], "str", Integer.valueOf(1), null, new java.util.ArrayList<>()};
        for (Object x : xs) {
            String kind = x instanceof int[] ? "int[]"
                          : x instanceof String ? "String"
                          : x instanceof Number ? "Number"
                          : x instanceof java.util.List ? "List"
                          : x == null ? "null" : "?";
            System.out.println("kind " + kind);
        }
        try {
            String bad = (String) xs[2];
            System.out.println("no-cce " + bad);
        } catch (ClassCastException e) {
            System.out.println("cce=caught");
        }

        // array.length intrinsic + 数组工具
        int[] arr = {3, 1, 2};
        System.out.println("len " + arr.length + " copy " + java.util.Arrays.toString(
            java.util.Arrays.copyOf(arr, 5)));
        java.util.Arrays.sort(arr);
        System.out.println("sorted " + java.util.Arrays.toString(arr) + " "
                           + java.util.Arrays.binarySearch(arr, 2));
        System.out.println("fill " + java.util.Arrays.toString(mk(4)));
    }

    static int[] mk(int n) {
        int[] out = new int[n];
        java.util.Arrays.fill(out, 7);
        return out;
    }
}
