package pack;

import pack.bench.ArrayKernel;
import pack.bench.CalcKernel;
import pack.bench.StringKernel;

public final class Main {
    private Main() {}

    public static void main(String[] args) {
        long start = System.nanoTime();
        int checksum = 0;
        for (int i = 0; i < 20000; i++) {
            checksum ^= CalcKernel.mix(i, i * 3 + 7);
            checksum += StringKernel.mix("mj2x", i);
            checksum ^= ArrayKernel.multi(i & 15, (i >>> 4) & 7);
        }
        long elapsedMs = (System.nanoTime() - start) / 1000000L;
        if (elapsedMs < 1L) {
            elapsedMs = 1L;
        }
        System.out.print("Checksum: ");
        System.out.println(checksum);
        System.out.print("Calc: ");
        System.out.print(elapsedMs);
        System.out.println("ms");
    }
}
