package protected_samples.regc;

/**
 * Small committed benchmark fixture for RegC intrinsic/cache measurements.
 *
 * The goal is not wall-clock benchmarking inside unit tests.  The generator
 * records structural metrics from the generated C so mj2x changes can compare
 * JNI helper surface, cache declarations, and source size without depending on
 * an external TEST.jar.
 */
public class RegCIntrinsicBenchmarkSample {
    public static int stringReadHotPath(String value, String needle) {
        int score = 0;
        score += value.length();
        score += value.charAt(0);
        score += value.hashCode();
        score += value.isEmpty() ? 1 : 0;
        score += value.startsWith(needle) ? 3 : 5;
        score += value.endsWith(needle) ? 7 : 11;
        score += value.indexOf(needle);
        score += value.contains(needle) ? 13 : 17;
        return score;
    }

    public static String builderChars(char[] chars, int value) {
        return new StringBuilder().append("v=").append(value).append(':').append(chars).toString();
    }

    public static int parseAndCompare(String lhs, String rhs) {
        return Integer.compare(Integer.parseInt(lhs), Integer.parseInt(rhs)) +
               Float.compare(-0.0f, 0.0f) +
               Double.compare(Double.NaN, 1.0d);
    }

    public static Object multiArray(int rows, int cols) {
        String[][] values = new String[rows][cols];
        values[0][0] = "seed";
        return values;
    }
}
