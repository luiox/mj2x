// 语料：switch 全形态——tableswitch（稠密）、lookupswitch（稀疏）、string switch
// （hashCode+equals 双段）、fallthrough、switch 与循环/异常组合。
public class SwitchForms {
    static String dense(int x) {
        String r;
        switch (x) {          // 0..5 稠密 → tableswitch
            case 0: r = "zero"; break;
            case 1: r = "one"; break;
            case 2: r = "two"; break;
            case 3: r = "three"; break;
            case 4: r = "four"; break;
            case 5: r = "five"; break;
            default: r = "many"; break;
        }
        return r;
    }

    static String sparse(int x) {
        String r = "?";
        switch (x) {          // 大间隔 → lookupswitch
            case -1000000: r = "neg-mega"; break;
            case -7: r = "neg-seven"; break;
            case 0: r = "zero"; break;
            case 1 << 20: r = "mega"; break;
            case 99999999: r = "pos-mega"; break;
        }
        return r;
    }

    static String fall(int x) {
        StringBuilder sb = new StringBuilder();
        switch (x) {          // 故意 fallthrough
            case 1: sb.append("a");
            case 2: sb.append("b");
            case 3: sb.append("c"); break;
            case 4: sb.append("d");
            default: sb.append("z");
        }
        return sb.toString();
    }

    static String byName(String s) {
        switch (s) {          // javac：hashCode 分段 + equals 消歧
            case "alpha": return "A";
            case "beta": return "B";
            case "Alpha": return "a";
            case "": return "empty";
            case "conflict": return "c";
            default: return "?";
        }
    }

    public static void main(String[] args) {
        for (int i = -1; i <= 7; i++) System.out.println("dense " + i + "=" + dense(i));
        int[] sp = {-1000000, -7, 0, 1 << 20, 99999999, 42};
        for (int x : sp) System.out.println("sparse " + x + "=" + sparse(x));
        for (int i = 0; i <= 5; i++) System.out.println("fall " + i + "=" + fall(i));
        String[] names = {"alpha", "beta", "Alpha", "", "conflict", "zz"};
        for (String n : names) System.out.println("name " + n + "=" + byName(n));

        // switch 在循环里（块参数/回边组合）
        int acc = 0;
        for (int i = 0; i < 10; i++) {
            switch (i % 4) {
                case 0: acc += 1; break;
                case 1: acc += 2; break;
                case 2: acc += 4; continue;
                default: acc -= 1;
            }
            acc *= 2;
        }
        System.out.println("loop-switch=" + acc);

        // switch 内抛异常被外层捕获（dispatcher 与异常表组合）
        try {
            int v = 3;
            switch (v) {
                case 3: throw new IllegalStateException("sw");
                default: System.out.println("no-throw");
            }
        } catch (IllegalStateException e) {
            System.out.println("sw-exc=" + e.getMessage());
        }
    }
}
