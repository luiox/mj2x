// 语料：record 全形态——紧凑构造器校验、显式 accessor 覆写、静态工厂、泛型
// record、record 套 record、record 实现接口、内容 equals/hashCode、null 组件。
// record 本体是嵌套类（不被保护），受保护 main 经 JNI 走 accessor/字段/工厂。
public class RecordsShapes {
    record Point(int x, int y) {
        // 紧凑构造器：组件校验（不合法直接抛）
        Point {
            if (x < 0 || y < 0) throw new IllegalArgumentException("neg");
        }

        // 显式覆写 accessor（带副作用打印）
        @Override public int x() {
            System.out.println("accessor x");
            return x;
        }

        static Point origin() { return new Point(0, 0); }

        double norm() { return Math.sqrt(x * x + y * y); }
    }

    record Pair<A, B>(A a, B b) {}

    record Boxed(String tag, Point p) {}

    interface Named { String name(); }

    record Staff(String name, int id) implements Named {}

    public static void main(String[] args) {
        var p = new Point(3, 4);
        System.out.println(p.x() + "," + p.y() + " n=" + p.norm());

        Point o = Point.origin();
        // record 的 equals/hashCode 是内容实现（生成代码，非 identity）→ 确定性
        System.out.println(o.equals(new Point(0, 0)) + " " + (o.hashCode() == new Point(0, 0).hashCode()));
        System.out.println(p);

        try {
            new Point(-1, 2);
            System.out.println("no-exc");
        } catch (IllegalArgumentException e) {
            System.out.println("compact-exc");
        }

        Pair<String, Integer> pr = new Pair<>("k", 7);
        System.out.println(pr.a() + pr.b());

        Boxed bx = new Boxed("b", p);
        System.out.println(bx.tag() + " " + bx.p().y());

        Named n = new Staff("ann", 1);
        System.out.println(n.name() + " " + ((Staff) n).id());

        // null 组件的内容相等
        System.out.println(new Pair<>(null, null).equals(new Pair<>(null, null)));
    }
}
