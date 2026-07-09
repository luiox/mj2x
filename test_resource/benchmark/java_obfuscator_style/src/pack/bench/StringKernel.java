package pack.bench;

public final class StringKernel {
    private static final char[] MARK = new char[] {'m', 'j', '2', 'x', '-', '\u4e2d'};

    private StringKernel() {}

    public static int mix(String prefix, int value) {
        String text = new StringBuilder()
            .append(prefix)
            .append(':')
            .append(value)
            .append(':')
            .append(MARK)
            .toString();
        int score = text.length() + text.hashCode();
        if (text.startsWith(prefix) && text.contains("2x")) {
            score ^= text.charAt(0);
        }
        return score;
    }
}
