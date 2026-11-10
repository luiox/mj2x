/// @file vmx64_postlink.hpp
/// @brief vmx64 站点表 post-link 纯 XOR 加密器（链接产物字节级改写）。
///
/// @details
/// 契约见 vmx64_asm_emit.hpp「post-link 纯 XOR 加密器」节。三步原地改写：
/// 表项纯异或（keystream 单源 vmx64Ks64，密钥取表尾 k1 qword）、表区+base
/// qword 的 DIR64 重定位翻 ABSOLUTE、magic/计数/k1 定位痕迹清零。确定性：
/// 同产物 → 逐字节相同。fail-closed：定位符缺失（含重复加密）、计数越界、
/// k1 为 0、期望重定位项缺失、非 AMD64 PE——任一不满足即整体拒绝，不写
/// 半加密产物。
///
/// 消费方：CLI --compile 在 buildXmakeProject 产出 DLL 之后、嵌入/交付之前
/// （单消费点）。--generate-xmake 工作流不经此处——未加密产物本身可运行
/// （base qword 活重定位 → r12 = 0），加密是纯可选加固。

#pragma once

#include <string>

#include "libca/core/result.hpp"

namespace mj2x {

/// @brief 就地加密 @p dllPath 指向的 PE（AMD64 DLL/EXE）中的全部 vmx64
///        站点表。
///
/// @details
/// 全量读入 → 校验 + 改写（内存中完成，全部校验通过才落盘）→ 回写同一
/// 路径。未发现任何 vmx64 表（magic 扫描零命中）视为错误而非成功——消费
/// 方（CLI）只在 vmx64 产物上调用，零命中即产物形态破坏。
ca::core::Result<void, std::string> vmx64EncryptSiteTable(const std::string& dllPath);

}   // namespace mj2x
