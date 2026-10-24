// 语料：数组全形态——8 种基本类型数组读写、引用数组、二维/锯齿、length、
// 越界/存储异常捕获、数组作为参数与返回值、clone、空数组。
public class ArraysAllTypes {
    static int sum(int[] xs) {
        int s = 0;
        for (int x : xs) s += x;
        return s;
    }

    static long[] makeLongs(int n) {
        long[] out = new long[n];
        for (int i = 0; i < n; i++) out[i] = (long) i * i;
        return out;
    }

    public static void main(String[] args) {
        // 8 种基本类型
        boolean[] bs = {true, false, true};
        byte[] bytes = {1, -1, 127, -128};
        short[] shorts = {1, -1, 32767, -32768};
        char[] chars = {'a', '中', 0x41};
        int[] ints = {5, -5, Integer.MAX_VALUE, Integer.MIN_VALUE};
        long[] longs = makeLongs(5);
        float[] floats = {1.5f, -0.25f};
        double[] doubles = {1.0 / 3, -2.5};
        System.out.println("bool " + bs[0] + bs[1] + bs[2] + " len=" + bs.length);
        System.out.println("byte " + bytes[0] + " " + bytes[1] + " " + bytes[2] + " " + bytes[3]);
        System.out.println("short " + shorts[2] + " " + shorts[3]);
        System.out.print("char ");
        for (char c : chars) System.out.print((int) c + ",");
        System.out.println();
        System.out.println("int " + sum(ints));
        StringBuilder lsb = new StringBuilder();
        for (long l : longs) lsb.append(l).append(",");
        System.out.println("long " + lsb);
        System.out.println("float " + floats[0] + " " + floats[1] + " double " + doubles[0] + " " + doubles[1]);

        // 引用数组 + null 元素
        String[] names = {"a", null, "c"};
        for (String n : names) System.out.print((n == null ? "-" : n.length()) + " ");
        System.out.println();

        // 二维与锯齿
        int[][] grid = new int[3][4];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) grid[i][j] = i * 10 + j;
        System.out.println("grid " + grid[2][3] + " " + grid[0].length + "x" + grid.length);
        int[][] jagged = {{1}, {2, 3}, {4, 5, 6}};
        int jagSum = 0;
        for (int[] row : jagged) jagSum += sum(row);
        System.out.println("jagged " + jagSum + " rows=" + jagged.length);
        int[][] half = new int[3][];
        half[0] = new int[1];
        half[2] = new int[2];
        System.out.println("halved " + (half[1] == null) + " " + half[0].length);

        // 多维（三维）
        int[][][] cube = new int[2][2][2];
        cube[1][1][1] = 42;
        System.out.println("cube " + cube[1][1][1] + " " + cube.length * cube[0].length * cube[0][0].length);

        // 越界与存储异常（catch 面）
        try {
            int x = ints[4];
            System.out.println("no-oob " + x);
        } catch (ArrayIndexOutOfBoundsException e) {
            System.out.println("oob=caught");
        }
        try {
            Object[] objs = new String[2];
            objs[0] = Integer.valueOf(1);   // ArrayStoreException
            System.out.println("no-store-exc");
        } catch (ArrayStoreException e) {
            System.out.println("store=caught");
        }

        // clone 与 arraysEqual
        int[] copy = ints.clone();
        System.out.println("clone " + (copy != ints) + " " + java.util.Arrays.equals(copy, ints));

        // 空数组
        int[] empty = new int[0];
        System.out.println("empty " + empty.length + " " + sum(empty));

        // 数组协变读取
        Number[] nums = {Integer.valueOf(1), Double.valueOf(2.5)};
        System.out.println("covariant " + nums[0] + " " + nums[1]);
    }
}
