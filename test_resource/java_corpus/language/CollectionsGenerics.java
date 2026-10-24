// 语料：泛型与集合——通配符与边界、泛型方法、迭代器（checkcast 密集）、
// HashMap/TreeMap/LinkedHashMap 语义、Comparator 链、Collections 工具。
public class CollectionsGenerics {
    static <T extends Comparable<T>> T maxOf(Iterable<T> xs) {
        T best = null;
        for (T x : xs) {
            if (best == null || x.compareTo(best) > 0) best = x;
        }
        return best;
    }

    static double sumNumbers(java.util.Collection<? extends Number> xs) {
        double s = 0;
        for (Number n : xs) s += n.doubleValue();
        return s;
    }

    interface Container {
        <E> void put(E e);
        <E> E take(int i);
    }

    static class Bag implements Container {
        private final java.util.List<Object> items = new java.util.ArrayList<>();
        @Override public <E> void put(E e) { items.add(e); }
        @SuppressWarnings("unchecked")
        @Override public <E> E take(int i) { return (E) items.get(i); }
        int size() { return items.size(); }
    }

    public static void main(String[] args) {
        // 泛型方法 + 边界
        java.util.List<String> words = java.util.Arrays.asList("fig", "apple", "pear");
        System.out.println("max " + maxOf(words));
        System.out.println("sumi " + sumNumbers(java.util.Arrays.asList(1, 2, 3)));
        System.out.println("sumd " + sumNumbers(java.util.Arrays.asList(0.5, 1.5)));

        // 泛型接口方法
        Bag bag = new Bag();
        bag.put("one");
        bag.put(2);
        bag.put(3.0);
        System.out.println("bag " + bag.size() + " " + bag.take(0) + " " + bag.take(1) + " "
                           + bag.take(2));

        // HashMap 语义（key 集合顺序不打印——只打 key→value 查找）
        java.util.Map<String, Integer> ages = new java.util.HashMap<>();
        ages.put("a", 1);
        ages.put("b", 2);
        ages.put("a", 3);              // 覆盖
        System.out.println("map " + ages.get("a") + " " + ages.get("b") + " " + ages.get("zz")
                           + " " + ages.getOrDefault("zz", -1) + " size=" + ages.size());
        System.out.println("map-contains " + ages.containsKey("b") + " " + ages.containsValue(9));
        ages.remove("a");
        System.out.println("map-rm " + ages.size() + " " + ages.get("a"));

        // TreeMap（有序遍历，顺序确定可打印）
        java.util.TreeMap<Integer, String> sorted = new java.util.TreeMap<>();
        sorted.put(5, "five");
        sorted.put(1, "one");
        sorted.put(3, "three");
        System.out.println("tree " + sorted + " first=" + sorted.firstKey() + " head=" + sorted.headMap(4));

        // LinkedHashMap 插入序
        java.util.Map<String, Integer> lru = new java.util.LinkedHashMap<>();
        for (String k : new String[] {"x", "y", "x", "z"}) lru.put(k, lru.size());
        System.out.println("lru " + lru);

        // 迭代器（hasNext/next → checkcast）与 for-each
        java.util.List<String> list = new java.util.ArrayList<>();
        java.util.Collections.addAll(list, "p", "q", "r");
        StringBuilder it = new StringBuilder();
        for (java.util.Iterator<String> itor = list.iterator(); itor.hasNext(); ) {
            it.append(itor.next()).append("|");
        }
        System.out.println("iter " + it + " foreach " + list);

        // Comparator 链
        java.util.List<int[]> rows = new java.util.ArrayList<>();
        rows.add(new int[] {2, 9});
        rows.add(new int[] {1, 8});
        rows.add(new int[] {2, 3});
        rows.sort(java.util.Comparator.<int[]>comparingInt(r -> r[0]).thenComparing(r -> r[1]));
        StringBuilder rs = new StringBuilder();
        for (int[] r : rows) rs.append(r[0]).append(":").append(r[1]).append(" ");
        System.out.println("sorted-rows " + rs);

        // Collections 工具
        java.util.List<Integer> nums = new java.util.ArrayList<>(java.util.Arrays.asList(3, 1, 4, 1, 5));
        java.util.Collections.sort(nums);
        System.out.println("sorted " + nums + " max " + java.util.Collections.max(nums)
                           + " freq " + java.util.Collections.frequency(nums, 1));
        java.util.Collections.reverse(nums);
        java.util.Collections.shuffle(nums, new java.util.Random(42));   // 种子固定 → 确定
        System.out.println("shuffled " + nums);
        java.util.Collections.fill(nums, 0);
        System.out.println("filled " + nums);
    }
}
