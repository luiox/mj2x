// 语料：异常 × 回边 × 活性交叉——循环携带值在异常分派后必须存活、catch 内
// 使用循环累计值、finally 与回边组合、嵌套循环异常（寄存器分配 + 异常区域的
// 干扰面）。
public class ExcLiveLoop {
    static long run(int n, int boomAt) {
        long carried = 1;
        int stride = 3;
        for (int i = 1; i <= n; i++) {
            try {
                carried = carried * stride + i;
                if (i == boomAt) {
                    // 抛出时 carried/stride/i 都必须跨异常区域存活
                    throw new IllegalStateException("boom@" + i);
                }
                stride += 2;
            } catch (IllegalStateException e) {
                // 使用循环携带值恢复
                carried = carried / 2 + 17;
                stride = 5;
            }
        }
        return carried * 100 + stride;
    }

    static long nested(int n, int boomAt) {
        long acc = 0;
        for (int i = 0; i < n; i++) {
            long inner = i;
            for (int j = 0; j < n; j++) {
                try {
                    inner = inner * 3 + j;
                    if (i * n + j == boomAt) throw new ArithmeticException("deep");
                    acc += inner;
                } catch (ArithmeticException e) {
                    acc += inner / 2;    // 活值在 catch 内继续消费
                }
            }
        }
        return acc;
    }

    static String finallyAcrossLoop(int n) {
        StringBuilder out = new StringBuilder();
        long total = 0;
        for (int i = 0; i < n; i++) {
            try {
                if (i % 3 == 2) throw new RuntimeException("skip");
                total += i;
                out.append(i).append(' ');
            } catch (RuntimeException e) {
                out.append('-');
            } finally {
                if (i == n - 1) out.append(" total=").append(total);
            }
        }
        return out.toString();
    }

    public static void main(String[] args) {
        for (int boom : new int[] {-1, 3, 6, 9}) {
            System.out.println("run boom=" + boom + " -> " + run(8, boom));
        }
        System.out.println("nested " + nested(4, -1) + " " + nested(4, 7));
        System.out.println("fin-loop " + finallyAcrossLoop(8));

        // 异常对象本身跨回边携带
        RuntimeException pending = null;
        int count = 0;
        for (int i = 0; i < 5; i++) {
            try {
                if (i % 2 == 1) throw new RuntimeException("e" + i);
                count += i;
            } catch (RuntimeException e) {
                if (pending == null) pending = e;
            }
        }
        System.out.println("pending " + (pending != null) + " " + count);
    }
}
