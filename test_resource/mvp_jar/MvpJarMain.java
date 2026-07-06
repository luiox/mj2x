package mvpjar;

public class MvpJarMain {
    public static int entry(int value) {
        return MvpJarHelper.twice(value) + 1;
    }
}
