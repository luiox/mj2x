// 语料：方法形态——重载决议、可变参数（展开/直传数组）、long/double 双槽参数、
// 桥接方法（泛型覆盖）、接口 default 方法与 super 调用、私有实例方法。
public class MethodsShapes {
    static int overload(int x) { return x + 1; }
    static long overload(long x) { return x + 2; }
    static double overload(double x) { return x + 3; }
    static int overload(int x, int y) { return x * 10 + y; }
    static String overload(Object x) { return "obj:" + x; }
    static String overload(String x) { return "str:" + x; }

    static int varargsSum(int... xs) {
        int s = 0;
        for (int x : xs) s += x;
        return s;
    }

    static int varargsMixed(String label, int base, int... xs) {
        return (label + base).length() + varargsSum(xs);
    }

    static long twoSlot(long a, double b, int c, long d) {
        return (long) (a + b) * 100 + c + d;
    }

    interface Box {
        int value();

        default int doubled() { return value() * 2; }

        default String doubledPlusSuper() {
            // 接口内 static 方法与 default 的组合形态
            return tag() + doubled();
        }

        static String tag() { return "iface"; }
    }

    static class Base implements Box {
        @Override public int value() { return 21; }
    }

    // 泛型覆盖产生 bridge method
    static class Cell<T> {
        T v;
        void set(T v) { this.v = v; }
        T get() { return v; }
    }

    static class IntCell extends Cell<Integer> {
        @Override void set(Integer v) { super.set(v == null ? 0 : v + 1); }
    }

    private int secret(int x) { return x * 3; }   // 私有实例方法（invokevirtual 特化路径）
    int expose(int x) { return secret(x) + secret(1); }

    public static void main(String[] args) {
        System.out.println(overload(1) + " " + overload(1L) + " " + overload(1.0));
        System.out.println(overload(2, 3));
        System.out.println(overload((Object) "x") + " " + overload("x"));

        System.out.println("va " + varargsSum() + " " + varargsSum(1) + " "
                           + varargsSum(1, 2, 3, 4, 5));
        int[] arr = {9, 8, 7};
        System.out.println("va-arr " + varargsSum(arr));
        System.out.println("va-mixed " + varargsMixed("n", 4, 1, 2, 3) + " "
                           + varargsMixed("z", 0));

        System.out.println("twoslot " + twoSlot(10L, 0.5, 3, 4L) + " " + twoSlot(-1L, -0.25, 0, -2L));

        Base b = new Base();
        System.out.println("iface " + b.value() + " " + b.doubled() + " "
                           + b.doubledPlusSuper() + " " + Box.tag());

        Cell<Integer> c = new IntCell();
        c.set(10);   // 经 bridge method 分派
        c.set(null);
        System.out.println("bridge " + c.get());

        MethodsShapes m = new MethodsShapes();
        System.out.println("private " + m.expose(5));

        // 递归（含相互递归）
        System.out.println("fib " + fib(15));
        for (int i = 0; i <= 6; i++) System.out.print(isEven(i) ? "E" : "O");
        System.out.println();
    }

    static long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    static boolean isEven(int n) { return n == 0 || isOdd(n - 1); }
    static boolean isOdd(int n) { return n != 0 && isEven(n - 1); }
}
