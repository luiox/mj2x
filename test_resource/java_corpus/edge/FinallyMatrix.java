// 语料：finally 返回值矩阵——try/catch/finally 各路径 return 的 9 种组合 +
// finally 修改返回值（值捕获语义）+ finally 内抛异常覆盖。
public class FinallyMatrix {
    static StringBuilder log = new StringBuilder();

    static String path(int mode) {
        log.setLength(0);
        try {
            log.append("t");
            if (mode == 0) return "from-try";
            if (mode == 1) throw new RuntimeException("x");
            return "after";
        } catch (RuntimeException e) {
            log.append("c");
            if (mode == 9) throw new IllegalStateException("from-catch");
            return "from-catch";
        } finally {
            log.append("f");
        }
    }

    static int finallyReadsReturn(int x) {
        int result = x;
        try {
            result = x * 2;
            return result;        // 返回值在 finally 前已定格（值捕获）
        } finally {
            result = 999;         // 不影响返回
            log.append("f-ran");
        }
    }

    static int finallyThrows(int x) {
        try {
            return x;
        } finally {
            if (x > 10) throw new IllegalStateException("fin-boom");
        }
    }

    static String finallyLoop(int n) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < n; i++) {
            try {
                if (i == 2) continue;     // finally 在 continue 路径执行
                if (i == 4) break;        // finally 在 break 路径执行
                out.append(i);
            } finally {
                out.append('(').append(i).append(')');
            }
        }
        return out.toString();
    }

    public static void main(String[] args) {
        for (int mode : new int[] {0, 1, 2}) {
            log.setLength(0);
            System.out.println("path" + mode + "=" + path(mode) + " trace=" + log);
        }
        try {
            log.setLength(0);
            System.out.println("path9=" + path(9));
        } catch (IllegalStateException e) {
            System.out.println("path9-threw=" + e.getMessage() + " trace=" + log);
        }

        log.setLength(0);
        System.out.println("captured " + finallyReadsReturn(5) + " " + finallyReadsReturn(7)
                           + " trace=" + log);
        try {
            System.out.println("fin-ok " + finallyThrows(5));
            System.out.println("fin-boom " + finallyThrows(50));
        } catch (IllegalStateException e) {
            System.out.println("fin-exc=" + e.getMessage());
        }
        System.out.println("fin-loop " + finallyLoop(7));
    }
}
