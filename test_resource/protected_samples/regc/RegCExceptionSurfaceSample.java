package protected_samples.regc;

/**
 * RegC fixture covering exception-table dispatch and rethrow surfaces.
 */
public class RegCExceptionSurfaceSample {
    public static int catchOwn(int x) {
        try {
            if (x > 0) {
                throw new IllegalArgumentException();
            }
            return 1;
        } catch (IllegalArgumentException e) {
            return 2;
        }
    }

    public static int missCatch() {
        try {
            throw new IllegalStateException();
        } catch (IllegalArgumentException e) {
            return 3;
        }
    }

    public static void fail() {
        throw new IllegalArgumentException();
    }

    public static int catchCall() {
        try {
            fail();
            return 4;
        } catch (IllegalArgumentException e) {
            return 9;
        }
    }
}
