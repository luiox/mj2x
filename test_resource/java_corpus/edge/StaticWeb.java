// 语料：静态初始化网络——嵌套类的 <clinit> 相互引用（跨类初始化顺序）、
// 循环引用的类、首次触达时序、枚举与常量混布。
public class StaticWeb {
    static class Hub {
        static final String NAME = "hub";
        static int visits = 0;
        // 首次触达 Hub 时会连带触发 Spoke（读其静态字段）
        static int linked = Spoke.WIDTH * 2;
    }

    static class Spoke {
        // 反向引用 Hub.NAME（Hub 的 <clinit> 若未完成，读常量不触发循环初始化
        // ——编译期常量直接内联，是循环引用的安全形态）
        static final String LABEL = "spoke-of-" + Hub.NAME;
        static final int WIDTH = 4;
        static int count = Hub.visits;
    }

    static class Counter {
        static int value;
        static {
            value = 10;
            System.out.println("Counter<clinit> -> " + value);
        }
        static int bump() {
            value += 5;
            return value;
        }
    }

    enum Axis {
        X(Hub.NAME.length()),
        Y(Spoke.WIDTH * 10);

        final int scale;
        Axis(int scale) { this.scale = scale; }
        int scaled(int v) { return v * scale; }
    }

    public static void main(String[] args) {
        // 触发顺序：Hub<clinit> 内读 Spoke.WIDTH → Spoke<clinit> 先完成
        System.out.println("hub.linked=" + Hub.linked);
        System.out.println("spoke.label=" + Spoke.LABEL);

        Hub.visits = 5;
        System.out.println("spoke.count=" + Spoke.count);   // count 在 Spoke<clinit> 时快照

        System.out.println("counter " + Counter.bump() + " " + Counter.bump());

        for (Axis axis : Axis.values()) {
            System.out.println(axis + " scale=" + axis.scale + " v=" + axis.scaled(3));
        }

        // 接口常量（隐式 static final，无 <clinit> 触发）
        System.out.println("consts " + Consts.A + " " + Consts.B + " " + (Consts.A + Consts.B));
    }

    interface Consts {
        int A = 11;
        int B = 22;
        int C = A * B;   // 常量表达式仍内联
    }
}
