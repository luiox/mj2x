// 语料：multi-catch 与精确重抛——共享 catch 体的类型判定、catch 参数隐式
// final、precise rethrow（throw e 保留精确静态类型）、异常链构造、多 catch
// 分派顺序。
public class MultiCatchRethrow {
    static class AppErrorA extends RuntimeException {
        AppErrorA(String m) { super(m); }
    }
    static class AppErrorB extends RuntimeException {
        AppErrorB(String m) { super(m); }
    }
    static class AppErrorC extends RuntimeException {
        AppErrorC(String m) { super(m); }
    }

    static void f(int sel) {
        if (sel == 0) throw new AppErrorA("a");
        if (sel == 1) throw new AppErrorB("b");
        if (sel == 2) throw new AppErrorC("c");
        if (sel == 3) throw new IllegalStateException("ise");
    }

    static String multi(int sel) {
        try {
            f(sel);
            return "clean";
        } catch (AppErrorA | AppErrorB e) {
            return "mc " + e.getMessage() + " " + e.getClass().getSimpleName();
        } catch (RuntimeException e) {
            return "re " + e.getMessage();
        }
    }

    // 精确重抛：catch (final A | B e) throw e——调用方按精确类型分派
    static void rethrow(int sel) throws AppErrorA, AppErrorB {
        try {
            f(sel);
        } catch (final AppErrorA | AppErrorB e) {
            throw e;
        }
    }

    static String caller(int sel) {
        try {
            rethrow(sel);
            return "ok";
        } catch (AppErrorA e) {
            return "A:" + e.getMessage();
        } catch (AppErrorB e) {
            return "B:" + e.getMessage();
        }
    }

    public static void main(String[] args) {
        for (int i = 0; i < 4; i++) System.out.println(i + ": " + multi(i));
        System.out.println(caller(0) + " " + caller(1));

        try {
            f(0);
        } catch (AppErrorA | AppErrorB e) {
            System.out.println("type " + (e instanceof AppErrorA) + (e instanceof AppErrorB)
                               + (e instanceof AppErrorC));
        }

        try {
            try {
                f(1);
            } catch (AppErrorB e) {
                throw new AppErrorA("wrapped:" + e.getMessage());
            }
        } catch (AppErrorA e) {
            System.out.println("chain " + e.getMessage() + " cause=" + e.getCause());
        }
    }
}
