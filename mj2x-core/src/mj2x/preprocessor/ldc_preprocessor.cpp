#include "ldc_preprocessor.hpp"

#include "libca/core/any.hpp"
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// 字符串 → Utf8StringRef（经 arena intern）。arena 归 CompilerIndex 持有，比预处理期长命。
ca::str::Utf8StringRef intern(ca::str::Utf8StringArena& arena, const char* s)
{
    return arena.intern(s);
}

// 判断 ldc cst 是否是 Handle。
bool isHandleCst(const ca::core::Any& cst)
{
    return cst.as<Handle>() != nullptr;
}

// 判断 ldc cst 是否是 MethodType（Type，sort == METHOD）。
bool isMethodTypeCst(const ca::core::Any& cst)
{
    auto* p = cst.as<Type>();
    return p && p->getSort() == Type::METHOD;
}

/// 字段描述符为 primitive sort 时，承载其 Class 对象的包装类（Integer.TYPE 等）。
const char* primitiveBoxClass(int sort)
{
    switch (sort) {
    case Type::BOOLEAN: return "java/lang/Boolean";
    case Type::CHAR: return "java/lang/Character";
    case Type::BYTE: return "java/lang/Byte";
    case Type::SHORT: return "java/lang/Short";
    case Type::INT: return "java/lang/Integer";
    case Type::LONG: return "java/lang/Long";
    case Type::FLOAT: return "java/lang/Float";
    case Type::DOUBLE: return "java/lang/Double";
    default: return nullptr;
    }
}

/// 给一个方法的指令列表，在 ldc 节点处插入展开序列，并移除原 ldc。
/// 展开严格按 Lookup.findXxx 描述符的参数顺序压栈（栈校验见 ldc_preprocessor_test）：
///   findVirtual/findStatic:  (Class, String, MethodType)
///   findSpecial:             (Class, String, MethodType, Class caller)
///   findConstructor:         (Class, MethodType)
///   findGetter/findSetter 系: (Class, String, Class fieldType)
/// MethodType 一律经 MethodType.fromMethodDescriptorString(desc, loader) 构造，
/// loader 取调用者类的 getClassLoader()（保护态 jar 走自定义 cloader，系统
/// ClassLoader 解析不了应用类引用）。
/// @return 本方法是否实际发生 ldc 展开（供调用方置 cls.modified）。
bool expandLdcInMethod(ca::str::Utf8StringArena& arena, ca::str::Utf8StringRef callerClass,
                       MethodNode* m)
{
    // 收集要改写的 ldc 节点（边遍历边改 list 会迭代器失效，故先收集）
    std::vector<LdcInsnNode*> targets;
    for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
        if (insn->getType() == AbstractInsnNode::LDC_INSN) {
            auto* ldc = static_cast<LdcInsnNode*>(insn);
            if (isHandleCst(ldc->cst) || isMethodTypeCst(ldc->cst)) {
                targets.push_back(ldc);
            }
        }
    }
    if (targets.empty()) return false;

    // LDC String 常量（desc / name）。
    auto newStringLdc = [&](ca::str::Utf8StringRef value) {
        return m->create<LdcInsnNode>(
            std::string(reinterpret_cast<const char*>(value.data()), value.byte_length()));
    };

    // 压入 MethodType：LDC String(desc), LDC Class(caller), getClassLoader(),
    // MethodType.fromMethodDescriptorString(String, ClassLoader)。
    auto pushMethodTypeSeq = [&](std::vector<AbstractInsnNode*>& seq,
                                 ca::str::Utf8StringRef desc) {
        seq.push_back(newStringLdc(desc));
        seq.push_back(m->create<LdcInsnNode>(Type::getObjectType(callerClass)));
        seq.push_back(m->create<MethodInsnNode>(Opcodes::INVOKEVIRTUAL,
                                         intern(arena, "java/lang/Class"),
                                         intern(arena, "getClassLoader"),
                                         intern(arena, "()Ljava/lang/ClassLoader;"),
                                         false));
        seq.push_back(m->create<MethodInsnNode>(
            Opcodes::INVOKESTATIC,
            intern(arena, "java/lang/invoke/MethodType"),
            intern(arena, "fromMethodDescriptorString"),
            intern(arena,
                   "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/MethodType;"),
            false));
    };

    for (auto* ldc : targets) {
        std::vector<AbstractInsnNode*> seq;
        if (isHandleCst(ldc->cst)) {
            Handle                 h     = ldc->cst.cast<Handle>();
            ca::str::Utf8StringRef mname = h.getName();
            ca::str::Utf8StringRef mdesc = h.getDesc();
            const int              tag   = h.getTag();

            // Lookup receiver + declaring Class。
            seq.push_back(m->create<MethodInsnNode>(Opcodes::INVOKESTATIC,
                                             intern(arena, "java/lang/invoke/MethodHandles"),
                                             intern(arena, "lookup"),
                                             intern(arena, "()Ljava/lang/invoke/MethodHandles$Lookup;"),
                                             false));
            seq.push_back(m->create<LdcInsnNode>(Type::getObjectType(h.getOwner())));

            // findXxx 选择（按 tag），name/MethodType/fieldType/caller 按其描述符顺序入栈。
            const char* lookupMethodName = "findVirtual";
            const char* lookupMethodDesc =
                "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
                "Ljava/lang/invoke/MethodHandle;";
            switch (tag) {
            case Opcodes::H_GETFIELD:
            case Opcodes::H_GETSTATIC:
            case Opcodes::H_PUTFIELD:
            case Opcodes::H_PUTSTATIC:
            {
                // findGetter/findSetter 系：第三个参数是字段类型 Class，不是 MethodType。
                // primitive 字段类型没有 ldc 形态的 Class 常量，用 GETSTATIC <Box>.TYPE。
                lookupMethodName = tag == Opcodes::H_GETFIELD    ? "findGetter"
                                   : tag == Opcodes::H_GETSTATIC ? "findStaticGetter"
                                   : tag == Opcodes::H_PUTFIELD   ? "findSetter"
                                                                  : "findStaticSetter";
                lookupMethodDesc =
                    "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                    "Ljava/lang/invoke/MethodHandle;";
                seq.push_back(newStringLdc(mname));
                const Type fieldType = Type::getType(h.getDesc());
                if (const char* box = primitiveBoxClass(fieldType.getSort())) {
                    seq.push_back(m->create<FieldInsnNode>(Opcodes::GETSTATIC,
                                                    intern(arena, box),
                                                    intern(arena, "TYPE"),
                                                    intern(arena, "Ljava/lang/Class;")));
                }
                else {
                    seq.push_back(m->create<LdcInsnNode>(Type::getType(h.getDesc())));
                }
                break;
            }
            case Opcodes::H_INVOKESPECIAL:
                // findSpecial(Class, String, MethodType, Class specialCaller)。
                lookupMethodName = "findSpecial";
                lookupMethodDesc =
                    "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
                    "Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;";
                seq.push_back(newStringLdc(mname));
                pushMethodTypeSeq(seq, mdesc);
                seq.push_back(m->create<LdcInsnNode>(Type::getObjectType(callerClass)));
                break;
            case Opcodes::H_NEWINVOKESPECIAL:
                // findConstructor(Class, MethodType)：无 name 参数。
                lookupMethodName = "findConstructor";
                lookupMethodDesc =
                    "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)"
                    "Ljava/lang/invoke/MethodHandle;";
                pushMethodTypeSeq(seq, mdesc);
                break;
            case Opcodes::H_INVOKESTATIC: lookupMethodName = "findStatic"; [[fallthrough]];
            case Opcodes::H_INVOKEVIRTUAL:
            case Opcodes::H_INVOKEINTERFACE:
            default:
                // findVirtual/findStatic(Class, String, MethodType)。
                seq.push_back(newStringLdc(mname));
                pushMethodTypeSeq(seq, mdesc);
                break;
            }
            seq.push_back(m->create<MethodInsnNode>(Opcodes::INVOKEVIRTUAL,
                                             intern(arena, "java/lang/invoke/MethodHandles$Lookup"),
                                             intern(arena, lookupMethodName),
                                             intern(arena, lookupMethodDesc),
                                             false));
        }
        else {
            // ldc MethodType → fromMethodDescriptorString(desc, callerClassLoader)
            Type t = ldc->cst.cast<Type>();
            pushMethodTypeSeq(seq, t.getDescriptor());
        }

        // 按程序序插到 ldc 之前，然后用 removeAndDelete 摘链销毁 ldc
        //（对齐 init_clinit_mover 先例）。
        for (auto* node : seq) {
            m->instructions.insertBefore(ldc, node);
        }
        m->instructions.removeAndDelete(ldc);
    }
    return true;
}

}   // namespace

void LdcPreprocessor::process(CompilerIndex& index)
{
    auto& arena = index.arena();
    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;
        for (auto* m : cls.classNode->methods) {
            // 展开改写了指令流：类必须标记 modified，否则未翻译类以
            // COMPUTE_MAXS 写出，旧 StackMapTable 配新指令流必 VerifyError
            //（对齐 init_clinit_mover 的先例）。
            if (expandLdcInMethod(arena, cls.classNode->name, m)) cls.modified = true;
        }
    }
}

}   // namespace mj2x
