// 语料：文本块与转义面——多行 text block（缩进剥离）、行尾续行 \、\s 尾随
// 空格、字符串内引号；NUL 转义（常量池真实 0x00 字节 → NewStringUTF 的
// modified UTF-8 C0 80 面）、代理对字面量（4 字节 UTF-8 → 代理对转换面）、
// BMP 转义、长文本块 lines() 计数。
public class TextBlocksAndEscapes {
    static String visible(String s) {
        return s.replace("\n", "\\n").replace("\0", "\\0");
    }

    public static void main(String[] args) {
        String block = """
                line1
                line2 "quoted"
                line3\ttabbed""";
        System.out.println("b1 [" + visible(block) + "]");

        String joined = """
                part-a \
                part-b""";          // 行尾 \ 续行：换行与缩进被剥离
        System.out.println("b2 [" + visible(joined) + "]");

        String trailing = """
                end\s""";           // \s 保留尾随空格
        System.out.println("b3 [" + trailing + "|" + trailing.length() + "]");

        // NUL：LDC 常量池真实 0x00（标准 UTF-8 直存），JNI NewStringUTF 需按
        // modified UTF-8 解码（C0 80 转换面）
        String nul = "a\0b\0";
        System.out.println("nul [" + visible(nul) + "] len=" + nul.length() + " c1=" + (int) nul.charAt(1));

        // 代理对字面量（增补平面 → 常量池 4 字节 UTF-8）
        String astral = "\uD83D\uDE00z\uD83E\uDD17";
        System.out.println("astral cp=" + astral.codePointAt(0) + " len=" + astral.length()
                           + " tail=" + (int) astral.charAt(astral.length() - 1));

        String mixed = "\u0041\u00e9\u4e2d";
        System.out.println("mixed " + mixed + " " + mixed.length());

        // 长文本块（压 LDC 与 String.lines）
        String rows = """
                alpha;bravo;charlie;delta
                echo;foxtrot;golf;hotel
                india;juliet;kilo;lima
                mike;november;oscar;papa""";
        System.out.println("rows " + rows.lines().count() + " " + rows.lines().findFirst().orElse("?"));

        // 文本块参与拼接（indy makeConcat 与多行常量混排）
        String who = "x";
        String msg = """
                hi %s""".formatted(who);
        System.out.println("fmt [" + visible(msg) + "]");
    }
}
