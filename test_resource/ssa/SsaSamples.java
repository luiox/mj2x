package ssa;

// R1a 验收样本：纯计算静态方法（R1a 不 lower 对象/数组/调用）。
// - add: 线性，无控制流。验证基本 IR 生成 + 无栈槽。
// - loopSum: for 循环。验证 block-arg phi（s/i 在循环头有 ≥2 def 源）。
// - loopCount: while 循环，验证回边 phi。
public class SsaSamples {
    public static int add(int a, int b) {
        return a + b;
    }
    public static int loopSum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i;
        }
        return s;
    }
    public static int loopCount(int n) {
        int c = 0;
        while (c < n) {
            c += 1;
        }
        return c;
    }
}
