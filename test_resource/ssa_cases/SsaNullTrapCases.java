package ssa_cases;

public class SsaNullTrapCases {
    public int value;

    public int id(int input) {
        return input + 1;
    }

    public static int catchNullField() {
        try {
            SsaNullTrapCases box = null;
            return box.value;
        } catch (NullPointerException ex) {
            return 55;
        }
    }

    public static int catchNullCall() {
        try {
            SsaNullTrapCases box = null;
            return box.id(1);
        } catch (NullPointerException ex) {
            return 66;
        }
    }

    public static int catchNullArray() {
        try {
            int[] values = null;
            return values[0];
        } catch (NullPointerException ex) {
            return 77;
        }
    }
}
