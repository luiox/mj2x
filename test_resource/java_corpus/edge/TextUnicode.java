// 语料：Unicode 文本面——CJK/emoji 代理对、codePoint 遍历、toChars 往返、
// UTF-8 字节长度、大小写转换的多字节行为。字符串处理非 ASCII 面。
public class TextUnicode {
    public static void main(String[] args) {
        String[] texts = {
            "中文测试文本",
            "日本語のテキスト",
            "한국어 텍스트",
            "emoji: \uD83D\uDE00 tail",            // 代理对（U+1F600）
            "mixed 中a文b \uD83D\uDE0A end",
            "combining: é vs é",
        };
        for (String t : texts) {
            System.out.println("text " + t);
            System.out.println("  utf16len=" + t.length()
                               + " cp=" + t.codePointCount(0, t.length())
                               + " bytes=" + t.getBytes(java.nio.charset.StandardCharsets.UTF_8).length);
            StringBuilder cps = new StringBuilder();
            for (int i = 0; i < t.length(); ) {
                int cp = t.codePointAt(i);
                cps.append(Integer.toHexString(cp)).append(' ');
                i += Character.charCount(cp);
            }
            System.out.println("  cps " + cps);
        }

        // codePoint ↔ char[] 往返
        int[] points = {'中', 0x1F600, 'A', 0x10FFFF};
        String rebuilt = new String(points, 0, points.length);
        System.out.println("rebuilt=" + rebuilt + " len=" + rebuilt.length());
        for (int cp : points) {
            char[] pair = Character.toChars(cp);
            System.out.println("  cp " + Integer.toHexString(cp) + " chars=" + pair.length
                               + " back=" + (pair.length == 2
                                             ? Character.toCodePoint(pair[0], pair[1]) : (int) pair[0]));
        }

        // 代理对元素访问
        String emoji = "\uD83D\uDE00";
        System.out.println("surrogate " + Character.isHighSurrogate(emoji.charAt(0))
                           + " " + Character.isLowSurrogate(emoji.charAt(1))
                           + " cp=" + Integer.toHexString(emoji.codePointAt(0)));

        // 大小写与比较（多字节行为确定）
        System.out.println("upper " + "straße".toUpperCase() + " " + "中文".toUpperCase());
        System.out.println("cmp " + "中".compareTo("文") + " " + "中".compareTo("A"));
        System.out.println("case-folds " + ("İ".equalsIgnoreCase("i̇")));

        // StringBuilder 与 Unicode
        StringBuilder sb = new StringBuilder();
        sb.appendCodePoint(0x1F600).appendCodePoint('中');
        System.out.println("sb " + sb + " len=" + sb.length());
    }
}
