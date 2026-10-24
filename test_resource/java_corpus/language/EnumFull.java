// 语料：枚举全形态——values/valueOf、ordinal/name/compareTo、带字段与构造、
// 静态块、switch（$SwitchMap 合成类）、枚举实现接口。
public class EnumFull {
    enum Color {
        RED, GREEN, BLUE;

        static { System.out.println("Color<clinit>"); }
    }

    enum Planet {
        MERCURY(3.303e+23, 2.4397e6),
        EARTH(5.976e+24, 6.37814e6),
        JUPITER(1.9e+27, 7.1492e7);

        private final double mass;
        private final double radius;

        Planet(double mass, double radius) {
            this.mass = mass;
            this.radius = radius;
        }

        double surfaceGravity() { return 6.67300E-11 * mass / (radius * radius); }
    }

    enum Op implements Runnable {
        ADD {
            @Override public void run() { System.out.println("op:add " + (1 + 2)); }
            @Override int apply(int a, int b) { return a + b; }
        },
        MUL {
            @Override public void run() { System.out.println("op:mul " + (2 * 3)); }
            @Override int apply(int a, int b) { return a * b; }
        };
        abstract int apply(int a, int b);
    }

    public static void main(String[] args) {
        // values/valueOf/ordinal/name
        for (Color c : Color.values()) System.out.println(c.ordinal() + ":" + c.name());
        System.out.println("valueOf " + Color.valueOf("BLUE") + " " + (Color.valueOf("RED") == Color.RED));
        System.out.println("cmp " + Color.RED.compareTo(Color.BLUE) + " " + Color.GREEN.equals(Color.GREEN));

        // 字段与方法
        for (Planet p : Planet.values())
            System.out.println(p + " g=" + String.format(java.util.Locale.ROOT, "%.4f", p.surfaceGravity()));

        // switch on enum（$SwitchMap 合成静态数组）
        for (Color c : Color.values()) {
            String hex;
            switch (c) {
                case RED: hex = "f00"; break;
                case GREEN: hex = "0f0"; break;
                case BLUE: hex = "00f"; break;
                default: hex = "???"; break;
            }
            System.out.println("sw " + c + "#" + hex);
        }

        // 常量体 + 接口实现
        for (Op op : Op.values()) {
            op.run();
            System.out.println("apply " + op.name() + "=" + op.apply(6, 7));
        }
        Runnable r = Op.ADD;
        r.run();

        // 边界：valueOf 不存在 → 异常
        try {
            Color.valueOf("PINK");
            System.out.println("no-exc");
        } catch (IllegalArgumentException e) {
            System.out.println("valueOf-exc=caught");
        }
    }
}
