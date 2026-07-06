package protected_samples.regc;

/**
 * RegC fixture covering helper-call surfaces for objects, fields, arrays, and calls.
 */
public class RegCHelperSurfaceSample {
    public int value;
    public static int sink;

    public RegCHelperSurfaceSample(int value) {
        this.value = value;
    }

    public int bump(int delta) {
        value += delta;
        return value;
    }

    public static int mix(int a, int b) {
        return a ^ (b + 7);
    }

    public static int fieldAndCall(int x) {
        RegCHelperSurfaceSample box = new RegCHelperSurfaceSample(x);
        sink = box.value;
        return mix(box.bump(2), sink);
    }

    public static int arrays(int n) {
        int[] values = new int[3];
        values[0] = n;
        values[1] = n + 1;
        values[2] = values[0] + values[1];
        return values.length + values[2];
    }

    public static int referenceOps(Object value) {
        if (value instanceof String) {
            return ((String) value).length();
        }
        return -1;
    }
}
