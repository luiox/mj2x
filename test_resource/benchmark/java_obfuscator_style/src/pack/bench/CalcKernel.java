package pack.bench;

public final class CalcKernel {
    private CalcKernel() {}

    public static int mix(int a, int b) {
        int max = a > b ? a : b;
        int min = a < b ? a : b;
        int x = max - min;
        x ^= Integer.compare(a, b);
        x += Long.compare((long) a * 17L, (long) b * 11L);
        x += Float.compare((float) a, (float) b);
        x += Double.compare((double) (a & 1023), (double) (b & 255));
        x += Integer.parseInt(Integer.toString((a ^ b) & 4095));
        return Integer.rotateLeft(x, a & 7) ^ b;
    }
}
