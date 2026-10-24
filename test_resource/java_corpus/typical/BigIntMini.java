// 典型程型：迷你大整数——int 数组为 limbs 的加减乘（schoolbook）、进制打印、
// 与 long 边界值互验。数组 + 进位链（依赖链密集）。
public class BigIntMini {
    static int[] fromLong(long v) {
        boolean neg = v < 0;
        long mag = neg ? -v : v;
        int[] out = new int[4];
        for (int i = 0; i < 4; i++) out[i] = (int) ((mag >>> (i * 16L)) & 0xFFFFL);
        if (neg) negate(out);
        return out;
    }

    static void negate(int[] limbs) {
        for (int i = 0; i < limbs.length; i++) limbs[i] = ~limbs[i];
        addSmall(limbs, 1);
    }

    static void addSmall(int[] limbs, int add) {
        long carry = add;
        for (int i = 0; i < limbs.length && carry != 0; i++) {
            long sum = (limbs[i] & 0xFFFFL) + (carry & 0xFFFFL);
            limbs[i] = (int) (short) sum;
            carry = sum >>> 16;
        }
    }

    static int[] add(int[] a, int[] b) {
        int[] out = new int[Math.max(a.length, b.length) + 1];
        long carry = 0;
        for (int i = 0; i < out.length; i++) {
            long sum = carry;
            if (i < a.length) sum += a[i] & 0xFFFFL;
            if (i < b.length) sum += b[i] & 0xFFFFL;
            out[i] = (int) (short) sum;
            carry = sum >>> 16;
        }
        return out;
    }

    static int[] mul(int[] a, int[] b) {
        int[] out = new int[a.length + b.length + 1];
        for (int i = 0; i < a.length; i++) {
            long ai = a[i] & 0xFFFFL;
            long carry = 0;
            for (int j = 0; j < b.length; j++) {
                long cur = (out[i + j] & 0xFFFFL) + ai * (b[j] & 0xFFFFL) + carry;
                out[i + j] = (int) (short) cur;
                carry = cur >>> 16;
            }
            int k = i + b.length;
            while (carry != 0) {
                long cur = (out[k] & 0xFFFFL) + carry;
                out[k] = (int) (short) cur;
                carry = cur >>> 16;
                k++;
            }
        }
        return out;
    }

    static String render(int[] limbs) {
        StringBuilder sb = new StringBuilder();
        for (int i = limbs.length - 1; i >= 0; i--) sb.append(limbs[i]).append(':');
        return sb.toString();
    }

    public static void main(String[] args) {
        // 用 long 乘法交叉验证大整数乘法
        long[][] pairs = {{123456L, 789L}, {999999L, 999999L}, {1L << 30, 1L << 30}};
        for (long[] p : pairs) {
            int[] prod = mul(fromLong(p[0]), fromLong(p[1]));
            StringBuilder hex = new StringBuilder();
            for (int i = prod.length - 1; i >= 0; i--) {
                String h = Integer.toHexString(prod[i] & 0xFFFF);
                hex.append("0000", 0, 4 - h.length()).append(h);
            }
            System.out.println(p[0] + "*" + p[1] + " = " + Long.toHexString(p[0] * p[1])
                               + " / " + hex);
        }

        int[] acc = fromLong(0);
        for (int i = 1; i <= 20; i++) acc = add(acc, fromLong(1L << (i % 30)));
        System.out.println("acc " + render(acc));
        for (int i = 0; i < 5; i++) acc = mul(acc, fromLong(3));
        System.out.println("mul5 " + render(acc));
        addSmall(acc, 65535);
        System.out.println("addsmall " + render(acc));
        negate(acc);
        System.out.println("neg " + render(acc));
    }
}
