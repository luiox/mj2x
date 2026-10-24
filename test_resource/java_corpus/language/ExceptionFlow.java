// 语料：异常全形态——try/catch/finally 执行顺序、finally 覆盖返回值、嵌套 try、
// 多 catch（union types）、rethrow、异常表多入口匹配、异常穿越调用栈。
public class ExceptionFlow {
    static StringBuilder trace = new StringBuilder();

    static int finallyOverrides() {
        try {
            trace.append("t");
            return 1;
        } finally {
            trace.append("f");
            // finally 内 return 覆盖 try 的返回值
            return 2;
        }
    }

    static String order(int mode) {
        trace.setLength(0);
        try {
            trace.append("[");
            if (mode == 1) throw new RuntimeException("r1");
            if (mode == 2) throw new IllegalStateException("r2");
            trace.append("ok");
        } catch (RuntimeException e) {
            trace.append("catch:").append(e.getMessage());
        } finally {
            trace.append("]");
        }
        return trace.toString();
    }

    static String multi(int x) {
        try {
            if (x == 0) throw new IllegalArgumentException("iae");
            if (x == 1) throw new IllegalStateException("ise");
            if (x == 2) throw new Exception("plain");
            return "none";
        } catch (IllegalArgumentException | IllegalStateException e) {
            // union catch：一个 handler 接两种类型
            return "union:" + e.getMessage();
        } catch (Exception e) {
            return "base:" + e.getMessage();
        }
    }

    static String rethrower(int x) throws Exception {
        try {
            throw new Exception("orig");
        } catch (Exception e) {
            if (x > 0) throw e;              // rethrow
            return "swallowed:" + e.getMessage();
        }
    }

    static String nested() {
        try {
            try {
                throw new RuntimeException("inner");
            } finally {
                trace.append("inner-f");
            }
        } catch (RuntimeException e) {
            try {
                trace.append("|outer-c");
                throw new RuntimeException("outer");
            } catch (RuntimeException e2) {
                trace.append("|outer-c2:").append(e2.getMessage());
            } finally {
                trace.append("|outer-f");
            }
        }
        return trace.toString();
    }

    static int throughStack(int depth) {
        // 异常穿越 N 层调用栈（异常表只在最外层）。注意：栈帧形态在保护后
        // 不同（native 帧不进 getStackTrace），禁止把堆栈形态当输出。
        try {
            return innerThrower(depth, 0);
        } catch (IllegalStateException e) {
            return depth * 100 + 7;
        }
    }

    static int innerThrower(int depth, int hops) {
        if (depth == 0) throw new IllegalStateException("bottom");
        return innerThrower(depth - 1, hops + 1);
    }

    public static void main(String[] args) {
        trace.setLength(0);
        System.out.println("override=" + finallyOverrides() + " trace=" + trace);
        System.out.println("order0=" + order(0));
        System.out.println("order1=" + order(1));
        System.out.println("order2=" + order(2));
        System.out.println("multi " + multi(0) + " " + multi(1) + " " + multi(2) + " " + multi(9));
        try {
            System.out.println("rethrow " + rethrower(1));
        } catch (Exception e) {
            System.out.println("rethrown-out:" + e.getMessage());
        }
        try {
            System.out.println("swallow " + rethrower(0));
        } catch (Exception e) {
            System.out.println("should-not-reach");
        }
        trace.setLength(0);
        System.out.println("nested=" + nested());
        System.out.println("stack " + throughStack(0) + " " + throughStack(5));

        // 构造器抛异常 + catch
        try {
            Object o = new Bomb();
            System.out.println("no-bomb");
        } catch (IllegalStateException e) {
            System.out.println("ctor-exc=" + e.getMessage());
        }

        // 循环内异常（异常区域 × 回边）
        int rescued = 0;
        for (int i = -2; i <= 2; i++) {
            try {
                rescued += 10 / i;
            } catch (ArithmeticException e) {
                rescued += 100;
            }
        }
        System.out.println("loop-exc=" + rescued);
    }

    static class Bomb {
        Bomb() {
            trace.append("bomb-ctor");
            throw new IllegalStateException("boom");
        }
    }
}
