package protected_samples.llvm;

/**
 * LLVM indy / intrinsic surface fixture.
 *
 * <p>javac compiles string concatenation and the arraycopy call into shapes the
 * pipeline expands before LLVM lowering:
 *
 * <ul>
 *   <li>string concat -> invokedynamic StringConcatFactory (expanded by the
 *       InvokeDynamicPreprocessor into StringBuilder bytecode);</li>
 *   <li>System.arraycopy -> plain JNI static call for the LLVM backend
 *       (the arraycopy intrinsic is RegC-only);</li>
 *   <li>Integer.TYPE / String.class -> class-literal LDC (getstatic TYPE /
 *       LDC class) emitted through mj2x_llvm_class_const.</li>
 * </ul>
 */
public class LlvmIndySurfaceSample {
    public static String concat(String tag, int value) {
        return "[" + tag + "]=" + value;
    }

    public static int copySlice(int[] src, int offset, int count) {
        int[] dst = new int[count];
        System.arraycopy(src, offset, dst, 0, count);
        int sum = 0;
        for (int v : dst) {
            sum += v;
        }
        return sum;
    }

    public static Class<?> intType() {
        return Integer.TYPE;
    }

    public static Class<?> stringType() {
        return String.class;
    }

    public static String check() {
        boolean types = intType() == Integer.TYPE && stringType() == String.class;
        return concat("k", 7) + " slice=" + copySlice(new int[] {1, 2, 3, 4}, 1, 2)
            + " types=" + types;
    }
}
