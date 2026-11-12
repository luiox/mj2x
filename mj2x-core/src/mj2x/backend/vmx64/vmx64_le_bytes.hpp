#pragma once

// vmx64_le_bytes.hpp
//
// vmx64 域内小端序随机访问读/写原语（std::vector<uint8_t> 镜像上的 offset
// 寻址）。历史上三份逐字拷贝：vmx64_postlink.cpp 匿名 namespace（生产侧）、
// vmx64_postlink_test.cpp 与 vmx64_stream_interp_test.cpp（测试侧），收编
// 为单源。
//
// 越界安全由调用方先行保证（post-link 加密器的 fail-closed 面在读改写前
// 逐层验过 PE/COFF 布局；流级解释器的镜像边界自带布局不变量），本头与
// 既有实现一致不做边界检查。
//
// 为什么不用 libca 的 Bytes/BytesMut：那是游标式（读前进 pos_、Result 错
// 误模型），站点表改写需要随机访问回补（先全表扫描，再回补 .reloc 与
// 痕迹清零），游标模型不适配。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mj2x {

/// @brief 读小端 u16（off 相对镜像起点；调用方保证 off+1 不越界）。
inline uint16_t leRead16(const std::vector<uint8_t>& img, std::size_t off)
{
    return static_cast<uint16_t>(img[off] | (img[off + 1] << 8));
}

/// @brief 读小端 u32。
inline uint32_t leRead32(const std::vector<uint8_t>& img, std::size_t off)
{
    return static_cast<uint32_t>(leRead16(img, off)) |
           (static_cast<uint32_t>(leRead16(img, off + 2)) << 16);
}

/// @brief 读小端 u64。
inline uint64_t leRead64(const std::vector<uint8_t>& img, std::size_t off)
{
    return static_cast<uint64_t>(leRead32(img, off)) |
           (static_cast<uint64_t>(leRead32(img, off + 4)) << 32);
}

/// @brief 写小端 u16（调用方保证 off+1 不越界）。
inline void leWrite16(std::vector<uint8_t>& img, std::size_t off, uint16_t v)
{
    img[off]     = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

/// @brief 写小端 u32。
inline void leWrite32(std::vector<uint8_t>& img, std::size_t off, uint32_t v)
{
    leWrite16(img, off, static_cast<uint16_t>(v & 0xFFFF));
    leWrite16(img, off + 2, static_cast<uint16_t>((v >> 16) & 0xFFFF));
}

/// @brief 写小端 u64。
inline void leWrite64(std::vector<uint8_t>& img, std::size_t off, uint64_t v)
{
    leWrite32(img, off, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    leWrite32(img, off + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

}   // namespace mj2x
