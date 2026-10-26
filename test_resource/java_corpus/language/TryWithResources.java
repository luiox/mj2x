// 语料：try-with-resources 全形态——单/多资源逆序 close、close 抛异常（主异常
// + suppressed）、资源构造器抛异常（后续资源不 open、先开资源照常 close）、
// 嵌套 t-w-r、effectively-final 资源（JDK9）与资源复用二次 close。
public class TryWithResources {
    static class Res implements AutoCloseable {
        final String name;
        final boolean throwOnClose;

        Res(String name, boolean throwOnClose) {
            this.name = name;
            this.throwOnClose = throwOnClose;
            System.out.println("open " + name);
        }

        @Override public void close() {
            System.out.println("close " + name);
            if (throwOnClose) throw new IllegalStateException("close-" + name);
        }
    }

    static class CtorFail extends Res {
        CtorFail() {
            super("ctor-fail", false);
            throw new IllegalArgumentException("ctor");
        }
    }

    static String run() {
        try (Res a = new Res("A", false); Res b = new Res("B", false)) {
            System.out.println("body");
            return "ok";
        }
    }

    static String closeThrows() {
        try (Res a = new Res("X", true)) {
            return "body-x";
        } catch (IllegalStateException e) {
            return "caught " + e.getMessage() + " suppressed=" + e.getSuppressed().length;
        }
    }

    static String primaryAndCloseThrow() {
        try (Res a = new Res("Y", true)) {
            throw new IllegalArgumentException("primary");
        } catch (IllegalArgumentException e) {
            StringBuilder sb = new StringBuilder(e.getMessage());
            for (Throwable t : e.getSuppressed()) sb.append(';').append(t.getMessage());
            return sb.toString();
        }
    }

    static String twoCloseThrow() {
        try (Res a = new Res("P", true); Res b = new Res("Q", true)) {
            return "two";
        } catch (IllegalStateException e) {
            return e.getMessage() + " sup=" + e.getSuppressed().length + "/"
                   + e.getSuppressed()[0].getMessage();
        }
    }

    static String nested() {
        try (Res o = new Res("O", false)) {
            try (Res i = new Res("I", true)) {
                return "inner-body";
            } catch (IllegalStateException e) {
                return "inner-caught";
            }
        }
    }

    static String ctorThrows() {
        try (Res a = new Res("ok1", false); Res b = new CtorFail()) {
            return "unreachable";
        } catch (IllegalArgumentException e) {
            return "ctor-exc " + e.getMessage();
        }
    }

    static String effFinal() {
        Res r = new Res("E", false);
        try (r) {
            System.out.println("eff-body");
        }
        try (r) {
            System.out.println("reuse-body");
        }
        return "eff";
    }

    public static void main(String[] args) {
        System.out.println("r1 " + run());
        System.out.println("r2 " + closeThrows());
        System.out.println("r3 " + primaryAndCloseThrow());
        System.out.println("r4 " + twoCloseThrow());
        System.out.println("r5 " + nested());
        System.out.println("r6 " + ctorThrows());
        System.out.println("r7 " + effFinal());
    }
}
