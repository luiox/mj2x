// 语料：assert 语义——javac 脱糖出 $assertionsDisabled 静态字段 + <clinit> 里
// Class.desiredAssertionStatus() JNI 调用；断言关闭（默认无 -ea）下条件与
// 消息表达式均惰性不执行（副作用计数验证）。受保护 <clinit> 含该脱糖序列。
public class AssertSemantics {
    static int sideEffects = 0;

    static boolean cond() {
        sideEffects++;
        return false;
    }

    static String message() {
        sideEffects++;
        return "expensive-message";
    }

    static int checked(int x) {
        assert x >= 0 : "negative: " + message();
        assert cond() : message();
        return x * 2;
    }

    public static void main(String[] args) {
        System.out.println("desired=" + AssertSemantics.class.desiredAssertionStatus());
        System.out.println("checked " + checked(21));
        System.out.println("side=" + sideEffects);

        try {
            assert false : "never";
            System.out.println("after");
        } catch (AssertionError e) {
            System.out.println("caught-assert");
        }
        System.out.println("side-final=" + sideEffects);
    }
}
