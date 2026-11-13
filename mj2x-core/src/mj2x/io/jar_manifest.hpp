#pragma once

#include <vector>

#include "libca/core/datatype.hpp"

namespace mj2x {

/// @brief JAR 主清单条目名。JVM 按精确大小写定位 java -jar 的主清单。
inline constexpr const char* kJarManifestEntry = "META-INF/MANIFEST.MF";

/// @brief 主清单 native 访问声明属性（issue #58）。
///
/// JDK 25 起可执行 JAR 主清单声明该属性后，java -jar 启动的产物内
/// System.load 不再产生 restricted method 告警（本机 JDK 25.0.3 实测：
/// 有属性无告警，无属性 4 行告警）。JDK 24 及以下把未知主属性当普通
/// 属性忽略（本机 8u162/17.0.20/21.0.11 实测加载行为不变），故写入面
/// 不按 JDK 版本做条件；是否声明由用户开关 --enable-native-access 决定
/// （默认关 = 不声明），条件开关只加在打包调用侧。
inline constexpr const char* kEnableNativeAccess      = "Enable-Native-Access";
inline constexpr const char* kEnableNativeAccessValue = "ALL-UNNAMED";

/// @brief 产出嵌入 native 库的输出 JAR 的 MANIFEST.MF 字节。
///
/// @details
/// original 为 null 或空字节时输出全新主段（Manifest-Version 加属性行）；
/// 否则把 Enable-Native-Access 合并进原主段（第一段，到第一个空行为止），
/// 插入位置在 Manifest-Version 行之后（无版本行则在主段开头），原字节
/// 除插入行外逐字节保留（既有属性、换行风格、per-entry 段与续行不动）。
/// 主段已声明该属性时原样返回：用户显式写入的取值优先，不覆盖。
/// 属性行用 CRLF 终止（JAR 规范 canonical 形态，本机 JDK 8/17/21/25
/// 实测可解析）。
///
/// @param original 既有 MANIFEST.MF 字节，null 表示输出 JAR 原本无清单。
std::vector<ca::u8> buildNativeAccessManifest(const std::vector<ca::u8>* original);

/// @brief 判断 manifest 字节主段（第一段）是否已声明 Enable-Native-Access。
bool manifestHasNativeAccess(const std::vector<ca::u8>& bytes);

}   // namespace mj2x
