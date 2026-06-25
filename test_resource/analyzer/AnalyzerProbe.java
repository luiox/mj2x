package analyzer;

/**
 * R1 analyzer-verification probe: bytecodes with control flow that MUST produce
 *   - BasicInterpreter frame merges at the loop header (multiple incoming stacks)
 *   - SourceInterpreter multi-element insns_ sets (phi-like reaching defs) for the
 *     induction variable at the loop header
 *
 * Hand-analyzed expected behavior (see analyzer_probe_test.cpp assertions):
 *   sumTo(int n):  loop header has local1 (s) with >=2 producing instructions
 *                  (ICONST_0 init + IADD inside loop) => a real phi.
 *   gcd(int a,int b): while-loop merge; local1/local2 each get >=2 sources.
 */
public class AnalyzerProbe {
    public static int sumTo(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i;
        }
        return s;
    }

    public static int gcd(int a, int b) {
        while (b != 0) {
            int t = b;
            b = a % b;
            a = t;
        }
        return a;
    }

    public static int abs(int x) {
        if (x < 0) {
            return -x;
        }
        return x;
    }
}
