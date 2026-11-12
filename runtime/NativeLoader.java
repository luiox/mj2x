// ============================================================================
// mj2x/runtime/NativeLoader — 引导类的可读源码版本（规范参考实现）。
//
// 权威字节码由生成器程序化构造：
//   mj2x-core/src/mj2x/postprocessor/native_loader_postprocessor.cpp
//     ::buildNativeLoaderClass()
// 本文件与其保持结构同步；修改任一侧时必须同步另一侧并跑
// CompileRegCJarWithEncryptedNativeLibRunsJvm / StackCBackendE2E 全量回归。
// 与 cloader/Loader.java 同一约定：本文件不参与任何构建目标，仅为可读
// 参照（loader 的 clinit 前插调用面见 mj2x-core native_loader_call.hpp）。
//
// 职责：把 JAR 内嵌的 native 库资源解包到临时文件并 System.load 加载一次
// （ACC_SYNCHRONIZED 幂等，loaded 标记防重复）。
//
// 与字节码的注入差异：解密初始密钥由生成器按 xorKey != 0 ? xorKey :
// 0x9E3779B9 注入（xorKey 为 0 的明文形态同样落到固定种子）；本参考源
// 硬编码固定种子形态，加密形态的取值以生成器发射为准。
//
// JDK 24+ native 访问声明（issue #58）：
//   - System.load 自 JDK 24 起是受限方法，未声明允许时 JVM 打印 restricted
//     method 告警；按 JDK 公告，未来版本未声明时会直接失败。
//   - 可执行 JAR（java -jar 启动）：JDK 25 起主清单声明
//     Enable-Native-Access: ALL-UNNAMED 即免告警。打包侧在本仓
//     mj2x-cli embedNativeLibraryInJar 写入/合并该属性。本机实测
//     （JDK 25.0.3）：无属性 4 行告警，有属性无告警；JDK 8u162/17.0.20/
//     21.0.11 忽略该属性，加载行为不变。
//   - 库形态（classpath 启动）：主清单属性不生效，实测 classpath 启动仍
//     告警。部署方需在命令行声明：
//       java --enable-native-access=ALL-UNNAMED -cp app.jar ...
//     （或按需收紧 --illegal-native-access）。
//   - 替代加载路径评估：jar 内嵌 JNI 库的加载入口只有 System.load
//     （System.loadLibrary 不接受任意路径），声明机制就是 JDK 提供的
//     正面路径，故不实现替代加载路径。
// ============================================================================
package mj2x.runtime;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class NativeLoader {

    private static boolean loaded = false;

    private NativeLoader() {
    }

    // 与字节码逐段对应：查 loaded 幂等 → getSystemResourceAsStream（缺失抛
    // UnsatisfiedLinkError）→ newTempFile 落盘（字节码内是 ThreadLocalRandom
    // 随机名 + createNewFile 原子独占创建，不走 File.createTempFile，避开其
    // SecureRandom 冷启动成本，见 native_loader_postprocessor.cpp 内注释）→
    // 资源名以 .enc 结尾时按 xorshift32 密钥流逐字节解密 → System.load →
    // loaded = true。IOException 统一包成 UnsatisfiedLinkError。
    public static synchronized void load(String resourcePath, String tempSuffix) {
        if (loaded) return;
        try {
            InputStream in = ClassLoader.getSystemResourceAsStream(resourcePath);
            if (in == null) throw new UnsatisfiedLinkError("mj2x native resource not found");
            File   temp = newTempFile(tempSuffix);
            FileOutputStream out = new FileOutputStream(temp);
            temp.deleteOnExit();
            byte[]  buf     = new byte[8192];
            boolean decrypt = resourcePath.endsWith(".enc");
            int     state   = 0x9E3779B9;
            int     n;
            while ((n = in.read(buf)) >= 0) {
                if (decrypt) {
                    for (int i = 0; i < n; i++) {
                        state ^= state << 13;
                        state ^= state >>> 17;
                        state ^= state << 5;
                        buf[i] = (byte) (buf[i] ^ (state & 0xFF));
                    }
                }
                out.write(buf, 0, n);
            }
            out.close();
            in.close();
            System.load(temp.getAbsolutePath());
            loaded = true;
        } catch (java.io.IOException e) {
            throw new UnsatisfiedLinkError(e.toString());
        }
    }

    // 字节码内联发射的私有助手（32 次重试原子创建，防碰撞/防抢占）；此处
    // 仅示意形态，权威实现以字节码为准。
    private static File newTempFile(String tempSuffix) throws java.io.IOException {
        File candidate;
        do {
            candidate = new File(System.getProperty("java.io.tmpdir"),
                                 "mj2x" + Long.toHexString(
                                     java.util.concurrent.ThreadLocalRandom.current().nextLong())
                                     + tempSuffix);
        } while (!candidate.createNewFile());
        return candidate;
    }
}
