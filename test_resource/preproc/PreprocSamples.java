package preproc;

import java.lang.invoke.MethodType;

// R1b preprocessor 验证样本。
// - methodTypeLdc: ldc MethodType → LdcPreprocessor 应展开为 fromMethodDescriptorString
// - simpleAdd: 无特殊指令，preprocessor 不应影响它
public class PreprocSamples {
    public static int simpleAdd(int a, int b) {
        return a + b;
    }
    // ldc MethodType —— 触发 LdcPreprocessor
    public static MethodType methodTypeLdc() {
        return MethodType.methodType(int.class, String.class);
    }
}
