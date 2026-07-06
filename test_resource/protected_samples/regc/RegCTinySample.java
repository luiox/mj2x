package protected_samples.regc;

/**
 * Tiny RegC fixture for register-style arithmetic and control-flow samples.
 *
 * <p>The methods are intentionally small so mjdbg can compare CBackend
 * stack-shaped output with RegC virtual-register helper output.
 */
public class RegCTinySample {
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

    public static int choose(int x) {
        switch (x & 3) {
            case 0:
                return 11;
            case 1:
                return 13;
            default:
                return 17;
        }
    }
}
