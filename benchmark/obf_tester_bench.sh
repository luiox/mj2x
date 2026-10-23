#!/usr/bin/env bash
# jvm-obf-tester 对照基准：原始 jar / os30 jar / mj2x 产物 三方跑同一批 probe。
#
# 为什么多次取中位数：单次 JVM 运行包含 JIT 预热，首跑数字噪声极大（实测
# 同一 jar 连续两次 Total time 可差 2 倍以上）。故每个 jar 跑 N 次，按 probe
# 取中位数。
#
# 用法：
#   obf_tester_bench.sh [重复次数] [额外 jar ...]
# 例：
#   ./benchmark/obf_tester_bench.sh 7 target/mj2x_regc.jar target/mj2x_llvm.jar

set -uo pipefail

RUNS="${1:-7}"
shift || true

TESTER_DIR="${JVM_OBF_TESTER_DIR:-D:/WorkSpace/jvm-obf-tester/target}"
JAVA_BIN="${JAVA_BIN:-java}"

declare -a JARS=(
  "${TESTER_DIR}/jvm-obf-tester.jar"
  "${TESTER_DIR}/jvm-obf-tester_os30.jar"
)
JARS+=("$@")

for jar in "${JARS[@]}"; do
  [ -f "${jar}" ] || { echo "SKIP (missing): ${jar}" >&2; continue; }
done

# 每项 probe 的耗时抓成 "name<TAB>ms"，供 awk 汇总。
probe_times() {
  grep -E '^\| .*\| (Passed|Failed) \|' | \
    sed -E 's/^\|[[:space:]]*([^|]*[^|[:space:]])[[:space:]]*\|.*\|[[:space:]]*([0-9.]+)ms.*/\1\t\2/'
}

echo "runs per jar: ${RUNS}"
echo

for jar in "${JARS[@]}"; do
  [ -f "${jar}" ] || continue
  echo "### $(basename "${jar}")"
  tmp=$(mktemp)
  for ((i = 1; i <= RUNS; i++)); do
    "${JAVA_BIN}" -jar "${jar}" 2>/dev/null | probe_times >> "${tmp}"
  done
  awk -F'\t' '
    { key=$1; v=$2+0; n[key]++; sum[key]+=v; if (arr[key]=="") arr[key]=v; else arr[key]=arr[key] " " v }
    END {
      for (k in n) {
        split(arr[k], a, " ");
        # 插入排序取中位数
        for (i=2; i<=n[k]; i++) { x=a[i]; j=i-1; while (j>=1 && a[j]+0 > x+0) { a[j+1]=a[j]; j-- } a[j+1]=x }
        med = (n[k]%2==1) ? a[(n[k]+1)/2] : (a[n[k]/2]+a[n[k]/2+1])/2;
        printf "  %-30s median %10.3fms  mean %10.3fms  n=%d\n", k, med, sum[k]/n[k], n[k];
        total += med;
      }
      printf "  %-30s %10s  %10.3fms\n", "TOTAL(median sum)", "", total;
    }' "${tmp}"
  rm -f "${tmp}"
  echo
done
