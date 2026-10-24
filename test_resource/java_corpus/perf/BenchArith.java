// 性能语料：算术密集循环——整数混合运算 + 分支混合（解释器 dispatch 压力）。
// 只打校验和（确定性），耗时由 runner 测量。
public class BenchArith {
    public static void main(String[] args) {
        long checksum = 0;
        int seed = 12345;
        for (int iter = 0; iter < 2_000_000; iter++) {
            int v = seed + iter;
            int mix = v * 31 + (v >>> 3);
            mix ^= mix << 7;
            mix += v % 97;
            switch (mix & 3) {
                case 0: checksum += mix; break;
                case 1: checksum -= mix / 3; break;
                case 2: checksum ^= mix * 7L; break;
                default: checksum += mix & 0xFF;
            }
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
        }
        System.out.println("checksum=" + checksum);
    }
}
