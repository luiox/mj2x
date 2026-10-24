// 典型程型：哈希与位操作——fnv1a、murmur 风格混合、位段打包、rotl、CRC 风格表
// 驱动循环。密集位运算面。
public class HashAndCrypto {
    static long fnv1a(byte[] data) {
        long hash = 0xcbf29ce484222325L;
        for (byte b : data) {
            hash ^= (b & 0xFF);
            hash *= 0x100000001b3L;
        }
        return hash;
    }

    static int mix32(int k) {
        k ^= k >>> 16;
        k *= 0x85ebca6b;
        k ^= k >>> 13;
        k *= 0xc2b2ae35;
        k ^= k >>> 16;
        return k;
    }

    static int rotl(int v, int n) { return (v << n) | (v >>> (32 - n)); }

    static int[] crcTable() {
        int[] table = new int[256];
        for (int i = 0; i < 256; i++) {
            int c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
            table[i] = c;
        }
        return table;
    }

    static int crc32(byte[] data, int[] table) {
        int crc = 0xFFFFFFFF;
        for (byte b : data) crc = table[(crc ^ (b & 0xFF)) & 0xFF] ^ (crc >>> 8);
        return crc ^ 0xFFFFFFFF;
    }

    // 位段打包/解包（flags 编解码典型形态）
    static long pack(int a, int b, boolean flag, long id) {
        return ((long) a & 0xFF)
               | (((long) b & 0xFF) << 8)
               | ((flag ? 1L : 0L) << 16)
               | ((id & 0xFFFFFFFFFFFFL) << 17);
    }

    public static void main(String[] args) {
        String[] samples = {"", "a", "hello world", "mj2x corpus", "中文测试"};
        for (String s : samples) {
            byte[] bytes = s.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            System.out.println(s + " len=" + bytes.length
                               + " fnv=" + Long.toHexString(fnv1a(bytes))
                               + " crc=" + Integer.toHexString(crc32(bytes, crcTable())));
        }
        for (int k : new int[] {0, 1, -1, Integer.MIN_VALUE, 0xDEADBEEF, 123456789}) {
            System.out.println("mix " + k + " -> " + Integer.toHexString(mix32(k)));
        }
        for (int n = 1; n <= 31; n += 6) {
            System.out.println("rotl " + n + "=" + Integer.toHexString(rotl(0x12345678, n)));
        }
        long packed = pack(200, 55, true, 0x123456789ABCL);
        System.out.println("packed " + Long.toHexString(packed));
        System.out.println("unpack " + (packed & 0xFF) + " " + ((packed >>> 8) & 0xFF) + " "
                           + ((packed >>> 16) & 1) + " " + Long.toHexString((packed >>> 17) & 0xFFFFFFFFFFFFL));
        long x = 0;
        for (int i = 0; i < 64; i++) x ^= 1L << i;
        System.out.println("xor-all=" + x);
        for (int i = 0; i < 8; i++) System.out.print(Long.bitCount(i * 0x9E3779B97F4A7C15L) + " ");
        System.out.println();
    }
}
