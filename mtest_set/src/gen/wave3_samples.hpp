// wave3_samples.hpp - G12/G13/G14/G15 semantic samples（#820）。
//
// 吸收三家外部测试套件（huzpsb/JavaObfuscatorTest、bludnov/Obfuscation-Tester、
// ttvar/jvm-obf-tester）的语义覆盖面，补 mtest_set 缺失的类别：
//   G12 StringOps     字符串核心操作（length/charAt / equals/compareTo）
//   G13 BitOps        位运算（移位/掩码/异或 / 超界移位掩码）
//   G14 DeepRecursion 递归调用（fib 8 深度，验证调用栈语义）
//   G15 CollectionOps 接口调用（List.size 经 INVOKEINTERFACE）
// 全部为干净字节码（生成器不产混淆样本），行为契约进 core_fixture_registry。
// 其中 shift_out_of_range / string_compare 是四后端语义回归集成员
//（X2：掩码/字符串比较属跨后端易漂移点，历史缺陷是 RegC/LLVM 缺 JVM 掩码）。

#pragma once

#include "sample_builder.hpp"

namespace mtest_set::wave3 {

using ca::str::ZUtf8StringRef;
using jbytecode::core::Label;
using jbytecode::core::Opcodes;

inline auto ref(const char* text)
{
    return ZUtf8StringRef::from_static(text);
}

inline SampleBundle single(const char* path, std::vector<ca::u8> bytes)
{
    return singleArtifact(path, std::move(bytes));
}

/// G12：String.length() + String.charAt()（INVOKEVIRTUAL）。
/// int run(String s) { return s.length() * 100 + s.charAt(0); }
inline std::vector<ca::u8> genStringOps(ca::str::Utf8StringArena& arena)
{
    auto* run = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                               ref("run"), ref("(Ljava/lang/String;)I"),
                               ref(""), /*exceptions*/ {});
    run->visitCode();
    // s.length() * 100
    run->visitVarInsn(Opcodes::ALOAD, 0);
    run->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/String"),
                         ref("length"), ref("()I"), false);
    run->visitIntInsn(Opcodes::BIPUSH, 100);
    run->visitInsn(Opcodes::IMUL);
    // + s.charAt(0)
    run->visitVarInsn(Opcodes::ALOAD, 0);
    run->visitInsn(Opcodes::ICONST_0);
    run->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/String"),
                         ref("charAt"), ref("(I)C"), false);
    run->visitInsn(Opcodes::IADD);
    run->visitInsn(Opcodes::IRETURN);
    run->visitMaxs(0, 0);
    run->visitEnd();

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("StringOps");
    cn.superName = ref("java/lang/Object");
    cn.methods.push_back(run);
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

/// G13：纯位运算（移位/无符号右移/掩码/异或）。
/// int run(int v) { return (v << 3) | ((v >>> 1) ^ (v & 0x7F)); }
inline std::vector<ca::u8> genBitOps(ca::str::Utf8StringArena& arena)
{
    auto* run = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                               ref("run"), ref("(I)I"), ref(""), /*exceptions*/ {});
    run->visitCode();
    // (v << 3)
    run->visitVarInsn(Opcodes::ILOAD, 0);
    run->visitIntInsn(Opcodes::BIPUSH, 3);
    run->visitInsn(Opcodes::ISHL);
    // | ((v >>> 1) ^ (v & 0x7F))
    run->visitVarInsn(Opcodes::ILOAD, 0);
    run->visitInsn(Opcodes::ICONST_1);
    run->visitInsn(Opcodes::IUSHR);
    run->visitVarInsn(Opcodes::ILOAD, 0);
    run->visitIntInsn(Opcodes::BIPUSH, 127);
    run->visitInsn(Opcodes::IAND);
    run->visitInsn(Opcodes::IXOR);
    run->visitInsn(Opcodes::IOR);
    run->visitInsn(Opcodes::IRETURN);
    run->visitMaxs(0, 0);
    run->visitEnd();

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("BitOps");
    cn.superName = ref("java/lang/Object");
    cn.methods.push_back(run);
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

/// G14：递归调用（fib，自调用 8 深度）。
/// int run(int n) { return n <= 1 ? n : run(n - 1) + run(n - 2); }
inline std::vector<ca::u8> genDeepRecursion(ca::str::Utf8StringArena& arena)
{
    auto* run = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                               ref("run"), ref("(I)I"), ref(""), /*exceptions*/ {});
    run->visitCode();
    auto* baseCase = new Label();
    auto* end      = new Label();
    // n <= 1 → base
    run->visitVarInsn(Opcodes::ILOAD, 0);
    run->visitInsn(Opcodes::ICONST_1);
    run->visitJumpInsn(Opcodes::IF_ICMPLE, baseCase);
    // run(n-1) + run(n-2)
    run->visitVarInsn(Opcodes::ILOAD, 0);
    run->visitInsn(Opcodes::ICONST_1);
    run->visitInsn(Opcodes::ISUB);
    run->visitMethodInsn(Opcodes::INVOKESTATIC, ref("DeepRecursion"),
                         ref("run"), ref("(I)I"), false);
    run->visitVarInsn(Opcodes::ILOAD, 0);
    run->visitInsn(Opcodes::ICONST_2);
    run->visitInsn(Opcodes::ISUB);
    run->visitMethodInsn(Opcodes::INVOKESTATIC, ref("DeepRecursion"),
                         ref("run"), ref("(I)I"), false);
    run->visitInsn(Opcodes::IADD);
    run->visitJumpInsn(Opcodes::GOTO, end);
    // base: return n
    run->visitLabel(baseCase);
    run->visitVarInsn(Opcodes::ILOAD, 0);
    run->visitInsn(Opcodes::IRETURN);
    // end: return acc
    run->visitLabel(end);
    run->visitInsn(Opcodes::IRETURN);
    run->visitMaxs(0, 0);
    run->visitEnd();

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("DeepRecursion");
    cn.superName = ref("java/lang/Object");
    cn.methods.push_back(run);
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

/// G13 补充：超界/负数移位计数（JVM &31/&63 掩码是定义行为，C/LLVM 裸移位是
/// UB/poison——四后端语义回归集成员）。移位计数走参数（防 javac/mj2x 常量
/// 折叠吃掉运行期移位，测不到发射端掩码）：
/// int iShl(int a, int b)  { return (1<<a) ^ (-1<<b) ^ (5<<3); }       // (33,-1) → -2147483574
/// int iShr(int a, int b)  { return (-1>>a) ^ (-256>>b) ^ (96>>2); }   // (32,33) → 103
/// int iUShr(int a, int b) { return (-1>>>a) ^ (-1>>>b) ^ (-1>>>31); } // (32,33) → -2147483647
/// long lShl(int a, int b) { return (1L<<a) ^ (1L<<b) ^ (1L<<63); }    // (64,65) → -9223372036854775805
/// long lUShr(int a, int b){ return (-1L>>>a) ^ (-1L>>>b) ^ (-1L>>>63); } // (64,65) → -9223372036854775807
inline std::vector<ca::u8> genShiftOutOfRange(ca::str::Utf8StringArena& arena)
{
    auto makeInt = [&](const char* name, auto emitBody) {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, ref(name),
                                 ref("(II)I"), ref(""), /*exceptions*/ {});
        m->visitCode();
        emitBody(m);
        m->visitInsn(Opcodes::IRETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        return m;
    };
    auto makeLong = [&](const char* name, auto emitBody) {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, ref(name),
                                 ref("(II)J"), ref(""), /*exceptions*/ {});
        m->visitCode();
        emitBody(m);
        m->visitInsn(Opcodes::LRETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        return m;
    };

    // iShl: (1<<a) ^ (-1<<b) ^ (5<<3)
    auto* iShl = makeInt("iShl", [&](MethodNode* m) {
        m->visitInsn(Opcodes::ICONST_1);
        m->visitVarInsn(Opcodes::ILOAD, 0);
        m->visitInsn(Opcodes::ISHL);
        m->visitInsn(Opcodes::ICONST_M1);
        m->visitVarInsn(Opcodes::ILOAD, 1);
        m->visitInsn(Opcodes::ISHL);
        m->visitInsn(Opcodes::IXOR);
        m->visitIntInsn(Opcodes::BIPUSH, 5);
        m->visitInsn(Opcodes::ICONST_3);
        m->visitInsn(Opcodes::ISHL);
        m->visitInsn(Opcodes::IXOR);
    });
    // iShr: (-1>>a) ^ (-256>>b) ^ (96>>2)
    auto* iShr = makeInt("iShr", [&](MethodNode* m) {
        m->visitInsn(Opcodes::ICONST_M1);
        m->visitVarInsn(Opcodes::ILOAD, 0);
        m->visitInsn(Opcodes::ISHR);
        m->visitIntInsn(Opcodes::SIPUSH, -256);
        m->visitVarInsn(Opcodes::ILOAD, 1);
        m->visitInsn(Opcodes::ISHR);
        m->visitInsn(Opcodes::IXOR);
        m->visitIntInsn(Opcodes::BIPUSH, 96);
        m->visitInsn(Opcodes::ICONST_2);
        m->visitInsn(Opcodes::ISHR);
        m->visitInsn(Opcodes::IXOR);
    });
    // iUShr: (-1>>>a) ^ (-1>>>b) ^ (-1>>>31)
    auto* iUShr = makeInt("iUShr", [&](MethodNode* m) {
        m->visitInsn(Opcodes::ICONST_M1);
        m->visitVarInsn(Opcodes::ILOAD, 0);
        m->visitInsn(Opcodes::IUSHR);
        m->visitInsn(Opcodes::ICONST_M1);
        m->visitVarInsn(Opcodes::ILOAD, 1);
        m->visitInsn(Opcodes::IUSHR);
        m->visitInsn(Opcodes::IXOR);
        m->visitInsn(Opcodes::ICONST_M1);
        m->visitIntInsn(Opcodes::BIPUSH, 31);
        m->visitInsn(Opcodes::IUSHR);
        m->visitInsn(Opcodes::IXOR);
    });
    // lShl: (1L<<a) ^ (1L<<b) ^ (1L<<63)
    auto* lShl = makeLong("lShl", [&](MethodNode* m) {
        m->visitInsn(Opcodes::LCONST_1);
        m->visitVarInsn(Opcodes::ILOAD, 0);
        m->visitInsn(Opcodes::LSHL);
        m->visitInsn(Opcodes::LCONST_1);
        m->visitVarInsn(Opcodes::ILOAD, 1);
        m->visitInsn(Opcodes::LSHL);
        m->visitInsn(Opcodes::LXOR);
        m->visitInsn(Opcodes::LCONST_1);
        m->visitIntInsn(Opcodes::BIPUSH, 63);
        m->visitInsn(Opcodes::LSHL);
        m->visitInsn(Opcodes::LXOR);
    });
    // lUShr: (-1L>>>a) ^ (-1L>>>b) ^ (-1L>>>63)
    auto* lUShr = makeLong("lUShr", [&](MethodNode* m) {
        m->visitInsn(Opcodes::LCONST_0);
        m->visitInsn(Opcodes::LCONST_1);
        m->visitInsn(Opcodes::LSUB);   // -1L
        m->visitVarInsn(Opcodes::ILOAD, 0);
        m->visitInsn(Opcodes::LUSHR);
        m->visitInsn(Opcodes::LCONST_0);
        m->visitInsn(Opcodes::LCONST_1);
        m->visitInsn(Opcodes::LSUB);
        m->visitVarInsn(Opcodes::ILOAD, 1);
        m->visitInsn(Opcodes::LUSHR);
        m->visitInsn(Opcodes::LXOR);
        m->visitInsn(Opcodes::LCONST_0);
        m->visitInsn(Opcodes::LCONST_1);
        m->visitInsn(Opcodes::LSUB);
        m->visitIntInsn(Opcodes::BIPUSH, 63);
        m->visitInsn(Opcodes::LUSHR);
        m->visitInsn(Opcodes::LXOR);
    });

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("ShiftOutOfRange");
    cn.superName = ref("java/lang/Object");
    cn.methods.push_back(iShl);
    cn.methods.push_back(iShr);
    cn.methods.push_back(iUShr);
    cn.methods.push_back(lShl);
    cn.methods.push_back(lUShr);
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

/// G12 补充：字符串比较（equals/compareTo/equalsIgnoreCase——跨后端易漂移点，
/// 四后端语义回归集成员）。
/// boolean eqSame()        "abc".equals("abc")        → true
/// int cmpLess()           "abc".compareTo("abd")     → -1
/// int cmpGreater()        "b".compareTo("a")         → 1
/// boolean eqIgnoreCase()  "ABC".equalsIgnoreCase("abc") → true
inline std::vector<ca::u8> genStringCompare(ca::str::Utf8StringArena& arena)
{
    auto make = [&](const char* name, const char* desc, const char* a, const char* b,
                    const char* method, const char* methodDesc) {
        auto* m = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, ref(name),
                                 ref(desc), ref(""), /*exceptions*/ {});
        m->visitCode();
        m->visitLdcInsn(ca::core::Any(std::string(a)));
        m->visitLdcInsn(ca::core::Any(std::string(b)));
        m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/String"), ref(method),
                           ref(methodDesc), false);
        m->visitInsn(Opcodes::IRETURN);
        m->visitMaxs(0, 0);
        m->visitEnd();
        return m;
    };

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("StringCompare");
    cn.superName = ref("java/lang/Object");
    cn.methods.push_back(make("eqSame", "()Z", "abc", "abc", "equals",
                              "(Ljava/lang/Object;)Z"));
    cn.methods.push_back(make("cmpLess", "()I", "abc", "abd", "compareTo",
                              "(Ljava/lang/String;)I"));
    cn.methods.push_back(make("cmpGreater", "()I", "b", "a", "compareTo",
                              "(Ljava/lang/String;)I"));
    cn.methods.push_back(make("eqIgnoreCase", "()Z", "ABC", "abc", "equalsIgnoreCase",
                              "(Ljava/lang/String;)Z"));
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

/// G15：接口调用（java/util/List.size()，INVOKEINTERFACE）。
/// int run(java.util.List l) { return l.size(); }
inline std::vector<ca::u8> genCollectionOps(ca::str::Utf8StringArena& arena)
{
    auto* run = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                               ref("run"), ref("(Ljava/util/List;)I"),
                               ref(""), /*exceptions*/ {});
    run->visitCode();
    run->visitVarInsn(Opcodes::ALOAD, 0);
    run->visitMethodInsn(Opcodes::INVOKEINTERFACE, ref("java/util/List"),
                         ref("size"), ref("()I"), true);
    run->visitInsn(Opcodes::IRETURN);
    run->visitMaxs(0, 0);
    run->visitEnd();

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("CollectionOps");
    cn.superName = ref("java/lang/Object");
    cn.methods.push_back(run);
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

}   // namespace mtest_set::wave3
