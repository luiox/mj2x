/// @file vmx64_postlink.hpp
/// @brief vmx64 站点表 post-link 加密器：对链接产出的 DLL/EXE 做字节级
///        原地改写。
///
/// @details
/// 输入是 ml64/link 刚产出的 AMD64 PE（未加密形态）。站点表此时在 .data
/// 里是明文：表项 = handler 首选 VA - ks64(t) 的线性差、每个表项带 DIR64
/// 重定位、表尾 k1 密钥材料与 magic 定位符全部可读。加密三步（完整算法
/// 与数字例子见 doc:vm-site-table-encryption；发射侧契约见
/// vmx64_asm_emit.hpp 的 post-link 站点表加密一节）：
///   1. 表项纯异或：表项落盘值逐项 XOR ks64(t, k1)，k1 取表尾 qword
///      （keystream 单源 vmx64Ks64）；
///   2. 重定位失效：.reloc 里指向表区（n 个表项 + base qword）的 DIR64
///      项 type 翻成 ABSOLUTE，装载器不再往密文加镜像基差；
///   3. 定位痕迹清零：magic/站点数/k1 三个 qword 归零，文件里不再有可
///      定位表头的特征字节，同时构成防重复加密的幂等护栏。
///
/// 确定性：同一产物 → 逐字节相同的加密结果（无随机量）。
/// fail-closed：magic 扫描零命中（含已加密产物）、站点数越界、k1 为 0、
/// 期望的 DIR64 重定位项缺失或类型不符、.reloc 块畸形、非 AMD64 PE，
/// 任一不满足即整体拒绝，内存改写全部作废、不落盘半加密产物。
///
/// 消费方：CLI --compile 在 buildXmakeProject 产出 DLL 之后、嵌入/交付之前
/// 调用（单消费点）。--generate-xmake 工作流不经此处，未加密产物本身可
/// 运行（base qword 的活重定位使运行时 r12 = 0，走明文 dispatch），加密是
/// 纯可选加固。

#pragma once

#include <string>

#include "libca/core/result.hpp"

namespace mj2x {

/// @brief 就地加密 @p dllPath 指向的 PE（AMD64 DLL/EXE）中的全部 vmx64
///        站点表。
///
/// @details
/// 全量读入 → 内存中校验 + 改写 → 校验全过才写临时文件 → rename 原子替换
/// 回原路径；中途任何失败（校验不过、写盘失败、rename 失败）都保持原文件
/// 一字不动。未发现任何 vmx64 表（magic 扫描零命中）视为错误而非成功，
/// 消费方只在 vmx64 产物上调用，零命中即产物已加密或产物不是 vmx64
/// 形态。
ca::core::Result<void, std::string> vmx64EncryptSiteTable(const std::string& dllPath);

}   // namespace mj2x
