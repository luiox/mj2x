// 性能语料：字段与集合——对象字段读写、HashMap get/put、数组遍历写（JNI 字段
// 访问路径压力）。
public class BenchFields {
    static class Point {
        int x, y;
        long tag;
        Point next;

        Point(int x, int y, long tag) {
            this.x = x;
            this.y = y;
            this.tag = tag;
        }
    }

    static int staticCounter = 0;

    public static void main(String[] args) {
        long checksum = 0;

        // 链表字段遍历（instance 字段 + 引用字段）
        Point head = new Point(0, 0, 0);
        Point tail = head;
        for (int i = 1; i <= 2000; i++) {
            Point p = new Point(i, i * 2, i * 7L);
            tail.next = p;
            tail = p;
        }
        for (Point p = head; p != null; p = p.next) {
            checksum += p.x * 31 + p.y + p.tag;
        }

        // 字段写密集
        for (int i = 0; i < 200_000; i++) {
            head.x = i;
            head.y = head.x + 1;
            staticCounter = staticCounter + 3;
            checksum += head.y & 0xFF;
        }

        // HashMap get/put 密集（JDK 交互 + 装箱）
        java.util.Map<Integer, Integer> map = new java.util.HashMap<>();
        for (int i = 0; i < 20_000; i++) {
            map.put(i & 0xFFF, i);
        }
        for (int i = 0; i < 200_000; i++) {
            Integer got = map.get(i & 0xFFF);
            if (got != null) checksum += got & 0xFF;
        }

        // 数组遍历写
        int[] arr = new int[10_000];
        for (int i = 0; i < arr.length; i++) arr[i] = i * 3;
        for (int i = 0; i < arr.length; i++) checksum += arr[i] & 0x7F;

        System.out.println("checksum=" + checksum);
    }
}
