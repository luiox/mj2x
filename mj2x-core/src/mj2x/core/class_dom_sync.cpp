#include "class_dom_sync.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "jbc/core/bytes.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/log/log_macros.hpp"

namespace mj2x {

using jbytecode::core::ClassWriter;

namespace {

/// @brief tree 序列化旗标：恒 plain（0）。
static ca::i32 writerFlags(const CompilerClass& cls)
{
    (void)cls;
    // 旗标恒 0（plain）：COMPUTE_MAXS/FRAMES 的帧计算在旧栈 writer 上有
    // 树/arena 侧副作用（批 2 实证：COMPUTE 同步致 LLVM 样例回退 + dump
    // 段错误；plain 序列化无此面）。帧重算归 output_writer / 批 6 的
    // smt_build，本同步只求「如实序列化当前 tree 状态」。
    return 0;
}

static std::vector<ca::u8> serializeTree(const CompilerClass& cls,
                                         ca::str::Utf8StringArena& arena)
{
    ClassWriter writer(arena, writerFlags(cls));
    cls.classNode->accept(&writer);
    return writer.toByteArray();
}

}   // namespace

void syncTreeToDom(CompilerIndex& index)
{
    for (auto& [name, cls] : index.getClasses()) {
        if (!cls.classNode) continue;

        auto bytes = serializeTree(cls, index.arena());

        auto read = jbc::core::readClass(jbc::core::ByteView{bytes.data(), bytes.size()});
        if (read.is_err()) {
            throw std::runtime_error("class_dom_sync: readClass failed for " + name);
        }
        auto result = std::move(read).unwrap();
        // Recovered = 主体可读的降级事实保留（libjbc 设计：Recovered 模型照样
        // 可用、可原样写回）——与旧栈「读到什么写什么」口径一致，不拒绝。

        cls.dom = std::move(result.file);
        CA_LOGT_DEBUG("core", "dom synced: {} ({} methods)\n", name.c_str(),
                      cls.dom->methods.size());
    }
}

}   // namespace mj2x
