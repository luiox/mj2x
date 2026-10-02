#include "llvm_intrinsic_registry.hpp"

#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"

#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace mj2x {

// =============================================================================
// #1027 B2 单源化：本文件原为 92 条 (owner,name,desc,symbol) 手工对齐副本 +
// LLVM 类型文本。现改为从 RegC 权威表（builtinIntrinsicTable，含 llvmDirect
// 能力位）机械派生——新增 intrinsic 只改 regc_intrinsic_registry.cpp 一处。
//
// 派生口径（与切换前手工表逐字对齐，mj2x_unittest 的
// LlvmIntrinsicRegistryGoldenCross 以切换前全表为 golden 逐字段钉死）：
//   - Math/RuntimeFunction 纯标量：I→i32, J→i64, F→float, D→double（表内无
//     窄类型；Integer/Long 位操作 llvmDirect=false 不派生，保持 JNI 回退）。
//   - 包装类/Random/Builder 共用 crt 形参口径：Z/B→i8, C/S→i16, I→i32,
//     J→i64, F→float, D→double, L·[→ptr；V 在返回位 → 空。
//   - declareText = "declare <ret> @<symbol>(<args>)"（Random 前置隐式状态
//     ptr；Builder 无 declareText 字段）。
// =============================================================================

namespace {

bool sameText(ca::str::Utf8StringRef ref, const char* text)
{
    return std::string_view(reinterpret_cast<const char*>(ref.data()), ref.byte_length()) ==
           std::string_view(text);
}

/// crt 形参口径 → LLVM 类型（V 由调用方特判；空 desc 不应出现）。
const char* llvmCrtType(char head)
{
    switch (head) {
    case 'Z':
    case 'B': return "i8";
    case 'C':
    case 'S': return "i16";
    case 'I': return "i32";
    case 'J': return "i64";
    case 'F': return "float";
    case 'D': return "double";
    default: return "ptr";   // L...; / [
    }
}

/// desc "(II)I" → 参数类型头字符序列 + 返回头字符。
/// 注意必须真解析：L...; 是完整类型（跳到 ';'），不能按单字符切。
struct DescShape
{
    char               ret = 'V';
    std::vector<char>  args;
};

DescShape parseDesc(const char* desc)
{
    DescShape          shape;
    const std::string_view view(desc);
    const size_t       close = view.rfind(')');
    size_t             i     = 1;
    while (close != std::string_view::npos && i < close) {
        const char head = view[i];
        shape.args.push_back(head);
        if (head == 'L') {
            while (i < close && view[i] != ';') ++i;
        }
        else if (head == '[') {
            // 数组：组件类型紧随其后（表内现无数组项，解析仍保真）。
            size_t j = i + 1;
            while (j < close && view[j] == '[') ++j;
            if (j < close && view[j] == 'L') {
                while (j < close && view[j] != ';') ++j;
            }
            i = j;
        }
        ++i;
    }
    shape.ret = close + 1 < view.size() ? view[close + 1] : 'V';
    return shape;
}

const char* wrapperKindText(RegCIntrinsicNativeObjectKind kind)
{
    switch (kind) {
    case RegCIntrinsicNativeObjectKind::Int: return "int";
    case RegCIntrinsicNativeObjectKind::Long: return "long";
    case RegCIntrinsicNativeObjectKind::Short: return "short";
    case RegCIntrinsicNativeObjectKind::Byte: return "byte";
    case RegCIntrinsicNativeObjectKind::Float: return "float";
    case RegCIntrinsicNativeObjectKind::Double: return "double";
    case RegCIntrinsicNativeObjectKind::Bool: return "bool";
    case RegCIntrinsicNativeObjectKind::Char: return "char";
    default: return "";
    }
}

const char* wrapperShadowType(const char* kind)
{
    if (std::string_view(kind) == "int") return "%mj2x_int";
    if (std::string_view(kind) == "long") return "%mj2x_long";
    if (std::string_view(kind) == "short") return "%mj2x_short";
    if (std::string_view(kind) == "byte") return "%mj2x_byte";
    if (std::string_view(kind) == "float") return "%mj2x_float";
    if (std::string_view(kind) == "double") return "%mj2x_double";
    if (std::string_view(kind) == "bool") return "%mj2x_bool";
    return "%mj2x_char";
}

std::string wrapperShadowDecl(const char* kind)
{
    const std::string_view type = wrapperShadowType(kind);
    if (type == "%mj2x_int") return "%mj2x_int = type { i32 }";
    if (type == "%mj2x_long") return "%mj2x_long = type { i64 }";
    if (type == "%mj2x_short") return "%mj2x_short = type { i16 }";
    if (type == "%mj2x_byte") return "%mj2x_byte = type { i8 }";
    if (type == "%mj2x_float") return "%mj2x_float = type { float }";
    if (type == "%mj2x_double") return "%mj2x_double = type { double }";
    if (type == "%mj2x_bool") return "%mj2x_bool = type { i8 }";
    return "%mj2x_char = type { i16 }";
}

/// 派生文本驻留区（deque：push_back 不移走既有元素，const char* 稳定）；
/// 表为进程级 static，驻留区与其同寿。
std::deque<std::string>& textArena()
{
    static std::deque<std::string> arena;
    return arena;
}

const char* internText(std::string text)
{
    textArena().push_back(std::move(text));
    return textArena().back().c_str();
}

/// Math/RuntimeFunction 条目派生。
std::vector<LlvmIntrinsicEntry> deriveMathEntries()
{
    std::vector<LlvmIntrinsicEntry> out;
    for (const auto& id : builtinIntrinsicTable()) {
        if (!id.llvmDirect || id.emitKind != RegCIntrinsicEmitKind::RuntimeFunction) continue;
        const DescShape shape = parseDesc(id.desc);
        LlvmIntrinsicEntry entry{};
        entry.owner  = id.owner;
        entry.name   = id.name;
        entry.desc   = id.desc;
        entry.symbol = id.symbol;
        entry.returnLlvm = llvmCrtType(shape.ret);
        for (size_t i = 0; i < 2; ++i) {
            entry.argLlvm[i] = i < shape.args.size() ? llvmCrtType(shape.args[i]) : "";
        }
        std::string declare = std::string("declare ") + entry.returnLlvm + " @" + id.symbol + "(";
        for (size_t i = 0; i < shape.args.size(); ++i) {
            if (i != 0) declare += ", ";
            declare += llvmCrtType(shape.args[i]);
        }
        declare += ")";
        entry.declareText = internText(std::move(declare));
        out.push_back(entry);
    }
    return out;
}

/// 包装类装箱（WrapperValueOf）条目派生。
std::vector<LlvmWrapperBoxEntry> deriveBoxEntries()
{
    std::vector<LlvmWrapperBoxEntry> out;
    for (const auto& id : builtinIntrinsicTable()) {
        if (!id.llvmDirect || id.emitKind != RegCIntrinsicEmitKind::WrapperValueOf) continue;
        const DescShape     shape = parseDesc(id.desc);
        LlvmWrapperBoxEntry entry{};
        entry.owner  = id.owner;
        entry.name   = id.name;
        entry.desc   = id.desc;
        entry.kind   = wrapperKindText(id.nativeObjectKind);
        entry.shadowType = wrapperShadowType(entry.kind);
        entry.shadowDecl = internText(wrapperShadowDecl(entry.kind));
        entry.symbol = id.symbol;
        entry.argLlvm = llvmCrtType(shape.args[0]);
        entry.declareText = internText(std::string("declare ") + entry.shadowType + " @" +
                                       id.symbol + "(" + entry.argLlvm + ")");
        out.push_back(entry);
    }
    return out;
}

/// 包装类拆箱（WrapperUnbox）条目派生；窄结果扩展：S/B→sext，Z/C→zext。
std::vector<LlvmWrapperUnboxEntry> deriveUnboxEntries()
{
    std::vector<LlvmWrapperUnboxEntry> out;
    for (const auto& id : builtinIntrinsicTable()) {
        if (!id.llvmDirect || id.emitKind != RegCIntrinsicEmitKind::WrapperUnbox) continue;
        const DescShape shape = parseDesc(id.desc);
        LlvmWrapperUnboxEntry entry{};
        entry.owner  = id.owner;
        entry.name   = id.name;
        entry.desc   = id.desc;
        entry.kind   = wrapperKindText(id.nativeObjectKind);
        entry.shadowType = wrapperShadowType(entry.kind);
        entry.shadowDecl = internText(wrapperShadowDecl(entry.kind));
        entry.symbol = id.symbol;
        entry.retLlvm = llvmCrtType(shape.ret);
        const char* ext = "";
        switch (shape.ret) {
        case 'S':
        case 'B': ext = "sext"; break;
        case 'Z':
        case 'C': ext = "zext"; break;
        default: break;
        }
        entry.retExt = ext;
        entry.declareText = internText(std::string("declare ") + entry.retLlvm + " @" + id.symbol +
                                       "(" + entry.shadowType + ")");
        out.push_back(entry);
    }
    return out;
}

/// Random 状态机条目派生：RandomInit 首条（对齐切换前表布局，matchLlvmRandomMethod
/// 跳过 [0]）+ NativeObjectMethod(Random) 其余；两趟保证 init 在前。
std::vector<LlvmRandomEntry> deriveRandomEntries()
{
    std::vector<LlvmRandomEntry> out;
    for (const auto& id : builtinIntrinsicTable()) {
        if (!id.llvmDirect || id.emitKind != RegCIntrinsicEmitKind::RandomInit) continue;
        const DescShape shape = parseDesc(id.desc);
        LlvmRandomEntry entry{};
        entry.owner  = id.owner;
        entry.name   = id.name;
        entry.desc   = id.desc;
        entry.symbol = id.symbol;
        entry.retLlvm = shape.ret == 'V' ? "void" : llvmCrtType(shape.ret);
        std::string params = "ptr";
        for (const char head : shape.args) {
            params += std::string(", ") + llvmCrtType(head);
        }
        entry.declareText =
            internText(std::string("declare ") + entry.retLlvm + " @" + id.symbol + "(" +
                       params + ")");
        out.push_back(entry);
    }
    for (const auto& id : builtinIntrinsicTable()) {
        const bool isMethod = id.llvmDirect &&
                              id.emitKind == RegCIntrinsicEmitKind::NativeObjectMethod &&
                              id.nativeObjectKind == RegCIntrinsicNativeObjectKind::Random;
        if (!isMethod) continue;
        const DescShape shape = parseDesc(id.desc);
        LlvmRandomEntry entry{};
        entry.owner  = id.owner;
        entry.name   = id.name;
        entry.desc   = id.desc;
        entry.symbol = id.symbol;
        entry.retLlvm = shape.ret == 'V' ? "void" : llvmCrtType(shape.ret);
        std::string params = "ptr";
        for (const char head : shape.args) {
            params += std::string(", ") + llvmCrtType(head);
        }
        entry.declareText =
            internText(std::string("declare ") + entry.retLlvm + " @" + id.symbol + "(" +
                       params + ")");
        out.push_back(entry);
    }
    return out;
}

/// StringBuilder/StringBuffer 状态机条目派生：kind 0=init 1=append 2=method。
std::vector<LlvmBuilderEntry> deriveBuilderEntries()
{
    std::vector<LlvmBuilderEntry> out;
    for (const auto& id : builtinIntrinsicTable()) {
        if (!id.llvmDirect) continue;
        int kind = -1;
        if (id.emitKind == RegCIntrinsicEmitKind::StringBuilderInit) kind = 0;
        else if (id.emitKind == RegCIntrinsicEmitKind::StringBuilderAppend) kind = 1;
        else if (id.emitKind == RegCIntrinsicEmitKind::StringBuilderMethod) kind = 2;
        if (kind < 0) continue;
        const DescShape shape = parseDesc(id.desc);
        LlvmBuilderEntry entry{};
        entry.owner  = id.owner;
        entry.name   = id.name;
        entry.desc   = id.desc;
        entry.symbol = id.symbol;
        entry.kind   = kind;
        if (kind == 2) {
            // method 的 out 参数类型：V→空（纯 out 回写），其余按 crt 口径。
            entry.outLlvm = shape.ret == 'V' ? "" : llvmCrtType(shape.ret);
        }
        else {
            entry.outLlvm = "";
        }
        for (size_t i = 0; i < 3; ++i) {
            entry.argLlvm[i] = i < shape.args.size() ? llvmCrtType(shape.args[i]) : "";
        }
        out.push_back(entry);
    }
    return out;
}

const std::vector<LlvmIntrinsicEntry>& mathTable()
{
    static const std::vector<LlvmIntrinsicEntry> table = deriveMathEntries();
    return table;
}
const std::vector<LlvmWrapperBoxEntry>& boxTable()
{
    static const std::vector<LlvmWrapperBoxEntry> table = deriveBoxEntries();
    return table;
}
const std::vector<LlvmWrapperUnboxEntry>& unboxTable()
{
    static const std::vector<LlvmWrapperUnboxEntry> table = deriveUnboxEntries();
    return table;
}
const std::vector<LlvmRandomEntry>& randomTable()
{
    static const std::vector<LlvmRandomEntry> table = deriveRandomEntries();
    return table;
}
const std::vector<LlvmBuilderEntry>& builderTable()
{
    static const std::vector<LlvmBuilderEntry> table = deriveBuilderEntries();
    return table;
}

}   // namespace

const LlvmIntrinsicEntry* matchLlvmIntrinsic(ca::str::Utf8StringRef owner,
                                             ca::str::Utf8StringRef name,
                                             ca::str::Utf8StringRef desc)
{
    for (const auto& entry : mathTable()) {
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

const LlvmWrapperBoxEntry* matchLlvmWrapperBox(ca::str::Utf8StringRef owner,
                                               ca::str::Utf8StringRef name,
                                               ca::str::Utf8StringRef desc)
{
    for (const auto& entry : boxTable()) {
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

const LlvmWrapperUnboxEntry* matchLlvmWrapperUnbox(ca::str::Utf8StringRef owner,
                                                   ca::str::Utf8StringRef name,
                                                   ca::str::Utf8StringRef desc)
{
    for (const auto& entry : unboxTable()) {
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

const LlvmRandomEntry* matchLlvmRandomInit(ca::str::Utf8StringRef owner,
                                           ca::str::Utf8StringRef name,
                                           ca::str::Utf8StringRef desc)
{
    // RandomInit 是派生序列首条（kEntries 中 RandomInit 唯一）。
    if (!randomTable().empty() && sameText(owner, randomTable()[0].owner) &&
        sameText(name, randomTable()[0].name) && sameText(desc, randomTable()[0].desc)) {
        return &randomTable()[0];
    }
    return nullptr;
}

const LlvmRandomEntry* matchLlvmRandomMethod(ca::str::Utf8StringRef owner,
                                             ca::str::Utf8StringRef name,
                                             ca::str::Utf8StringRef desc)
{
    for (size_t index = 1; index < randomTable().size(); ++index) {
        const auto& entry = randomTable()[index];
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

const LlvmBuilderEntry* matchLlvmBuilder(ca::str::Utf8StringRef owner,
                                         ca::str::Utf8StringRef name,
                                         ca::str::Utf8StringRef desc)
{
    for (const auto& entry : builderTable()) {
        if (sameText(owner, entry.owner) && sameText(name, entry.name) &&
            sameText(desc, entry.desc)) {
            return &entry;
        }
    }
    return nullptr;
}

bool isLlvmBuilderAllocation(ca::str::Utf8StringRef typeDesc)
{
    return sameText(typeDesc, "java/lang/StringBuilder") ||
           sameText(typeDesc, "java/lang/StringBuffer");
}

}   // namespace mj2x
