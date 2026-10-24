// 性能语料：调用密集——虚分派/接口分派/递归调用链（invoke 压力面）。
public class BenchCalls {
    interface Op { long apply(long x); }

    static class Add implements Op {
        @Override public long apply(long x) { return x + 3; }
    }

    static class Mul implements Op {
        @Override public long apply(long x) { return x * 5; }
    }

    static class Mix implements Op {
        @Override public long apply(long x) { return x ^ (x << 1); }
    }

    static long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

    static long deep(long v, int depth) {
        if (depth == 0) return v;
        return deep(v * 3 + 1, depth - 1);
    }

    public static void main(String[] args) {
        Op[] ops = {new Add(), new Mul(), new Mix()};
        long checksum = 0;
        // 虚分派 + 接口分派 60 万次
        long v = 1;
        for (int i = 0; i < 600_000; i++) {
            v = ops[i % 3].apply(v);
            if ((v & 0xFFFFF) == 0) v = i + 1;
        }
        checksum += v;
        // 递归调用（fib 20 × 20 轮）
        for (int r = 0; r < 20; r++) checksum += fib(20);
        // 深调用链（200 层 × 2000 次）
        for (int i = 0; i < 2000; i++) checksum += deep(i, 200) % 997;
        System.out.println("checksum=" + checksum);
    }
}
