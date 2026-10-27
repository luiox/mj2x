#include "compiler_index.hpp"

#include <map>
#include <string>

namespace mj2x {

namespace {

}   // namespace

void refreshMethodSnapshots(CompilerClass& cls)
{
    if (!cls.dom) return;

    // key 锚定迁移：现快照按 name:desc 收表，DOM 扫描重建（顺序随 DOM），
    // 管线旗标/编译产物随键带走；DOM 侧消失的条目（无移除入口，防御面）
    // 一并丢弃。
    std::map<std::string, CompilerMethod> carried;
    for (auto& method : cls.methods) {
        carried.emplace(method.name + ":" + method.desc, std::move(method));
    }
    cls.methods.clear();
    for (auto& dm : cls.dom->methods) {
        const auto name = std::string(cls.dom->nameOf(dm));
        const auto desc = std::string(cls.dom->cp.utf8Of(dm.descCp));
        CompilerMethod cm;
        auto           it = carried.find(name + ":" + desc);
        if (it != carried.end()) {
            cm = std::move(it->second);
            carried.erase(it);
        }
        cm.name      = name;
        cm.desc      = desc;
        cm.access    = dm.access;
        {
            // maxStack/maxLocals 同源（Code 属性定长头；无 Code = 0）。
            cm.maxStack  = 0;
            cm.maxLocals = 0;
            auto code    = cls.dom->decodeCode(dm);
            if (code.is_ok()) {
                auto* c = std::move(code).unwrap();
                if (c != nullptr) {
                    cm.maxStack  = c->maxStack;
                    cm.maxLocals = c->maxLocals;
                }
            }
        }
        cls.methods.push_back(std::move(cm));
    }

    std::map<std::string, CompilerField> carriedFields;
    for (auto& field : cls.fields) {
        carriedFields.emplace(field.name + ":" + field.desc, std::move(field));
    }
    cls.fields.clear();
    for (auto& df : cls.dom->fields) {
        const auto name = std::string(cls.dom->nameOf(df));
        const auto desc = std::string(cls.dom->cp.utf8Of(df.descCp));
        CompilerField cf;
        auto          it = carriedFields.find(name + ":" + desc);
        if (it != carriedFields.end()) {
            cf = std::move(it->second);
            carriedFields.erase(it);
        }
        cf.name   = name;
        cf.desc   = desc;
        cf.access = df.access;
        cls.fields.push_back(std::move(cf));
    }
}

void refreshSnapshots(CompilerIndex& index)
{
    for (auto& [name, cls] : index.getClasses()) {
        (void)name;
        refreshMethodSnapshots(cls);
    }
}

void CompilerIndex::addClass(const std::string& name, CompilerClass cls)
{
    classes_[name] = std::move(cls);
}

CompilerClass* CompilerIndex::getClass(const std::string& name)
{
    auto it = classes_.find(name);
    return it != classes_.end() ? &it->second : nullptr;
}

void CompilerIndex::addJarEntry(JarEntryRecord entry)
{
    jarEntries_.push_back(std::move(entry));
}

void CompilerIndex::upsertJarResource(const std::string& name, std::vector<ca::u8> data)
{
    for (auto& entry : jarEntries_) {
        if (entry.name == name) {
            entry.data    = std::move(data);
            entry.isClass = false;
            entry.className.clear();
            return;
        }
    }

    JarEntryRecord entry;
    entry.name    = name;
    entry.data    = std::move(data);
    entry.isClass = false;
    jarEntries_.push_back(std::move(entry));
}

}   // namespace mj2x
