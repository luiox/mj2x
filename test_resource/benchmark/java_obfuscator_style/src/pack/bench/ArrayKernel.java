package pack.bench;

public final class ArrayKernel {
    private ArrayKernel() {}

    public static int multi(int rows, int cols) {
        int[][][] data = new int[rows + 1][cols + 1][2];
        int sum = 0;
        for (int r = 0; r < data.length; r++) {
            for (int c = 0; c < data[r].length; c++) {
                data[r][c][0] = r + c;
                data[r][c][1] = r * 31 - c;
                sum += data[r][c][0] ^ data[r][c][1];
            }
        }
        return sum;
    }
}
