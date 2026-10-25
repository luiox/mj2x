public class ExceptionEntryState {
    public static int straight(int seed) {
        int state = seed * 3;          // 块外初值（非零）
        try {
            state = state + 10 / 0;    // 必抛：state 的新定义未完成
        } catch (ArithmeticException e) {
            return state + 1000;       // 期望读到抛点前的 state
        }
        return -1;
    }
    public static void main(String[] args) {
        System.out.println("straight=" + straight(7));
        int acc = 5;
        for (int i = 0; i < 1; i++) {
            try { acc = acc + 10 / 0; }
            catch (ArithmeticException e) { acc = acc + 100; }
        }
        System.out.println("loop=" + acc);
    }
}
