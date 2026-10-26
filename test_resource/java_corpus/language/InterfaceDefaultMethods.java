// 语料：接口默认方法深面——受保护主类自身实现带 default 的接口（接口字节码
// 里的 default 方法回调受保护 native 实现的重入面）、菱形双接口同签名 default
// （必须覆写 + X.super 委派）、继承不覆写（主类分派到接口 default 实现）、
// 私有/私有静态接口方法（JDK9+）、默认方法链式互调。
// 注：接口须为同文件顶层包私有类型（Java 不允许类 implements 自身的嵌套接口）。
interface Greeter {
    String name();

    // default 回调抽象方法：name() 的实现在受保护主类 → 字节码→native 重入
    default String greet() { return "hello " + name() + "!"; }

    default String greetLoud() {
        return greet().toUpperCase(java.util.Locale.ROOT);
    }

    private String secret() { return "g-secret"; }

    default String reveal() { return secret() + "/p"; }

    static String kind() { return "greeter"; }
}

interface Formatter {
    String greet();

    default String bracket(String s) { return "[" + s + "]"; }

    private static String pad() { return "pad"; }

    default String padded() { return pad() + "{" + greet() + "}"; }
}

// 再抽象：子接口把 default 变回抽象 → 实现类必须自己给
interface Strict extends Greeter {
    @Override String greetLoud();
}

public class InterfaceDefaultMethods implements Greeter, Formatter {
    @Override public String name() { return "main"; }

    // 菱形：两接口同签名 default，必须覆写；用 Greeter.super 委派
    @Override public String greet() {
        return Formatter.class.getSimpleName() + ":" + Greeter.super.greet();
    }

    static class StrictImpl implements Strict {
        @Override public String name() { return "strict"; }
        @Override public String greet() { return "strict-greet"; }
        @Override public String greetLoud() { return "STRICT"; }
    }

    public static void main(String[] args) {
        InterfaceDefaultMethods m = new InterfaceDefaultMethods();

        // 经主类静态类型：覆写后的 greet
        System.out.println("own " + m.greet());

        // 经主类静态类型调未覆写的 default（分派到接口 default，default 再回调
        // 受保护 native 的 name()）
        System.out.println("loud " + m.greetLoud());

        // 私有接口方法（经 default 间接）+ 静态接口方法
        System.out.println("reveal " + m.reveal() + " " + Greeter.kind());

        // Formatter 侧：default 调私有静态 + 回调 greet()
        System.out.println("pad " + m.padded() + " " + m.bracket("b"));

        // 接口引用分派
        Greeter g = m;
        System.out.println("iface-ref " + g.greet() + " " + g.greetLoud());

        // 再抽象形态
        StrictImpl s = new StrictImpl();
        System.out.println("strict " + s.greet() + " " + s.greetLoud() + " " + s.reveal());
    }
}
