// 性能语料：字符串密集——StringBuilder 追加/反转、indexOf 扫描、拼接、拆分。
public class BenchString {
    public static void main(String[] args) {
        long checksum = 0;
        for (int round = 0; round < 200; round++) {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 500; i++) {
                sb.append("tok").append(i % 17).append(';');
            }
            String packed = sb.toString();
            int hits = 0, pos = 0;
            while ((pos = packed.indexOf("tok3", pos)) >= 0) {
                hits++;
                pos += 4;
            }
            String reversed = new StringBuilder(packed).reverse().toString();
            checksum += hits * 31 + reversed.length();
            checksum += packed.hashCode() & 0xFFFF;   // hashCode 函数确定（非恒等散列）
            String[] parts = packed.split(";", 100);
            checksum += parts.length;
        }
        System.out.println("checksum=" + checksum);
    }
}
