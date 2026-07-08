package protected_samples.stackc;

/**
 * Tiny StackC fixture for mjdbg smoke traces.
 *
 * <p>The methods intentionally stay small so a debugger can compare Java
 * entry/exit events with the generated native arithmetic and branch bodies.
 */
public class StackCTinySample {
    public static int arithmetic(int a, int b) {
        int mixed = (a + b) * 3;
        return mixed ^ (a << 1);
    }

    public static int branchLoopSwitch(int n) {
        int acc = 0;
        for (int i = 0; i < n; i++) {
            if ((i & 1) == 0) {
                acc += i * 2;
            } else {
                acc -= i;
            }
        }
        switch (acc & 3) {
            case 0:
                return acc + 11;
            case 1:
                return acc + 13;
            default:
                return acc + 17;
        }
    }

    public static long wideMath(long seed, double value) {
        long narrowed = (long) value;
        return (seed << 3) + narrowed;
    }
}
