#include "ldc_preprocessor.hpp"

#include <any>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// 字符串 → Utf8StringRef（经 arena intern）。arena 归 CompilerIndex 持有，比预处理期长命。
ca::str::Utf8StringRef intern(ca::str::Utf8StringArena& arena, const char* s) {
    return arena.intern(s);
}

// 判断 ldc cst 是否是 Handle。
bool isHandleCst(const std::any& cst) {
    return cst.type() == typeid(Handle);
}

// 判断 ldc cst 是否是 MethodType（Type，sort == METHOD）。
bool isMethodTypeCst(const std::any& cst) {
    if (cst.type() != typeid(Type)) return false;
    return std::any_cast<Type>(cst).getSort() == Type::METHOD;
}

// 给一个方法的指令列表，在 ldc 节点处插入展开序列，并移除原 ldc。
// 返回是否改写过（用于跳过未改写的方法）。
void expandLdcInMethod(ca::str::Utf8StringArena& arena, MethodNode* m) {
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
    if (targets.empty()) return;

    for (auto* ldc : targets) {
        if (isHandleCst(ldc->cst)) {
            Handle h = std::any_cast<Handle>(ldc->cst);
            // 展开序列（插入到 ldc 之前，然后删 ldc）：
            //   INVOKESTATIC MethodHandles.lookup()           ; → Lookup
            //   LDC <owner class>                              ; → Class
            //   per tag → findVirtual/findStatic/...(Class, name, MethodType)
            auto* callLookup = new MethodInsnNode(
                Opcodes::INVOKESTATIC,
                intern(arena, "java/lang/invoke/MethodHandles"),
                intern(arena, "lookup"),
                intern(arena, "()Ljava/lang/invoke/MethodHandles$Lookup;"),
                false);
            // owner class：用 LDC Type OBJECT（Class 常量）
            auto* ldcOwner = new LdcInsnNode(Type::getObjectType(h.getOwner()));
            // 方法名
            ca::str::Utf8StringRef mname = h.getName();
            ca::str::Utf8StringRef mdesc = h.getDesc();
            // 按 tag 选 Lookup 方法 + descriptor
            const char* lookupMethodName = "findVirtual";
            const char* lookupMethodDesc =
                "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
                "Ljava/lang/invoke/MethodHandle;";
            int tag = h.getTag();
            switch (tag) {
                case Opcodes::H_GETFIELD:
                    lookupMethodName = "findGetter";
                    lookupMethodDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                                       "Ljava/lang/invoke/MethodHandle;";
                    break;
                case Opcodes::H_GETSTATIC:
                    lookupMethodName = "findStaticGetter";
                    lookupMethodDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                                       "Ljava/lang/invoke/MethodHandle;";
                    break;
                case Opcodes::H_PUTFIELD:
                    lookupMethodName = "findSetter";
                    lookupMethodDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                                       "Ljava/lang/invoke/MethodHandle;";
                    break;
                case Opcodes::H_PUTSTATIC:
                    lookupMethodName = "findStaticSetter";
                    lookupMethodDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)"
                                       "Ljava/lang/invoke/MethodHandle;";
                    break;
                case Opcodes::H_INVOKEVIRTUAL:
                case Opcodes::H_INVOKEINTERFACE:
                    lookupMethodName = "findVirtual";
                    break;
                case Opcodes::H_INVOKESTATIC:
                    lookupMethodName = "findStatic";
                    break;
                case Opcodes::H_INVOKESPECIAL:
                    lookupMethodName = "findSpecial";
                    lookupMethodDesc = "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
                                       "Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;";
                    break;
                case Opcodes::H_NEWINVOKESPECIAL:
                    lookupMethodName = "findConstructor";
                    lookupMethodDesc = "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)"
                                       "Ljava/lang/invoke/MethodHandle;";
                    break;
                default:
                    break;
            }
            // 对 field tag（getter/setter），descriptor 参数是字段类型 Class，不是 MethodType。
            // R1b 简化：field tag 也用 findGetter(Class, name, Class) 的形式，
            //   需要把字段 desc（如 "I"）转成 Class。这里用 LDC Type OBJECT(descriptor)。
            // 为控制复杂度，R1b 先完整实现 method handle（findVirtual/findStatic/findSpecial/
            //   findConstructor），field handle 留个 TODO（遇到再补，SnakeGame 没有 field handle ldc）。
            (void)lookupMethodDesc;
            // 构造 MethodType（descriptor → MethodType.fromMethodDescriptorString）：
            //   LDC <mdesc string>
            //   INVOKESTATIC getClass().getClassLoader()  —— 复用 MethodType 展开逻辑（见下）
            // R1b 简化：method handle 用 findVirtual/findStatic/findConstructor 的通用形式。
            auto* ldcMdesc = new LdcInsnNode(std::string(reinterpret_cast<const char*>(mdesc.data()), mdesc.byte_length()));
            // MethodType.fromMethodDescriptorString(desc, classloader)
            // 先取 classloader：INVOKESTATIC java/lang/Class... 不行，要先有 class。
            // 用调用者的 getClass()：但这里在 ldc 处没有 this。
            // 简化：用系统 ClassLoader：INVOKESTATIC ClassLoader.getSystemClassLoader()
            auto* callSysCl = new MethodInsnNode(
                Opcodes::INVOKESTATIC,
                intern(arena, "java/lang/ClassLoader"),
                intern(arena, "getSystemClassLoader"),
                intern(arena, "()Ljava/lang/ClassLoader;"),
                false);
            auto* callFromDesc = new MethodInsnNode(
                Opcodes::INVOKESTATIC,
                intern(arena, "java/lang/invoke/MethodType"),
                intern(arena, "fromMethodDescriptorString"),
                intern(arena, "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/MethodType;"),
                false);
            // LDC method name string
            auto* ldcName = new LdcInsnNode(std::string(reinterpret_cast<const char*>(mname.data()), mname.byte_length()));
            // findXxx(Class, String, MethodType)
            auto* callFind = new MethodInsnNode(
                Opcodes::INVOKEVIRTUAL,
                intern(arena, "java/lang/invoke/MethodHandles$Lookup"),
                intern(arena, lookupMethodName),
                intern(arena, lookupMethodDesc),
                false);

            // 插入序列到 ldc 之前，删 ldc
            m->instructions.insertBefore(ldc, callFind);
            m->instructions.insertBefore(callFind, ldcName);
            m->instructions.insertBefore(ldcName, callFromDesc);
            m->instructions.insertBefore(callFromDesc, callSysCl);
            m->instructions.insertBefore(callSysCl, ldcMdesc);
            m->instructions.insertBefore(ldcMdesc, ldcOwner);
            m->instructions.insertBefore(ldcOwner, callLookup);
            m->instructions.remove(ldc);
        } else if (isMethodTypeCst(ldc->cst)) {
            // ldc MethodType → fromMethodDescriptorString(desc, systemClassLoader)
            Type t = std::any_cast<Type>(ldc->cst);
            auto desc = t.getDescriptor();
            auto* ldcDesc = new LdcInsnNode(std::string(reinterpret_cast<const char*>(desc.data()), desc.byte_length()));
            auto* callSysCl = new MethodInsnNode(
                Opcodes::INVOKESTATIC,
                intern(arena, "java/lang/ClassLoader"),
                intern(arena, "getSystemClassLoader"),
                intern(arena, "()Ljava/lang/ClassLoader;"),
                false);
            auto* callFromDesc = new MethodInsnNode(
                Opcodes::INVOKESTATIC,
                intern(arena, "java/lang/invoke/MethodType"),
                intern(arena, "fromMethodDescriptorString"),
                intern(arena, "(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/invoke/MethodType;"),
                false);
            m->instructions.insertBefore(ldc, callFromDesc);
            m->instructions.insertBefore(callFromDesc, callSysCl);
            m->instructions.insertBefore(callSysCl, ldcDesc);
            m->instructions.remove(ldc);
        }
    }
}

} // namespace

void LdcPreprocessor::process(CompilerIndex& index) {
    auto& arena = index.arena();
    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;
        for (auto* m : cls.classNode->methods) {
            expandLdcInMethod(arena, m);
        }
    }
}

} // namespace mj2x
