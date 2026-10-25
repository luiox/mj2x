// 语料：循环构建查找表 + 循环出口后寄存器下标读取——块参数（边拷贝 dest）
// 跨循环携带与出口参数链的干扰面（regfile 区间计算 J-12 缺陷的原始形态）。
// 表驱动 CRC 与混合位运算，全输出确定。
public class LoopCarriedTable {
    public static void main(String[] args) {
        int[] table = new int[256];
        for (int i = 0; i < 256; i++) {
            int c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
            table[i] = c;
        }
        int c0 = 0xFFFFFFFF;
        int idx = (c0 ^ (0x61 & 0xFF)) & 0xFF;
        int loaded = table[idx];
        System.out.println("idx=" + Integer.toHexString(idx));
        System.out.println("t9e=" + Integer.toHexString(table[0x9E]));
        System.out.println("loaded=" + Integer.toHexString(loaded));
        int shifted = c0 >>> 8;
        System.out.println("xored=" + Integer.toHexString(loaded ^ shifted));
        StringBuilder sb = new StringBuilder();
        for (int probe : new int[] {0, 1, 158, 255}) {
            sb.append(Integer.toHexString(table[probe])).append(',');
        }
        System.out.println("probes=" + sb);
    }
}
