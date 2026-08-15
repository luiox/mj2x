package protected_samples.llvm;

/**
 * LLVM exception-table dispatch and monitor surface fixture.
 *
 * <p>Covers the exception-region dispatch helper, handler matching, rethrow
 * paths, and monitorenter/monitorexit lowering.
 */
public class LlvmExceptionMonitorSample {
    public static int catchOwn(int x) {
        try {
            return 100 / x;
        }
        catch (ArithmeticException e) {
            return 7;
        }
    }

    public static int missCatch() {
        try {
            throw new IllegalStateException("unmatched");
        }
        catch (ArithmeticException e) {
            return 1;
        }
    }

    public static int locked(int n) {
        synchronized (LlvmExceptionMonitorSample.class) {
            return n * 2;
        }
    }

    public static String check() {
        int   a      = catchOwn(0);
        int   b      = catchOwn(10);
        String missed = "missed";
        try {
            missCatch();
        }
        catch (IllegalStateException e) {
            missed = "caught";
        }
        return "catch0=" + a + " catch10=" + b + " " + missed + " locked=" + locked(5);
    }
}
