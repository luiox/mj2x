// 语料：lambda 与方法引用——非捕获（单例）、捕获局部/字段/循环变量、this::、
// 静态方法引用、构造器引用、组合器、比较器排序（invokedynamic 派发全路径）。
public class LambdasCapture {
    interface IntOp { int apply(int a, int b); }
    interface ObjSource<T> { T get(); }

    int base = 10;
    static int counter = 0;

    int addBase(int x) { return x + base; }
    int addBase2(int a, int b) { return a - b + base; }
    static int bump(int x) { counter += x; return counter; }

    public static void main(String[] args) {
        // 非捕获 lambda（javac 生成单例）
        IntOp add = (a, b) -> a + b;
        IntOp sub = (a, b) -> a - b;
        IntOp zero = (a, b) -> 0;
        System.out.println("plain " + add.apply(3, 4) + " " + sub.apply(10, 4) + " "
                           + zero.apply(9, 9));

        // 捕获 effectively final 局部变量
        final int k = 100;
        IntOp addK = (a, b) -> a + b + k;
        System.out.println("capture " + addK.apply(1, 2));

        // 捕获 this 字段
        LambdasCapture self = new LambdasCapture();
        IntOp addBase = self::addBase2;                 // this 方法引用
        IntOp lambdaBase = (a, b) -> self.addBase(a - b);
        System.out.println("this-ref " + addBase.apply(5, 0) + " " + lambdaBase.apply(7, 2));

        // 静态方法引用 + 有状态计数
        java.util.function.IntUnaryOperator bump = LambdasCapture::bump;
        System.out.println("static-ref " + bump.applyAsInt(3) + " " + bump.applyAsInt(4));

        // 构造器引用
        ObjSource<StringBuilder> sbNew = StringBuilder::new;
        System.out.println("ctor-ref " + sbNew.get().append("ok").toString());

        // 循环内捕获（每次迭代独立实例）
        java.util.List<java.util.function.IntSupplier> makers = new java.util.ArrayList<>();
        for (int i = 0; i < 4; i++) {
            final int v = i * i;
            makers.add(() -> v);
        }
        StringBuilder got = new StringBuilder();
        for (java.util.function.IntSupplier sup : makers) got.append(sup.getAsInt()).append(",");
        System.out.println("loop-capture " + got);

        // 比较器 lambda 排序（JDK 交互 + 比较分支）
        String[] words = {"pear", "Apple", "fig", "banana", "date"};
        java.util.Arrays.sort(words, (a, b) -> a.length() - b.length());
        System.out.println("by-len " + String.join(",", words));
        java.util.Arrays.sort(words, String.CASE_INSENSITIVE_ORDER);
        System.out.println("ci " + String.join(",", words));
        java.util.Arrays.sort(words);
        System.out.println("natural " + String.join(",", words));

        // 组合器（JDK 内部 lambda 组合）
        java.util.function.Function<Integer, Integer> twice = x -> x * 2;
        java.util.function.Function<Integer, Integer> inc = x -> x + 1;
        System.out.println("compose " + twice.apply(inc.apply(5)) + " " + inc.apply(twice.apply(5)));

        // 求值策略：lambda 体惰性（传参不执行）
        Runnable lazy = () -> System.out.println("should-not-run");
        System.out.println("lazy-created");
        lazy.run();

        // 递归 lambda（经字段自引用）
        java.util.function.IntUnaryOperator[] fibBox = new java.util.function.IntUnaryOperator[1];
        fibBox[0] = n -> n < 2 ? n : fibBox[0].applyAsInt(n - 1) + fibBox[0].applyAsInt(n - 2);
        System.out.println("fib-lambda " + fibBox[0].applyAsInt(12));
    }
}
