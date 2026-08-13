package ssa_cases;

public class SsaExceptionPrecisionCases {
    // Protected-range precision: instructions before the try (x*2) are outside
    // the protected range; the try ends with a non-terminator (y++), so the
    // end label sits on a block boundary. Division by zero inside the try is
    // caught.
    public static int semiCatch(int x) {
        int y = x * 2;
        try {
            y = y / x;
            y++;
        } catch (ArithmeticException e) {
            return -1;
        }
        return y;
    }

    // The throwing instruction after the try (x / 0) is outside the protected
    // range: the exception must propagate, not be caught.
    public static int semiAfter(int x) {
        try {
            x /= 1;
        } catch (ArithmeticException e) {
            return -1;
        }
        return x / 0;
    }
}
