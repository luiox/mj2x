// ============================================================================
// NativeLoaderStub — bootstrap 分解微探针专用 stub（issue #1183，仅探针 jar）。
//
// 与 mj2x-core native_loader_postprocessor.cpp 生成的 mj2x/runtime/NativeLoader
// 同名同签名（public static synchronized void load(String,String)），供测量 jar
// 替换原类：设置 -Dmj2x.probe.skip=1 时跳过提取与 System.load（仅置 loaded），
// 使「提取」「加载」两段可被驱动分别隔离计时；未设属性时按原逻辑提取+加载
// （探针流程不使用该路径，仅为类行为完整保真）。
// ============================================================================
package mj2x.runtime;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class NativeLoader {

    private static boolean loaded = false;

    private NativeLoader() {
    }

    public static synchronized void load(String resourcePath, String tempSuffix) {
        if (loaded) return;
        if (System.getProperty("mj2x.probe.skip") != null) {
            loaded = true;
            return;
        }
        try {
            InputStream in = ClassLoader.getSystemResourceAsStream(resourcePath);
            if (in == null) throw new UnsatisfiedLinkError("mj2x native resource not found");
            File temp = File.createTempFile("mj2x-", tempSuffix);
            temp.deleteOnExit();
            FileOutputStream out = new FileOutputStream(temp);
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
}
