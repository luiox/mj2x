// 典型程型：排序与查找——手写快排/归并/插入排序 + 二分查找 + 边界（空/单元素/
// 全相同/已排序/逆序）。密集分支 + 递归 + 数组交换。
public class SortAndSearch {
    static void quickSort(int[] a, int lo, int hi) {
        if (lo >= hi) return;
        int p = a[(lo + hi) >>> 1];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) {
                int t = a[i]; a[i] = a[j]; a[j] = t;
                i++; j--;
            }
        }
        quickSort(a, lo, j);
        quickSort(a, i, hi);
    }

    static int[] mergeSort(int[] a, int lo, int hi, int[] tmp) {
        if (hi - lo <= 1) return a;
        int mid = (lo + hi) >>> 1;
        mergeSort(a, lo, mid, tmp);
        mergeSort(a, mid, hi, tmp);
        int i = lo, j = mid, k = lo;
        while (i < mid && j < hi) tmp[k++] = a[i] <= a[j] ? a[i++] : a[j++];
        while (i < mid) tmp[k++] = a[i++];
        while (j < hi) tmp[k++] = a[j++];
        System.arraycopy(tmp, lo, a, lo, hi - lo);
        return a;
    }

    static void insertionSort(int[] a) {
        for (int i = 1; i < a.length; i++) {
            int v = a[i], j = i - 1;
            while (j >= 0 && a[j] > v) {
                a[j + 1] = a[j];
                j--;
            }
            a[j + 1] = v;
        }
    }

    static int binarySearch(int[] a, int key) {
        int lo = 0, hi = a.length - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >>> 1;
            if (a[mid] == key) return mid;
            if (a[mid] < key) lo = mid + 1;
            else hi = mid - 1;
        }
        return -(lo + 1);   // 插入点编码（对齐 JDK 语义）
    }

    static String render(int[] a) {
        StringBuilder sb = new StringBuilder();
        for (int v : a) sb.append(v).append(' ');
        return sb.toString();
    }

    public static void main(String[] args) {
        int[][] sets = {
            {5, 3, 8, 1, 9, 2, 7},
            {1},
            {},
            {4, 4, 4, 4},
            {1, 2, 3, 4, 5},
            {9, 8, 7, 6, 5},
            {2, -1, 0, -7, 3, 3, -7},
        };
        for (int[] src : sets) {
            int[] a = src.clone(), b = src.clone(), c = src.clone();
            quickSort(a, 0, a.length - 1);
            mergeSort(b, 0, b.length, new int[b.length]);
            insertionSort(c);
            boolean agree = java.util.Arrays.equals(a, b) && java.util.Arrays.equals(a, c);
            System.out.println("sorted " + render(a) + " agree=" + agree);
        }

        int[] sorted = {1, 3, 5, 7, 9, 11};
        for (int key : new int[] {1, 7, 11, 0, 6, 12, 5}) {
            System.out.println("bs " + key + " -> " + binarySearch(sorted, key));
        }
    }
}
