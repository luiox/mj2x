package protected_samples.regc;

/**
 * RegC intrinsic surface fixture for mjdbg.
 *
 * Each method uses JDK calls that RegC lowers to native intrinsics rather than
 * JNI method dispatch.  The generated C should call into the runtime
 * (mj2x_math_*, _mj2x_intrinsic_parse_int, _mj2x_intrinsic_string_literal)
 * with no _mj2x_regc_call_* at these call sites.
 */
public class RegCIntrinsicSurfaceSample {
    public static int parseIntLiteral() {
        return Integer.parseInt("42");
    }

    public static int parseIntArg(String value) {
        return Integer.parseInt(value);
    }

    public static double sinAndPow(double x) {
        return Math.sin(x) + Math.pow(x, 2.0);
    }

    public static double sqrtAndCeil(double x) {
        return Math.ceil(Math.sqrt(x));
    }

    public static int mathChain(int a, int b) {
        return Math.max(a, b) + Math.abs(a - b);
    }

    public static long roundHalfAway(double x) {
        return Math.round(x);
    }
}
