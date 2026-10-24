// 典型程型：词频统计——字符串处理 + TreeMap 排序聚合 + 格式化输出。
public class WordCount {
    public static void main(String[] args) {
        String text = "the quick brown fox jumps over the lazy dog "
                      + "the dog barks and the fox runs the end";
        java.util.Map<String, Integer> counts = new java.util.TreeMap<>();
        for (String raw : text.split(" ")) {
            String w = raw.trim().toLowerCase();
            if (w.isEmpty()) continue;
            Integer seen = counts.get(w);
            counts.put(w, seen == null ? 1 : seen + 1);
        }
        // 按次数降序、同次数按字母序输出
        java.util.List<java.util.Map.Entry<String, Integer>> rows =
            new java.util.ArrayList<>(counts.entrySet());
        rows.sort((x, y) -> {
            int byCount = Integer.compare(y.getValue(), x.getValue());
            return byCount != 0 ? byCount : x.getKey().compareTo(y.getKey());
        });
        for (java.util.Map.Entry<String, Integer> row : rows) {
            System.out.println(row.getKey() + "=" + row.getValue());
        }
        int total = 0;
        for (int v : counts.values()) total += v;
        System.out.println("words=" + total + " distinct=" + counts.size());
    }
}
