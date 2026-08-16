package protected_samples.vmc4;

/// VMC4 red-team JNI-surface sample (companion to Vmc4IsaSurfaceSample).
///
/// Vmc4IsaSurfaceSample covers the pure-computation ISA surface only; this
/// sample exercises the JNI helper surface that capabilities.md §4 documents
/// (calls / fields / arrays / allocation / exceptions), so the mjdbg
/// observation reports and the ISA-recovery cross-check can validate the
/// per-callsite helper path (vm_call_table dispatch) rather than just the
/// dispatcher loop. Deterministic stdout so baseline/native runs compare
/// byte-for-byte.
public final class Vmc4JniSurfaceSample {
    private static int counter = 10;
    private final int factor;

    private interface Adder {
        int add(int a, int b);
    }

    private static final Adder SIMPLE = new Adder() {
        @Override
        public int add(int a, int b) { return a + b; }
    };

    public Vmc4JniSurfaceSample(int factor) { this.factor = factor; }

    /// instance field + static field load (field accessor helpers)
    public int scale(int v) { return v * factor + counter; }

    /// array load + array.length (array helpers)
    public static int sumArray(int[] a) {
        int s = 0;
        for (int i = 0; i < a.length; i++) s += a[i];
        return s;
    }

    /// allocation + array store (alloc helper + store helper)
    public static int fillSum(int n, int seed) {
        int[] a = new int[n];
        for (int i = 0; i < n; i++) a[i] = seed + i;
        return sumArray(a);
    }

    /// throw/catch inside a virtualized method (exception region table)
    public static int throwCatch(int v) {
        try {
            if (v < 0) throw new IllegalStateException("neg");
            return v * 2;
        }
        catch (IllegalStateException e) {
            return -1;
        }
    }

    /// string concat (String LDC 常量槽 + String.valueOf/invokevirtual)
    public static String greet(String who) { return "hi " + who + "!"; }

    /// interface dispatch on an object parameter (call helper, JVM 虚分派)
    public static int dispatch(Adder adder, int a, int b) { return adder.add(a, b); }

    public static void main(String[] args) {
        Vmc4JniSurfaceSample s = new Vmc4JniSurfaceSample(2);
        System.out.println("scale=" + s.scale(8));
        System.out.println("sumArray=" + sumArray(new int[] {1, 2, 3}));
        System.out.println("fillSum=" + fillSum(3, 10));
        System.out.println("throwCatch(neg)=" + throwCatch(-5));
        System.out.println("throwCatch(pos)=" + throwCatch(7));
        System.out.println("greet=" + greet("vmc4"));
        System.out.println("dispatch=" + dispatch(SIMPLE, 3, 4));
    }
}
