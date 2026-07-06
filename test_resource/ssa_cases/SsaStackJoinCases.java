package ssa_cases;

public class SsaStackJoinCases {
    public static int booleanTernary(boolean flag) {
        return flag ? 11 : 3;
    }

    public static int intTernary(int value) {
        return (value & 1) != 0 ? value + 7 : value - 5;
    }

    public static Object referenceTernary(boolean flag, Object left, Object right) {
        return flag ? left : right;
    }
}
