// ============================================================================
// mj2x/cloader/Loader — 桥接类的可读源码版本（规范参考实现）。
//
// 权威字节码由生成器程序化构造：
//   mj2x-core/src/mj2x/postprocessor/cloader_postprocessor.cpp
//     ::buildCLoaderClassNode()
// 本文件与其保持结构同步；修改任一侧时必须同步另一侧并跑
// StackCBackendE2E / Vmc4BackendE2E 全量回归。
//
// 与源码的差异（由生成器按输入注入，Java 无法静态表达）：
//   1. 类带 ACC_SYNTHETIC，方法亦然；
//   2. <clinit> 仅在 CLI 配置了 native 资源路径时才生成，
//      RESOURCE_PATH / TEMP_SUFFIX 由发射期字符串常量内联；
//   3. 未配置资源时整个 <clinit> 不存在。
//
// 职责：RegisterNatives 动态绑定的 Java 侧载体——转换类 clinit 调
// registerNativesForClass；native 库经 NativeLoader.load 解包加载一次。
// ============================================================================
package mj2x.cloader;

import java.lang.invoke.MethodHandle;

public final class Loader {

    // 发射期内联常量的占位形态：生成器把真实路径/后缀作为字符串常量直接
    // 写入 <clinit> 的 LDC（本类不声明这两个字段；此声明仅为让参考源码
    // 可独立编译）。TEMP_SUFFIX 缺省 ".mj2x"。
    private static final String RESOURCE_PATH = "<emitted-inline:resource-path>";
    private static final String TEMP_SUFFIX   = ".mj2x";

    private Loader() {
        super(); // 不实例化
    }

    // 仅当 CLI 配置 native 资源路径时由生成器注入整个 <clinit>。
    // load 只在 Loader 首次初始化时执行一次。
    static {
        mj2x.runtime.NativeLoader.load(RESOURCE_PATH, TEMP_SUFFIX);
    }

    // RegisterNatives 入口：转换类 clinit 以 (slot, class) 调用，
    // 实现由各后端生成的 JNI_OnLoad/native 注册段提供。
    public static native void registerNativesForClass(int slot, Class<?> clazz);

    // invokedynamic 包装调用面（Phase indy）：MethodHandle.invoke 的
    // 精确形态桥，供生成代码以静态符号引用。
    public static Object invokeMethodHandle1(MethodHandle handle, Object receiver)
            throws Throwable {
        return handle.invoke(receiver);
    }
}
