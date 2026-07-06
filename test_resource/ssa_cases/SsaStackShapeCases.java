package ssa_cases;

public class SsaStackShapeCases {
    public static long returnsLong(long v) {
        return v + 1L;
    }

    public static String returnsObject(int v) {
        return String.valueOf(v);
    }

    public static int discardCategory1(int v) {
        returnsObject(v);
        return v + 1;
    }

    public static int discardCategory2(int v) {
        returnsLong(v);
        return v + 2;
    }

    public static Object constructObject() {
        return new Object();
    }
}
