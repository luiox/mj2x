#pragma once

/// @file        dom_edit.hpp
/// @brief       批 4 改写载体的 ClassSession 作用域（pre/postprocessor 共享）。
///
/// 批 4 起 pre/postprocessor 直接改 DOM（libjbc ClassSession/CodeEditor 载体，
/// 语义 intern/promote/lower 全走库契约），DOM 是改写权威。会话构造接管
/// CompilerClass.dom，commit 经 writeClass→readClass 字节往返还回（真实
/// 编解码器纪律，与管线同步同一通道（批 6d 同步器退役后为唯一通道）；ClassFile 不可移动——AttrSlot
/// 的 lazy span 指向 source_，移动即悬垂，故不做指针交换捷径）。
///
/// fail-loud：会话打开/提交失败一律抛 std::runtime_error（pipeline 边界
/// 既有 catch 面），不留半截改写。

#include <stdexcept>
#include <string>
#include <utility>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/log/log_macros.hpp"

#include "mj2x/core/compiler_index.hpp"
#include "mj2x/core/jvm_opcodes.hpp"

namespace mj2x {

/// @brief 以 CompilerClass.dom 为底打开改写会话（接管 dom）。
inline std::shared_ptr<jbc::ir::ClassSession> openDomEdit(CompilerClass& cls,
                                                          const std::string& name)
{
    auto opened = jbc::ir::makeSession(std::move(cls.dom));
    if (opened.is_err()) {
        throw std::runtime_error("dom_edit: makeSession failed for " + name);
    }
    return std::move(opened).unwrap();
}

/// @brief 把改写完成的会话 backing 经字节往返还回 CompilerClass.dom。
inline void commitDomEdit(CompilerClass& cls, const std::string& name,
                          std::shared_ptr<jbc::ir::ClassSession> session)
{
    auto written = jbc::core::writeClass(session->backing());
    if (written.is_err()) {
        const auto we = std::move(written).unwrap_err();
        throw std::runtime_error(std::string("dom_edit: writeClass failed for ") + name
                                 + ": " + jbc::core::to_string(we));
    }
    auto bytes = std::move(written).unwrap();
    auto read  = jbc::core::readClass(
        jbc::core::ByteView{bytes.data(), bytes.size()});
    if (read.is_err()) {
        throw std::runtime_error("dom_edit: readClass failed for " + name);
    }
    cls.dom = std::move(read).unwrap().file;
}

/// @brief 方法身份（name+desc）的 DOM 成员锚定（类内唯一，fail-loud）。
///
/// 读侧共享入口：mark/annotation/filter 族 preprocessor 的方法级查询一律
/// 经此落 DOM（批 6d 树退役后快照 name/desc 即身份源）。
inline jbc::core::MemberInfo& domMethodFor(CompilerClass& cls, const std::string& name,
                                           const std::string& desc)
{
    auto* dom = cls.dom.get();
    for (auto& dm : dom->methods) {
        if (dom->cp.utf8Equals(dm.nameCp, name) && dom->cp.utf8Equals(dm.descCp, desc)) {
            return dm;
        }
    }
    throw std::runtime_error("dom_edit: method not in DOM: " + name + desc);
}

}   // namespace mj2x
