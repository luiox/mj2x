// ============================================================================
// BootstrapProbe — mj2x runtime bootstrap 四段分解微探针驱动（issue #1183）。
//
// 每种模式独立冷 JVM 运行（外层 bootstrap_probe.ps1 负责多轮取样与统计），
// 段内 System.nanoTime 计时并打印 `SEG <mode> <nanos>`（reg_k 另打逐类行）。
//
// 模式与被测段（对应 performance_evaluation_spec §3.5 分解表）：
//   full        污染 jar：timed Class.forName(首个转换类) —— 端到端 bootstrap
//               （提取 + System.load/JNI_OnLoad + 首类注册），作为总和锚点。
//   extract     污染 jar：驱动侧按 NativeLoader 生成字节码逐调用复刻提取段
//               （getSystemResourceAsStream → createTempFile/deleteOnExit →
//               8192 缓冲读循环（.enc 时 xorshift32 逐字节解密）→ 写出 → close），
//               不触碰 System.load —— 段 1。
//   load        stub jar + -Dmj2x.probe.skip=1：不计时预提取后 timed
//               System.load（LoadLibrary+重定位+CRT+JNI_OnLoad[填表+FindClass
//               (Loader)→Loader 类定义/初始化+桥接 RegisterNatives]）—— 段 2
//               （含 Loader 类定义）。
//   load_predef 同 load 但先不计时 Class.forName("mj2x.cloader.Loader")，
//               timed System.load 时 Loader 已定义 —— 段 2 减 Loader 定义；
//               (load − load_predef) 即 Loader 类定义+初始化成本 —— 段 4（RegC
//               产物无独立 DefineClass 段，hidden-clinit DefineClass 为 StackC 专有）。
//   reg_k       stub jar + skip：不计时预提取+System.load 后，timed 依次
//               Class.forName(前 k 个转换类)（-Dmj2x.probe.k）—— 段 3 逐类
//               注册（桥接分派 + MonitorEnter + RegisterNatives）。k 递增序列
//               回答「RegisterNatives 是否随 JNI 方法数线性」。
//
// 用法：java [-Dmj2x.probe.skip=1] [-Dmj2x.probe.k=N]
//           -cp <jar>;<本类目录> BootstrapProbe <mode> <resourcePath> [class...]
// ============================================================================
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Arrays;

public class BootstrapProbe {

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: BootstrapProbe <mode> <resourcePath> [class...]");
            System.exit(2);
        }
        String mode         = args[0];
        String resourcePath = args[1];
        String[] classes    = Arrays.copyOfRange(args, 2, args.length);

        switch (mode) {
            case "full": {
                long t0 = System.nanoTime();
                Class.forName(classes[0]);
                long t1 = System.nanoTime();
                System.out.println("SEG full " + (t1 - t0));
                break;
            }
            case "extract": {
                long t0 = System.nanoTime();
                File f = extractReplica(resourcePath, ".dll");
                long t1 = System.nanoTime();
                if (!f.exists() || f.length() == 0) throw new IllegalStateException("extract empty");
                System.out.println("SEG extract " + (t1 - t0));
                break;
            }
            case "extract_new": {
                // 与 #1183 改造后生成字节码同构的提取复刻（newTempFile:
                // ThreadLocalRandom 随机名 + createNewFile 原子独占，无
                // SecureRandom 冷启动税）。legacy extract 模式保留给改造前产物。
                long t0 = System.nanoTime();
                File f = extractReplicaNew(resourcePath, ".dll");
                long t1 = System.nanoTime();
                if (!f.exists() || f.length() == 0) throw new IllegalStateException("extract empty");
                System.out.println("SEG extract_new " + (t1 - t0));
                break;
            }
            case "load": {
                extractReplica(resourcePath, ".dll");   // 不计时
                File f = lastExtract;
                long t0 = System.nanoTime();
                System.load(f.getAbsolutePath());
                long t1 = System.nanoTime();
                System.out.println("SEG load " + (t1 - t0));
                break;
            }
            case "load_predef": {
                extractReplica(resourcePath, ".dll");   // 不计时
                Class.forName("mj2x.cloader.Loader");   // 不计时：Loader 类提前定义
                long t0 = System.nanoTime();
                System.load(lastExtract.getAbsolutePath());
                long t1 = System.nanoTime();
                System.out.println("SEG load_predef " + (t1 - t0));
                break;
            }
            case "reg_k": {
                runRegisterScaled(System.getProperty("mj2x.probe.k", String.valueOf(classes.length)),
                                  resourcePath, classes);
                break;
            }
            default:
                if (mode.startsWith("reg_")) {
                    runRegisterScaled(mode.substring("reg_".length()), resourcePath, classes);
                    break;
                }
                System.err.println("unknown mode: " + mode);
                System.exit(2);
        }
    }

    /// reg_<k> 模式公共体：不计时预提取 + System.load 后，timed 依次
    /// Class.forName 前 k 个转换类，打印逐类行与 `SEG reg_<k> <累计纳秒>`。
    private static void runRegisterScaled(String kSpec, String resourcePath,
                                          String[] classes) throws Exception {
        extractReplica(resourcePath, ".dll");                         // 不计时
        System.load(lastExtract.getAbsolutePath());                   // 不计时
        int k = Integer.parseInt(kSpec);
        long acc = 0;
        for (int i = 0; i < k && i < classes.length; i++) {
            long t0 = System.nanoTime();
            Class.forName(classes[i]);
            long t1 = System.nanoTime();
            acc += t1 - t0;
            System.out.println("SEG reg_one " + classes[i] + " " + (t1 - t0));
        }
        System.out.println("SEG reg_" + k + " " + acc);
    }

    private static File lastExtract;

    /// #1183 改造后 NativeLoader 的临时文件命名复刻：ThreadLocalRandom 随机名 +
    /// createNewFile 原子独占创建（32 次重试），无 File.createTempFile 的
    /// SecureRandom 冷启动税。
    private static File newTempFileReplica(String tempSuffix) throws Exception {
        String dir = System.getProperty("java.io.tmpdir", ".");
        java.util.concurrent.ThreadLocalRandom rnd =
            java.util.concurrent.ThreadLocalRandom.current();
        for (int i = 0; i < 32; i++) {
            File f = new File(dir, "mj2x-" + Long.toHexString(rnd.nextLong()) + tempSuffix);
            if (f.createNewFile()) return f;
        }
        throw new UnsatisfiedLinkError("mj2x native temp file create failed");
    }

    /// #1183 改造后生成字节码的逐调用复刻：newTempFileReplica + 8192 缓冲读循环
    /// （.enc 时 xorshift32 逐字节解密）+ 写出 + close，不触碰 System.load。
    private static File extractReplicaNew(String resourcePath, String tempSuffix) throws Exception {
        InputStream in = ClassLoader.getSystemResourceAsStream(resourcePath);
        if (in == null) throw new UnsatisfiedLinkError("mj2x native resource not found");
        File temp = newTempFileReplica(tempSuffix);
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
        lastExtract = temp;
        return temp;
    }

    /// NativeLoader 生成字节码的逐调用复刻（相同 API、相同 8192 缓冲、相同
    /// createTempFile("mj2x-", suffix)+deleteOnExit、.enc 时 xorshift32 逐字节
    /// 解密，密钥 0 时固定种子 0x9E3779B9）。探针 jar 走明文形态，解密分支仅
    /// 保形不触发。仅驱动侧测量用，不参与任何产物。
    private static File extractReplica(String resourcePath, String tempSuffix) throws Exception {
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
        lastExtract = temp;
        return temp;
    }
}
