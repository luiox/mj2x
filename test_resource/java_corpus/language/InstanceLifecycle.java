// 语料：实例生命周期与初始化顺序——static/instance 初始化块顺序、跨类引用初始化、
// 构造重载链、<clinit> 异常（ExceptionInInitializerError）、null 引用全操作路径。
public class InstanceLifecycle {
    static class Order {
        static int step = 0;
        int slot;
        static final int CONST = init("static-final", 1);
        static int second = init("static", 2);
        int first = init("instance", 3);

        Order() { init("ctor", 4); }
        Order(int v) { slot = v; init("ctor-int", 5); }

        static int init(String what, int v) {
            System.out.println("init#" + (++step) + "=" + what);
            return v;
        }
    }

    static class Lazy {
        static int value = 0;
        static {
            System.out.println("Lazy<clinit> runs");
            value = 42;
        }
        static int get() { return value; }
    }

    static class Broken {
        static int ok = 1;
        static {
            if (true) throw new RuntimeException("clinit-boom");
        }
    }

    static class CtorChain {
        int a, b;
        CtorChain() {
            this(1);
            System.out.println("chain-default");
        }
        CtorChain(int a) {
            this(a, a * 2);
            System.out.println("chain-one a=" + a);
        }
        CtorChain(int a, int b) {
            this.a = a;
            this.b = b;
            System.out.println("chain-two " + a + "," + b);
        }
    }

    public static void main(String[] args) {
        System.out.println("-- new Order --");
        Order o1 = new Order();
        System.out.println("-- new Order(9) --");
        Order o2 = new Order(9);
        System.out.println("slots " + o1.slot + " " + o2.slot);

        // 惰性 <clinit>（首次主动使用才初始化）
        System.out.println("before-lazy");
        System.out.println("lazy=" + Lazy.get());

        // <clinit> 异常 → ExceptionInInitializerError（后续使用 → NoClassDefFoundError）
        try {
            Broken.ok = 2;
            System.out.println("no-broken");
        } catch (ExceptionInInitializerError e) {
            System.out.println("clinit-error=" + e.getCause().getMessage());
        }
        try {
            int x = Broken.ok;
            System.out.println("no-ncdfe " + x);
        } catch (NoClassDefFoundError e) {
            System.out.println("ncdfe=caught");
        }

        System.out.println("-- ctor chain --");
        CtorChain chain = new CtorChain();
        System.out.println("chain " + chain.a + "," + chain.b);

        // null 引用：instanceof 恒 false、传参、比较
        InstanceLifecycle nil = null;
        System.out.println("nil " + (nil instanceof InstanceLifecycle) + " " + (nil == null));
        System.out.println("nilarg " + (nil == self(nil)));
        Object upcast = nil;
        System.out.println("upcast " + (upcast == null));
    }

    static InstanceLifecycle self(InstanceLifecycle x) { return x; }
}
