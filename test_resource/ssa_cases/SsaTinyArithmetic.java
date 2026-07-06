package ssa_cases;

public class SsaTinyArithmetic {
    public static int addMul(int a, int b) {
        return (a + b) * 3 - (a & b);
    }

    public static long wideMix(long a, int b) {
        long shifted = a << 2;
        return shifted + b;
    }

    public static int castCompare(double value) {
        int narrowed = (int) value;
        if (narrowed >= 7) {
            return narrowed;
        }
        return -narrowed;
    }

    public static int loopSum(int n) {
        int sum = 0;
        for (int i = 0; i < n; i++) {
            sum += i;
        }
        return sum;
    }

    public static int choose(int v) {
        switch (v) {
            case 0:
                return 11;
            case 1:
                return 13;
            case 5:
                return 17;
            default:
                return 19;
        }
    }
}
