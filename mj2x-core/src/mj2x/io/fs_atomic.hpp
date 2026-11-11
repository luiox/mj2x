#pragma once

// fs_atomic.hpp
//
// 原子文件替换写：临时文件 + rename 收尾。历史上同一机制有两份并行实现
// （mj2x-cli/src/native_project_builder.cpp 的 copyLibraryAtomically 与
// vmx64 post-link 加密器的回写段），且两者各缺一角（一份缺 WRITE_THROUGH
// 与唯一 token，一份失败不清理临时文件）——本文件收编为单源。
//
// 失败语义统一：任何一步失败都先删除临时文件再返回 Err，目标路径保持
// 原样（不产出半写/半拷贝的产物，也不留临时文件尸体）。

#include <filesystem>
#include <string>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

namespace mj2x {

/// @brief 把字节序列原子写入 @p dst：先写唯一临时文件，全部成功后 rename
///        替换目标。
///
/// @details
/// 临时名 = dst + ".mj2x-atomic-<token>"（steady_clock 计数 + 进程内单调
/// 序号，进程内/跨进程均不撞名）。写失败（打不开/短写）或替换失败都不
/// 触碰 dst 原有内容。不创建父目录——调用方自行保证 dst 的父目录存在
/// （需要自动建目录的消费方在调用前 create_directories，见
/// mj2x-cli 的 copyLibraryAtomically 包装）。
///
/// @param data 字节缓冲（可空指针当 size == 0，写出空文件合法）。
/// @return 失败携带可读错误文本（风格与 vmx64 post-link 加密器一致）。
ca::core::Result<void, std::string> writeFileAtomically(const std::filesystem::path& dst,
                                                        const ca::u8* data, ca::usize size);

/// @brief 把 @p src 原子复制为 @p dst：先 copy 到唯一临时文件，校验非空后
///        rename 替换目标。
///
/// @details
/// 复制结果必须是存在的非空常规文件——空产物对 mj2x 的消费面（native
/// 库交付）恒为失败，fail-loud 而不是静默交付空库。失败时不触碰 dst。
ca::core::Result<void, std::string> copyFileAtomically(const std::filesystem::path& src,
                                                       const std::filesystem::path& dst);

}   // namespace mj2x
