// 语料：初始化顺序全形态——受保护主类自身 <clinit> 的字段/静态块交错顺序、
// 静态块内限定名前向引用（取默认值）、编译期常量折叠 vs 运行期初始化、实例
// 字段与实例初始化块与构造器链（this(...) 委派）顺序、lazy holder 惰性单次、
// 接口常量内联不触发 <clinit> vs 非常量字段触发的 JLS 语义。
public class StaticInitOrder {
    static final int CONST = 40;

    static int a = log("f-a");
    static int b;

    static {
        System.out.println("blk1 a=" + a + " b=" + StaticInitOrder.b);
        b = log("blk1-sets-b");
    }

    static final int C = CONST + 2;
    static int c = CONST * 0 + log("f-c");

    static { System.out.println("blk2 C=" + C + " c=" + c); }

    int i1 = ilog("i1");
    { System.out.println("iblk1 i1=" + i1); }
    int i2 = ilog("i2");

    StaticInitOrder() {
        this(9);
        System.out.println("ctor0");
    }

    StaticInitOrder(int x) { System.out.println("ctor1 x=" + x); }

    static int log(String tag) {
        System.out.println("s:" + tag);
        return 1;
    }

    int ilog(String tag) {
        System.out.println("i:" + tag);
        return 2;
    }

    interface Consts {
        int K = 5;                                 // 编译期常量：读它不触发 <clinit>
        int J = StaticInitOrder.log("Consts-J");   // 非常量：读它触发接口 <clinit>

        static int probe() {
            System.out.println("Consts-probe");
            return 7;
        }
    }

    static class Holder {
        static final int V = StaticInitOrder.log("holder");
    }

    static String useHolder() {
        return "holder=" + Holder.V + "," + Holder.V;
    }

    public static void main(String[] args) {
        System.out.println("main-begin");
        System.out.println("K=" + Consts.K);
        System.out.println("J=" + Consts.J);
        System.out.println("probe=" + Consts.probe());

        StaticInitOrder o0 = new StaticInitOrder();
        StaticInitOrder o1 = new StaticInitOrder(1);
        System.out.println("fields " + o0.i2 + o1.i1);

        System.out.println(useHolder());
        System.out.println("end");
    }
}
