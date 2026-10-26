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

/// @brief tree 序列化旗标：与 output_writer 同口径（modified 判据一致）。
static ca::i32 writerFlags(const CompilerClass& cls)
{
    return cls.modified ? ClassWriter::COMPUTE_FRAMES : ClassWriter::COMPUTE_MAXS;
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
