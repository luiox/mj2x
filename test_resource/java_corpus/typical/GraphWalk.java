// 典型程型：图遍历——邻接表 DFS/BFS/拓扑排序/环检测，递归 + 队列/栈 + 状态数组。
public class GraphWalk {
    static String dfs(int[][] adj, int start) {
        boolean[] seen = new boolean[adj.length];
        StringBuilder order = new StringBuilder();
        dfsRec(adj, start, seen, order);
        return order.toString();
    }

    static void dfsRec(int[][] adj, int node, boolean[] seen, StringBuilder order) {
        if (seen[node]) return;
        seen[node] = true;
        order.append(node).append(' ');
        for (int next : adj[node]) dfsRec(adj, next, seen, order);
    }

    static String bfs(int[][] adj, int start) {
        boolean[] seen = new boolean[adj.length];
        int[] queue = new int[adj.length];
        int head = 0, tail = 0;
        queue[tail++] = start;
        seen[start] = true;
        StringBuilder order = new StringBuilder();
        while (head < tail) {
            int node = queue[head++];
            order.append(node).append(' ');
            for (int next : adj[node]) {
                if (!seen[next]) {
                    seen[next] = true;
                    queue[tail++] = next;
                }
            }
        }
        return order.toString();
    }

    static String topo(int[][] adj) {
        int n = adj.length;
        int[] indeg = new int[n];
        for (int[] row : adj) for (int next : row) indeg[next]++;
        int[] queue = new int[n];
        int head = 0, tail = 0;
        for (int i = 0; i < n; i++) if (indeg[i] == 0) queue[tail++] = i;
        StringBuilder order = new StringBuilder();
        int processed = 0;
        while (head < tail) {
            int node = queue[head++];
            order.append(node).append(' ');
            processed++;
            for (int next : adj[node]) if (--indeg[next] == 0) queue[tail++] = next;
        }
        return processed == n ? order.toString() : "CYCLE";
    }

    public static void main(String[] args) {
        int[][] dag = {
            {1, 2},    // 0
            {3},       // 1
            {3, 4},    // 2
            {5},       // 3
            {5},       // 4
            {},        // 5
        };
        System.out.println("dfs " + dfs(dag, 0));
        System.out.println("bfs " + bfs(dag, 0));
        System.out.println("topo " + topo(dag));

        int[][] cyclic = {{1}, {2}, {0, 3}, {}};
        System.out.println("cyclic-topo " + topo(cyclic));
        System.out.println("cyclic-dfs " + dfs(cyclic, 0));

        int[][] forest = {{1}, {}, {3}, {}};
        System.out.println("forest-dfs " + dfs(forest, 0));

        // 加权最短路（Dijkstra 简版，O(V^2)）
        int[][] weights = {
            {-1, 4, 2, -1},
            {-1, -1, 5, 10},
            {-1, -1, -1, 3},
            {-1, -1, -1, -1},
        };
        int n = weights.length;
        int[] dist = new int[n];
        boolean[] done = new boolean[n];
        java.util.Arrays.fill(dist, Integer.MAX_VALUE);
        dist[0] = 0;
        for (int round = 0; round < n; round++) {
            int best = -1;
            for (int i = 0; i < n; i++)
                if (!done[i] && (best == -1 || dist[i] < dist[best])) best = i;
            if (best == -1 || dist[best] == Integer.MAX_VALUE) break;
            done[best] = true;
            for (int j = 0; j < n; j++) {
                int w = weights[best][j];
                if (w >= 0 && dist[best] + w < dist[j]) dist[j] = dist[best] + w;
            }
        }
        System.out.println("dijkstra " + java.util.Arrays.toString(dist));
    }
}
