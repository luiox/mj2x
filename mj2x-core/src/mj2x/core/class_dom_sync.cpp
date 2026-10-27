#include "class_dom_sync.hpp"

#include <map>
#include <memory>
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

void syncDomToTree(CompilerIndex& index)
{
    for (auto& [name, cls] : index.getClasses()) {
        if (!cls.dom) continue;

        // DOM 是批 4 后的改写权威：writeClass 保真字节 → 旧栈 ClassReader
        // 重建树（真实编解码器往返，与 tree→DOM 方向同一纪律）。
        auto written = jbc::core::writeClass(*cls.dom);
        if (written.is_err()) {
            throw std::runtime_error("class_dom_sync: writeClass failed for " + name);
        }
        auto bytes = std::move(written).unwrap();

        // CompilerMethod 管线状态按 name+desc（类内唯一）锚定迁移；DOM 侧
        // 新增方法（__regc_* 搬运产物等）无旧状态可迁，按默认状态落位。
        std::map<std::string, CompilerMethod> carried;
        for (auto& method : cls.methods) {
            if (method.methodNode == nullptr) continue;
            auto key = method.methodNode->name.to_std_string() + ":"
                     + method.methodNode->desc.to_std_string();
            carried.emplace(std::move(key), std::move(method));
        }
        cls.methods.clear();
        for (auto& field : cls.fields) {
            (void)field;   // CompilerField 无管线状态，重建即可
        }
        cls.fields.clear();

        auto reader = std::make_unique<jbytecode::core::ClassReader>(
            index.arena(), bytes.data(), static_cast<ca::i32>(bytes.size()));
        auto node = std::make_unique<jbytecode::tree::ClassNode>();
        reader->accept(node.get(), 0);
        cls.classNode = std::move(node);

        for (auto* mn : cls.classNode->methods) {
            CompilerMethod cm;
            auto key = mn->name.to_std_string() + ":" + mn->desc.to_std_string();
            auto it  = carried.find(key);
            if (it != carried.end()) {
                cm = std::move(it->second);
                carried.erase(it);
            }
            cm.methodNode = mn;
            cls.methods.push_back(std::move(cm));
        }
        for (auto* fn : cls.classNode->fields) {
            CompilerField cf;
            cf.fieldNode = fn;
            cls.fields.push_back(std::move(cf));
        }
        CA_LOGT_DEBUG("core", "dom->tree synced: {} ({} methods)\n", name.c_str(),
                      cls.methods.size());
    }
}

}   // namespace mj2x
