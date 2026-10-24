// 语料：分支与循环全形态——全部 int 比较指令、引用 eq/ne、null 分支、
// 条件表达式嵌套、短路副作用、label break/continue、回边携带活性。
public class BranchLoopMatrix {
    static String cmpInts(int a, int b) {
        // 覆盖 if_icmplt/le/gt/ge 全部分支形态
        if (a < b) return "lt";
        if (a <= b) return "le";
        if (a > b) return "gt";
        return "ge";
    }

    static String cmpLongs(long a, long b) {
        if (a == b) return "eq";
        if (a != b && a > b) return "gt";
        return "lt";
    }

    public static void main(String[] args) {
        int[][] pairs = {{1, 2}, {2, 1}, {2, 2}, {-5, 5}, {5, -5}, {0, 0}};
        for (int[] p : pairs) {
            System.out.println(p[0] + "," + p[1] + " -> " + cmpInts(p[0], p[1]));
        }
        System.out.println("long " + cmpLongs(5L, 5L) + " " + cmpLongs(1L << 40, 3L));

        // 引用相等与 null 分支（ifnull/ifnonnull/if_acmpeq/if_acmpne）
        String s = new String("x"), t = s, n = null;
        System.out.println("acmp " + (s == t) + " " + (s == new String("x")) + " " + (n == null));
        Object o = n;
        if (o == null) System.out.println("ifnull hit");
        if (s != null) System.out.println("ifnonnull hit");

        // 短路副作用（分支边不执行）
        int[] calls = {0};
        java.util.function.Supplier<Boolean> side = () -> { calls[0]++; return true; };
        boolean sc = false && side.get() || side.get();
        System.out.println("shortcircuit=" + sc + " calls=" + calls[0]);

        // 三元嵌套
        int grade = 87;
        String g = grade > 89 ? "A" : grade > 79 ? "B" : grade > 69 ? "C" : "F";
        System.out.println("grade=" + g);

        // label break/continue（多层跳出）
        int found = -1;
        outer:
        for (int i = 0; i < 5; i++) {
            for (int j = 0; j < 5; j++) {
                if (i * j == 12) { found = i * 10 + j; break outer; }
                if (j == 3) continue outer;
            }
        }
        System.out.println("labeled=" + found);

        // do-while（回边携带）
        int x = 100, steps = 0;
        do {
            x = x % 2 == 0 ? x / 2 : 3 * x + 1;
            steps++;
        } while (x != 1);
        System.out.println("collatz steps=" + steps);

        // 无限 for + break（循环不变量跨回边）
        int sum = 0, i2 = 0;
        for (;;) {
            sum += i2;
            if (++i2 > 9) break;
        }
        System.out.println("infinite-for=" + sum);

        // while(true) + 多出口 + continue
        int exits = 0;
        while (true) {
            exits++;
            if (exits % 2 == 0) continue;
            if (exits > 6) break;
        }
        System.out.println("multiexit=" + exits);
    }
}
