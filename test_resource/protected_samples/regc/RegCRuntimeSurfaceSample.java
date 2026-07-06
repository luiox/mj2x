package protected_samples.regc;

/**
 * RegC/R3 fixture for helpers that have moved behind the runtime ABI.
 */
public class RegCRuntimeSurfaceSample {
    public static int divOrCatch(int a, int b) {
        try {
            return a / b;
        } catch (ArithmeticException ex) {
            return 77;
        }
    }

    public static int remOrCatch(int a, int b) {
        try {
            return a % b;
        } catch (ArithmeticException ex) {
            return 88;
        }
    }

    public static long longDiv(long a, long b) {
        return a / b;
    }
}
