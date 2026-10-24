// 语料：算术边界——溢出环绕、除法边界（INT_MIN/-1、除零捕获）、移位、位运算、
// char 算术。全部输出确定性，差分对拍用（baseline vs 保护后逐字节一致）。
public class ArithEdges {
    public static void main(String[] args) {
        // 溢出环绕
        int a = Integer.MAX_VALUE;
        System.out.println("ovf+1=" + (a + 1));
        System.out.println("ovf-1=" + (Integer.MIN_VALUE - 1));
        System.out.println("mul=" + (a * 3));
        long l = Long.MAX_VALUE;
        System.out.println("lovf+1=" + (l + 1));
        System.out.println("lovfmul=" + (l * 3L));

        // 除法/取余边界
        System.out.println("min/-1=" + (Integer.MIN_VALUE / -1));
        System.out.println("min%-1=" + (Integer.MIN_VALUE % -1));
        System.out.println("lmin/-1=" + (Long.MIN_VALUE / -1L));
        int seven = 7, two = 2;
        System.out.println("7/2=" + (seven / two) + " 7%2=" + (seven % two));
        System.out.println("-7/2=" + (-seven / two) + " -7%2=" + (-seven % two));
        System.out.println("7/-2=" + (seven / -two) + " 7%-2=" + (seven % -two));
        try {
            int z = seven / (seven - seven);
            System.out.println("no-div0=" + z);
        } catch (ArithmeticException e) {
            System.out.println("div0=caught");
        }

        // 移位（变量移位取低 5/6 位）
        for (int s = 0; s < 40; s += 7) {
            System.out.println("sh s=" + s + " l=" + (a << s) + " r=" + (a >> s) + " u=" + (a >>> s));
        }
        long lv = 0x123456789ABCDEFL;
        System.out.println("lsh=" + (lv << 13) + " " + (lv >> 13) + " " + (lv >>> 13));

        // 位运算 + 复合赋值（隐式窄化）
        int b = 0xF0F0F0F0;
        b &= 0x0F0F0F0F; b |= 0x00FF00FF; b ^= 0xFF00FF00;
        System.out.println("bits=" + b);
        byte bb = 120; bb += 10;   // 窄化环绕
        System.out.println("byte=" + bb);
        short ss = 32000; ss *= 2;
        System.out.println("short=" + ss);

        // char 算术（16 位无符号语义）
        char c = 'A';
        c += 2;
        System.out.println("char=" + c + " code=" + (int) c);
        char high = 0xFFFD;
        high++;
        System.out.println("highchar=" + (int) high);

        // 一元 + long/double 混合提升
        int i = -5;
        long lp = i;
        double d = lp;
        System.out.println("promote=" + (-d) + " " + (-lp));
        float f = 1.5f;
        System.out.println("fmix=" + (f * 2 + i));
    }
}
