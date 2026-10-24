// 语料：字符串与装箱——indy 字符串拼接（javac 9+ 默认 StringConcatFactory，混合
// 类型）、String 全家桶操作、包装类缓存恒等、拆箱 NPE、全部 wrapper 类型。
public class StringsAndBoxing {
    public static void main(String[] args) {
        // indy concat：makeConcat 常量配方 + 动态参数（各类型混合）
        String s = "s";
        int i = 42;
        long l = 1L << 40;
        double d = 0.5;
        Object o = Integer.valueOf(7);   // Object 形态参与 concat（toString 契约）
        String nul = null;
        System.out.println("cat " + s + i + l + d + true + (char) 'X');
        System.out.println("catnull " + s + nul);
        System.out.println("catobj " + s + o);
        System.out.println("catexpr " + (i + 1) + "=" + s + (d * 2));

        // 常量折叠拼接（编译期完成，ldc 常量形态）
        final String folded = "a" + "b" + 3;
        System.out.println("folded " + folded);

        // String 全家桶
        String text = "Hello, mj2x Corpus 你好";
        System.out.println("len " + text.length() + " utf16=" + text.codePointCount(0, text.length()));
        System.out.println("idx " + text.indexOf("mj2x") + " " + text.indexOf('o') + " "
                           + text.lastIndexOf('o'));
        System.out.println("sub " + text.substring(7) + " " + text.substring(7, 11));
        System.out.println("case " + text.toLowerCase().startsWith("hello")
                           + " " + text.toUpperCase().contains("MJ2X"));
        System.out.println("cmp " + text.compareTo("Hello") + " " + "abc".compareTo("abd")
                           + " " + "b".compareTo("a"));
        System.out.println("eq " + ("x".equals(new String("x"))) + " " + ("x" == new String("x"))
                           + " " + ("x".intern() == "x"));
        System.out.println("replace " + text.replace('o', '0').replace(",", ""));
        StringBuilder sb = new StringBuilder();
        for (int k = 0; k < 5; k++) sb.append(k).append('-');
        System.out.println("sb " + sb + " rev=" + new StringBuilder("abcd").reverse());
        System.out.println("split " + java.util.Arrays.toString("a,,b".split(",")));
        System.out.println("trim " + ("  x  ".trim()) + " isEmpty " + ("".isEmpty()));

        // switch on string（hashCode 分段）
        for (String cmd : new String[] {"go", "STOP", "go", "x"}) {
            String r;
            switch (cmd) {
                case "go": r = "GO"; break;
                case "stop": r = "STOP"; break;
                default: r = "?";
            }
            System.out.print(r + " ");
        }
        System.out.println();

        // 装箱缓存恒等（-128..127）
        Integer a1 = 100, a2 = 100, b1 = 200, b2 = 200;
        System.out.println("icache " + (a1 == a2) + " " + (b1 == b2) + " eq=" + b1.equals(b2));
        Integer parsed = Integer.valueOf("123");
        System.out.println("iparse " + parsed + " " + parsed.intValue() + " " + parsed.doubleValue());
        Integer nul2 = null;
        try {
            int boom = nul2 + 1;
            System.out.println("no-npe " + boom);
        } catch (NullPointerException e) {
            System.out.println("unbox-npe=caught");
        }

        // 全 wrapper 形态
        Byte by = (byte) -5;
        Short sh = (short) 3000;
        Long lo = 10_000_000_000L;
        Float fl = 2.5f;
        Double db = -0.5;
        Character ch = 'Ω';
        Boolean bo = Boolean.FALSE;
        System.out.println("wr " + by + " " + sh + " " + lo + " " + fl + " " + db + " " + ch + " " + bo);
        System.out.println("wr-ops " + (by + sh) + " " + (lo / 3) + " " + Character.isUpperCase(ch)
                           + " " + bo.compareTo(Boolean.TRUE));
        System.out.println("parse " + Long.parseLong("-99") + " " + Double.parseDouble("1.5e2")
                           + " " + Integer.toString(255, 16));
    }
}
