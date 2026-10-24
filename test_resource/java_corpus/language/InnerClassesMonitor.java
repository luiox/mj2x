// 语料：内部类与监视器——匿名类实现接口、实例内部类访问外围字段（this$0 合成
// 访问器）、静态嵌套类、局部类捕获；synchronized 方法/块/静态 + 异常展开时的
// monitor exit 路径。
public class InnerClassesMonitor {
    interface Op { int apply(int x); }

    private int outerField = 7;
    private static int staticOuter = 3;

    static class Nested {
        int twice(int x) { return x * 2 + staticOuter; }
    }

    class Inner {
        int sum(int x) { return x + outerField + staticOuter; }
        int deep() { return new Inner2().v; }
        class Inner2 { int v = outerField * 10; }
    }

    static Op makeAdder(final int delta) {
        class LocalAdder implements Op {
            @Override public int apply(int x) { return x + delta + staticOuter; }
        }
        return new LocalAdder();
    }

    int viaAnonymous(final int seed) {
        Op op = new Op() {
            @Override public int apply(int x) { return x * seed + outerField; }
            @Override public String toString() { return "anon"; }
        };
        return op.apply(seed);
    }

    static synchronized int staticSync(int x) {
        InnerClassesMonitor.staticOuter += x;
        return staticOuter;
    }

    synchronized int instanceSync(int x) {
        outerField += x;
        return outerField;
    }

    int blockSync(int x) {
        Object lock = new Object();
        synchronized (lock) {
            outerField += x;
        }
        synchronized (InnerClassesMonitor.class) {
            staticOuter += x;
            return outerField + staticOuter;
        }
    }

    String syncWithException() {
        try {
            synchronized (this) {
                outerField = 99;
                throw new IllegalStateException("in-lock");
            }
        } catch (IllegalStateException e) {
            // 异常展开时 monitor exit 必须执行；字段保持已写值
            return "unwind " + outerField;
        }
    }

    public static void main(String[] args) {
        InnerClassesMonitor self = new InnerClassesMonitor();

        Nested nested = new Nested();
        System.out.println("nested " + nested.twice(5));

        Inner inner = self.new Inner();
        Inner.Inner2 inner2 = inner.new Inner2();
        System.out.println("inner " + inner.sum(1) + " " + inner.deep() + " " + inner2.v);

        Op local = makeAdder(10);
        System.out.println("local-class " + local.apply(5));

        System.out.println("anon " + self.viaAnonymous(3));

        // synchronized 全形态（单线程确定性）
        System.out.println("static-sync " + staticSync(2) + " " + staticSync(3));
        System.out.println("instance-sync " + self.instanceSync(5));
        System.out.println("block-sync " + self.blockSync(1));
        System.out.println("unwind " + self.syncWithException());

        // monitor + 循环（检查点 × 回边）
        int guarded = 0;
        for (int i = 0; i < 5; i++) {
            synchronized (self) {
                guarded += i;
                if (i == 3) { guarded += 100; }
            }
        }
        System.out.println("loop-sync " + guarded);

        // JDK 侧的 monitor（内部类 + 集合）
        java.util.List<Integer> list = java.util.Collections.synchronizedList(new java.util.ArrayList<>());
        for (int i = 0; i < 4; i++) list.add(i * i);
        System.out.println("sync-list " + list);
    }
}
