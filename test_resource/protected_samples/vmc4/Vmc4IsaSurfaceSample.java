package protected_samples.vmc4;

/// VMC4 red-team cross-check sample (issue #762 / N-7).
///
/// Pure-computation method set: fib/compute/mix/scale/cmp cover the
/// COPY/UNARY/BINARY/COMPARE/CONVERT/SUPER_INC/SUPER_DEC/BRANCH/JUMP/RETURN/
/// HALT semantic surface (depending on the mj2x VMC4 encoder's folding and
/// constant propagation), with no JNI helper surface (no field/array/call/
/// alloc/intrinsic). Cross-check criterion: trace observation must agree with
/// the ISA-recovery report's handler semantics labeling.
public final class Vmc4IsaSurfaceSample {
    public static int fib(int n) {
        if (n <= 1) return n;
        return fib(n - 1) + fib(n - 2);
    }
    public static int compute(int a, int b) {
        int x = a * 3 + 7;
        int y = (b - 1) * 2;
        return (x > y) ? x - y : y - x;
    }
    public static long mix(long a, long b) {
        long t = a ^ (b << 3);
        return t + 42;
    }
    public static double scale(double v, int k) {
        return v * k + 0.5;
    }
    public static boolean cmp(int a, int b) {
        return a >= b && (a & 1) == 0;
    }
}
