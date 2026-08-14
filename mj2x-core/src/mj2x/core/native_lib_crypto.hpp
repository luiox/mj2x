// native_lib_crypto.hpp - native lib 加密存储（竞品借鉴低成本项，#759/#761）。
//
// 竞品（ClassGuard AES 类加载器 / JNIC 字符串加密密钥流）都把 native 载荷
// 以密文形态存放，静态提取不可直接加载。本模块提供打包侧（C++）与加载侧
// （NativeLoader 字节码）共享的 XOR 密钥流：
//   - xorshift32Step / xorStreamBytes：逐字节 XOR（每字节推进 state 取
//     `state & 0xFF`），算法与 NativeLoader 字节码内实现逐字节一致；
//   - kNativeLibEncryptedSuffix：加密资源入口后缀约定 `.enc`，NativeLoader
//     检测到后缀执行解密；sepacker 加壳 DLL 走同一存储形态（主线第 6 项
//     对接点）。
//
// 强度说明：非密码学（密钥藏于 NativeLoader 字节码常量），用途是把 jar 内
// 明文 dll 变成静态不可直接提取的 blob，抬高取证/静态分析门槛；强加密 +
// 密钥下沉 native 层属 sepacker 对接范围（native_library_packaging.md）。

#pragma once

#include <cstddef>
#include <cstdint>

namespace mj2x {

/// xorshift32 一步推进（与 NativeLoader 字节码内实现完全一致）。
inline std::uint32_t xorshift32Step(std::uint32_t state)
{
    std::uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/// 用 xorshift32 密钥流逐字节 XOR（原地）。每个输出字节先推进 state 再取
/// `state & 0xFF` 异或；密钥流纯状态驱动，与字节位置无关，C++ 打包侧与
/// Java 加载侧逐字节一致。key == 0 时用固定种子（与任意 key 等价）。
inline void xorStreamBytes(std::uint8_t* data, std::size_t size, std::uint32_t key)
{
    std::uint32_t state = key != 0 ? key : 0x9E3779B9u;
    for (std::size_t i = 0; i < size; ++i) {
        state = xorshift32Step(state);
        data[i] = static_cast<std::uint8_t>(data[i] ^ static_cast<std::uint8_t>(state & 0xFFu));
    }
}

/// 加密资源入口后缀约定：`<name>.dll` → `<name>.dll.enc`。
inline constexpr const char* kNativeLibEncryptedSuffix = ".enc";

}   // namespace mj2x
