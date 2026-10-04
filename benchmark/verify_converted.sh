#!/bin/bash
# 假阴性防护（#887 优化一）：断言 RegC 产物 jar 确系转换后产物。
# 背景：v69 fixture 曾出现「CLI 构建失败 → 回退未转换产物 → 探针全过」的
# 假阴性通过。此后任何 fixture/benchmark 跑分前必须先过本断言：
#   1. jar 内嵌 DLL（META-INF/mj2x/native/*.dll）存在且 > 16KB；
#   2. 入口类存根含 __regc_ 注册标记（native 化重命名）；
#   3. DLL 内含 RegC 运行时特征串。
# 用法: verify_converted.sh <jar> <EntryClassBinaryName>（依赖 unzip；JAR_TOOL/JAVAP 可 env 覆盖）
set -u
[ -n "${1:-}" ] && [ -n "${2:-}" ] || { echo "usage: verify_converted.sh <jar> <EntryClass>"; exit 2; }
jar="$1"; cls="$2"
JAR_TOOL="${JAR_TOOL:-C:/Program Files/Microsoft/jdk-25.0.3.9-hotspot/bin/jar.exe}"
JAVAP="${JAVAP:-C:/Program Files/Microsoft/jdk-25.0.3.9-hotspot/bin/javap.exe}"

sz=$(unzip -l "$jar" 'META-INF/mj2x/native/*.dll' 2>/dev/null | awk '/\.dll/{s+=$1} END{print s+0}')
if [ "$sz" -lt 16384 ]; then echo "FAIL: embedded DLL missing/too small ($sz bytes): $jar"; exit 1; fi

if ! "$JAVAP" -p -c -cp "$jar" "$cls" 2>/dev/null | grep -q "__regc_"; then
  echo "FAIL: stub class $cls has no __regc_ markers (NOT converted): $jar"; exit 1
fi

unzip -p "$jar" 'META-INF/mj2x/native/*.dll' > /tmp/vc_probe.dll
if ! grep -a -q "registerNativesForClass" /tmp/vc_probe.dll; then echo "FAIL: DLL lacks RegC runtime marker"; exit 1; fi
rm -f /tmp/vc_probe.dll
echo "OK: $jar is converted (dll=$sz bytes, entry=$cls)"
