// 语料：可变参数深面——空调用/展开/直传数组、泛型 varargs（T...）、装箱与
// 宽化决议混排、数组之数组（int[]...）、null 数组（NPE 面）、带前缀参数的
// varargs、varargs 递归。
public class VarargsForms {
    static int sum(int... xs) {
        int s = 0;
        for (int x : xs) s += x;
        return s;
    }

    static String show(Object... xs) {
        StringBuilder sb = new StringBuilder();
        for (Object x : xs) sb.append(x).append(',');
        return sb.toString();
    }

    static <T> T pick(T... xs) {
        return xs.length == 0 ? null : xs[xs.length - 1];
    }

    static long widen(long base, int... xs) {
        return base + sum(xs);
    }

    static int deep(int[]... xss) {
        int n = 0;
        for (int[] xs : xss) n += xs.length;
        return n;
    }

    static int direct() {
        return sum(new int[]{4, 5, 6});
    }

    static int fibva(int n, int... acc) {
        if (n <= 1) return acc.length == 0 ? n : acc[0] + n;
        return fibva(n - 1, acc) + fibva(n - 2, acc);
    }

    public static void main(String[] args) {
        System.out.println("empty " + sum() + " [" + show() + "]");
        System.out.println("spread " + sum(1, 2, 3) + " " + sum(10, 20, 30, 40));
        System.out.println("array " + direct());

        System.out.println("obj [" + show(1, "a", null, 2.5) + "]");
        System.out.println("box [" + show(Integer.valueOf(5), Boolean.TRUE) + "]");

        System.out.println("gen " + pick("a", "b", "c") + " " + pick());
        System.out.println("widen " + widen(100L, 1, 2));
        System.out.println("deep " + deep(new int[]{1, 2}, new int[]{3}, new int[]{}));

        try {
            int r = sum((int[]) null);
            System.out.println("no-npe " + r);
        } catch (NullPointerException e) {
            System.out.println("null-varargs-npe");
        }

        System.out.println("fib " + fibva(10) + " " + fibva(6, 100));
    }
}
