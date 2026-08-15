package protected_samples.llvm;

/**
 * Tiny LLVM IR fixture: register-style arithmetic, array access, and control flow.
 *
 * <p>Methods are intentionally small so mjdbg can compare LLVM textual IR with
 * StackC stack-shaped C and RegC virtual-register helper output.
 */
public class LlvmTinySample {
    public static int addMul(int a, int b) {
        return (a + b) * 3;
    }

    public static int loopSum(int n) {
        int acc = 0;
        for (int i = 0; i < n; i++) {
            acc += i;
        }
        return acc;
    }

    public static int maxOf(int a, int b) {
        return a > b ? a : b;
    }

    public static int pick(int x) {
        if (x < 0) {
            return -x;
        }
        return x * 2;
    }

    public static int absDiff(int a, int b) {
        return Math.abs(a - b);
    }

    public static String check() {
        return "addMul=" + addMul(2, 5) + " loopSum=" + loopSum(10) + " maxOf=" + maxOf(3, 9)
            + " pick=" + pick(-4) + " absDiff=" + absDiff(3, 11);
    }
}
