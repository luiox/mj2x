// 语料：switch 表达式全形态——箭头臂、多常量臂、yield 块、throw 臂、嵌套
// switch 表达式、String switch 表达式（hashCode 阶梯）、枚举 switch 表达式
// （$SwitchMap 合成类）、无 default 的穷尽枚举。
public class SwitchExpressions {
    enum Dir { N, E, S, W }

    static int denseScore(int x) {
        return switch (x) {
            case 0 -> 0;
            case 1 -> 10;
            case 2 -> 20;
            case 3, 4 -> 30 + x;
            default -> -1;
        };
    }

    static String sparseWord(int x) {
        return switch (x) {
            case 10 -> "ten";
            case 100 -> "hundred";
            case 1000 -> "thousand";
            default -> "other";
        };
    }

    static String withYield(int x) {
        return switch (x) {
            case 1 -> {
                int v = x * 100;
                yield "v" + v;
            }
            case 2 -> {
                if (x % 2 == 0) yield "even";
                yield "odd";
            }
            default -> {
                yield "d" + x;
            }
        };
    }

    static String stringSwitchExpr(String s) {
        return switch (s) {
            case "apple" -> "A";
            case "banana", "cherry" -> "BC";
            default -> "?";
        };
    }

    static String enumSwitchExpr(Dir d) {
        return switch (d) {
            case N -> "up";
            case E -> "right";
            case S -> "down";
            case W -> "left";
        };
    }

    static int throwArm(int x) {
        return switch (x) {
            case 0 -> 0;
            default -> throw new IllegalArgumentException("bad " + x);
        };
    }

    public static void main(String[] args) {
        for (int i = 0; i <= 5; i++) System.out.println("dense " + i + "=" + denseScore(i));
        System.out.println("sparse " + sparseWord(10) + " " + sparseWord(100) + " " + sparseWord(7));
        System.out.println("yield " + withYield(1) + " " + withYield(2) + " " + withYield(9));
        System.out.println("str " + stringSwitchExpr("apple") + " " + stringSwitchExpr("cherry") + " "
                           + stringSwitchExpr("zz"));
        for (Dir d : Dir.values()) System.out.println("dir " + d + "=" + enumSwitchExpr(d));
        System.out.println("nested " + (switch (2) {
            case 1 -> "one";
            case 2 -> switch (3) {
                case 3 -> "two/three";
                default -> "inner";
            };
            default -> "x";
        }));
        System.out.println("zero " + throwArm(0));
        try {
            throwArm(5);
            System.out.println("no-exc");
        } catch (IllegalArgumentException e) {
            System.out.println("throw-arm caught");
        }
    }
}
