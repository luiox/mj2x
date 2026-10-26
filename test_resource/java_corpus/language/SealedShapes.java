// 语料：sealed 全形态——sealed 接口 + permits、record 实现 sealed（代数数据
// 类型惯用法）、instanceof 模式匹配绑定（含复合条件流作用域）、sealed 抽象类
// 层级递归、受保护代码反射读 isSealed/getPermittedSubclasses。
public class SealedShapes {
    sealed interface Shape permits Circle, Square, Rect {}
    record Circle(double r) implements Shape {}
    record Square(double s) implements Shape {}
    record Rect(double w, double h) implements Shape {}

    static double area(Shape s) {
        if (s instanceof Circle c) return Math.PI * c.r() * c.r();
        if (s instanceof Square q) return q.s() * q.s();
        if (s instanceof Rect t) return t.w() * t.h();
        throw new IllegalArgumentException("exhausted");
    }

    static String describe(Shape s) {
        // 模式变量在 && / || 复合条件下的流作用域（体内不使用可能未绑定的变量）
        if (s instanceof Circle c && c.r() > 1.0) return "big-circle";
        if (s instanceof Square q || s instanceof Rect t) return "angular";
        return "small";
    }

    sealed static abstract class Tree permits Leaf, Node {}
    static final class Leaf extends Tree {
        final int v;
        Leaf(int v) { this.v = v; }
    }
    static final class Node extends Tree {
        final Tree l, r;
        Node(Tree l, Tree r) { this.l = l; this.r = r; }
    }

    static int sum(Tree t) {
        if (t instanceof Leaf lf) return lf.v;
        if (t instanceof Node nd) return sum(nd.l) + sum(nd.r);
        return 0;
    }

    public static void main(String[] args) {
        Shape[] shapes = { new Circle(1), new Circle(2.5), new Square(3), new Rect(2, 5) };
        for (Shape s : shapes) {
            System.out.println(s.getClass().getSimpleName() + " "
                               + String.format(java.util.Locale.ROOT, "%.3f", area(s)) + " " + describe(s));
        }

        Tree t = new Node(new Leaf(1), new Node(new Leaf(2), new Leaf(3)));
        System.out.println("sum " + sum(t));

        // 反射面（确定性）：PermittedSubclasses 属性存在性与形状
        System.out.println("permits " + Shape.class.isSealed() + " "
                           + Shape.class.getPermittedSubclasses().length);
    }
}
