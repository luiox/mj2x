package protected_samples.stackc;

/**
 * StackC fixture covering object, field, array, and type-check JNI surfaces.
 *
 * <p>mjdbg can use this sample after the tiny fixture to validate trace
 * grouping across allocation, member access, primitive arrays, and String LDC.
 */
public class StackCObjectArraySample {
    public int value;
    public static int sink;

    public StackCObjectArraySample(int value) {
        this.value = value;
    }

    public int bump(int delta) {
        value += delta;
        return value;
    }

    public static int objectFieldCall(int x) {
        StackCObjectArraySample box = new StackCObjectArraySample(x);
        sink = box.value;
        return box.bump(sink + 2);
    }

    public static int primitiveArrays(int n) {
        int[] values = new int[4];
        int total = 0;
        for (int i = 0; i < values.length; i++) {
            values[i] = n + i;
            total += values[i];
        }
        return total + values.length;
    }

    public static int referenceOps(Object value) {
        if (value instanceof String) {
            return ((String) value).length();
        }
        return -1;
    }

    public static String stringLiteral() {
        return "mjdbg-stackc";
    }
}
