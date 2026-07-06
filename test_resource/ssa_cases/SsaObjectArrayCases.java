package ssa_cases;

public class SsaObjectArrayCases {
    public static int sink;

    public int value;

    public SsaObjectArrayCases(int value) {
        this.value = value;
    }

    public int bump(int delta) {
        return value + delta;
    }

    public static int helper(int a, int b) {
        return a * b + 1;
    }

    public static int fieldAndCall(int x) {
        SsaObjectArrayCases box = new SsaObjectArrayCases(x);
        sink = box.value;
        return box.bump(helper(sink, 2));
    }

    public static int arrays(int n) {
        int[] data = new int[3];
        data[0] = n;
        data[1] = data.length;
        return data[0] + data[1];
    }

    public static Object refOps(Object o) {
        if (o instanceof String) {
            return (String) o;
        }
        return null;
    }

    public static String literal() {
        return "ssa-case";
    }
}
