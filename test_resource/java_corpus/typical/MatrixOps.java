// 典型程型：矩阵运算——乘法/转置/行列式（递归余子式），密集双槽算术 + 嵌套循环。
public class MatrixOps {
    static int[][] mul(int[][] a, int[][] b) {
        int n = a.length, m = b[0].length, k = b.length;
        int[][] out = new int[n][m];
        for (int i = 0; i < n; i++)
            for (int j = 0; j < m; j++) {
                int acc = 0;
                for (int t = 0; t < k; t++) acc += a[i][t] * b[t][j];
                out[i][j] = acc;
            }
        return out;
    }

    static int[][] transpose(int[][] a) {
        int[][] out = new int[a[0].length][a.length];
        for (int i = 0; i < a.length; i++)
            for (int j = 0; j < a[0].length; j++) out[j][i] = a[i][j];
        return out;
    }

    static long det(long[][] m) {
        int n = m.length;
        if (n == 1) return m[0][0];
        if (n == 2) return m[0][0] * m[1][1] - m[0][1] * m[1][0];
        long sum = 0;
        for (int c = 0; c < n; c++) {
            long[][] minor = new long[n - 1][n - 1];
            for (int i = 1; i < n; i++) {
                int mj = 0;
                for (int j = 0; j < n; j++) {
                    if (j == c) continue;
                    minor[i - 1][mj++] = m[i][j];
                }
            }
            sum += (c % 2 == 0 ? 1 : -1) * m[0][c] * det(minor);
        }
        return sum;
    }

    static String render(int[][] a) {
        StringBuilder sb = new StringBuilder();
        for (int[] row : a) {
            for (int v : row) sb.append(v).append(',');
            sb.append('|');
        }
        return sb.toString();
    }

    public static void main(String[] args) {
        int[][] a = {{1, 2, 3}, {4, 5, 6}};
        int[][] b = {{7, 8}, {9, 10}, {11, 12}};
        System.out.println("mul " + render(mul(a, b)));
        System.out.println("transpose " + render(transpose(a)));

        long[][] m3 = {{2, 0, 1}, {1, 3, 2}, {1, 1, 1}};
        System.out.println("det3=" + det(m3));
        long[][] m4 = {{1, 0, 2, -1}, {3, 0, 0, 5}, {2, 1, 4, -3}, {1, 0, 5, 0}};
        System.out.println("det4=" + det(m4));

        // 32x32 整数矩阵校验和（规模面）
        int n = 32;
        int[][] big = new int[n][n];
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) big[i][j] = (i * 31 + j * 17) % 101;
        int checksum = 0;
        for (int[] row : mul(big, big)) for (int v : row) checksum = checksum * 31 + v;
        System.out.println("checksum=" + checksum);
    }
}
