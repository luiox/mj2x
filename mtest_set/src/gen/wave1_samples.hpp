// wave1_samples.hpp - G3/G6/G7 semantic samples.

#pragma once

#include "sample_builder.hpp"

namespace mtest_set::wave1 {

using ca::str::ZUtf8StringRef;
using jbytecode::core::Opcodes;
using jbytecode::tree::MethodNode;

inline std::vector<ca::u8> genIntegralNarrowing(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(
        arena, "TypeIntegralNarrowing", "run", "(I)I", [](MethodNode* mv) {
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::I2B);
            mv->visitVarInsn(Opcodes::ISTORE, 1);
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::I2S);
            mv->visitVarInsn(Opcodes::ISTORE, 2);
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::I2C);
            mv->visitVarInsn(Opcodes::ISTORE, 3);
            mv->visitVarInsn(Opcodes::ILOAD, 1);
            mv->visitVarInsn(Opcodes::ILOAD, 2);
            mv->visitInsn(Opcodes::IADD);
            mv->visitVarInsn(Opcodes::ILOAD, 3);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

inline std::vector<ca::u8> genNumericWidening(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(
        arena, "TypeNumericWidening", "run", "(I)D", [](MethodNode* mv) {
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::I2L);
            mv->visitVarInsn(Opcodes::LSTORE, 2);
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::I2F);
            mv->visitVarInsn(Opcodes::FSTORE, 4);
            mv->visitVarInsn(Opcodes::ILOAD, 0);
            mv->visitInsn(Opcodes::I2D);
            mv->visitVarInsn(Opcodes::DSTORE, 5);
            mv->visitVarInsn(Opcodes::LLOAD, 2);
            mv->visitInsn(Opcodes::L2F);
            mv->visitVarInsn(Opcodes::FSTORE, 7);
            mv->visitVarInsn(Opcodes::LLOAD, 2);
            mv->visitInsn(Opcodes::L2D);
            mv->visitVarInsn(Opcodes::DSTORE, 8);
            mv->visitVarInsn(Opcodes::FLOAD, 4);
            mv->visitInsn(Opcodes::F2D);
            mv->visitVarInsn(Opcodes::DLOAD, 5);
            mv->visitInsn(Opcodes::DADD);
            mv->visitVarInsn(Opcodes::DLOAD, 8);
            mv->visitInsn(Opcodes::DADD);
            mv->visitVarInsn(Opcodes::FLOAD, 7);
            mv->visitInsn(Opcodes::F2D);
            mv->visitInsn(Opcodes::DADD);
            mv->visitInsn(Opcodes::DRETURN);
        });
}

inline std::vector<ca::u8> genNumericNarrowing(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(
        arena, "TypeNumericNarrowing", "run", "(D)I", [](MethodNode* mv) {
            mv->visitVarInsn(Opcodes::DLOAD, 0);
            mv->visitInsn(Opcodes::D2I);
            mv->visitVarInsn(Opcodes::ISTORE, 2);
            mv->visitVarInsn(Opcodes::DLOAD, 0);
            mv->visitInsn(Opcodes::D2L);
            mv->visitVarInsn(Opcodes::LSTORE, 3);
            mv->visitVarInsn(Opcodes::DLOAD, 0);
            mv->visitInsn(Opcodes::D2F);
            mv->visitVarInsn(Opcodes::FSTORE, 5);
            mv->visitVarInsn(Opcodes::FLOAD, 5);
            mv->visitInsn(Opcodes::F2I);
            mv->visitVarInsn(Opcodes::ISTORE, 6);
            mv->visitVarInsn(Opcodes::FLOAD, 5);
            mv->visitInsn(Opcodes::F2L);
            mv->visitVarInsn(Opcodes::LSTORE, 7);
            mv->visitVarInsn(Opcodes::LLOAD, 3);
            mv->visitInsn(Opcodes::L2I);
            mv->visitVarInsn(Opcodes::ISTORE, 9);
            mv->visitVarInsn(Opcodes::ILOAD, 2);
            mv->visitVarInsn(Opcodes::ILOAD, 6);
            mv->visitInsn(Opcodes::IADD);
            mv->visitVarInsn(Opcodes::ILOAD, 9);
            mv->visitInsn(Opcodes::IADD);
            mv->visitVarInsn(Opcodes::LLOAD, 7);
            mv->visitInsn(Opcodes::L2I);
            mv->visitInsn(Opcodes::IADD);
            mv->visitInsn(Opcodes::IRETURN);
        });
}

inline std::vector<ca::u8> genStaticSpecial(ca::str::Utf8StringArena& arena)
{
    return buildVisitorClass(arena,
                             "CallsStaticSpecial",
                             Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                             "java/lang/Object",
                             {},
                             [](auto& cw, auto& a) {
                                 auto ref = [](const char* text) {
                                     return ZUtf8StringRef::from_static(text);
                                 };
                                 auto* init = cw.visitMethod(Opcodes::ACC_PUBLIC,
                                                              ref("<init>"),
                                                              ref("()V"),
                                                              ref(""),
                                                              {});
                                 init->visitCode();
                                 init->visitVarInsn(Opcodes::ALOAD, 0);
                                 init->visitMethodInsn(Opcodes::INVOKESPECIAL,
                                                       ref("java/lang/Object"),
                                                       ref("<init>"),
                                                       ref("()V"),
                                                       false);
                                 init->visitInsn(Opcodes::RETURN);
                                 init->visitMaxs(1, 1);
                                 init->visitEnd();

                                 auto* helper = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                               ref("helper"), ref("(I)I"), ref(""), {});
                                 helper->visitCode();
                                 helper->visitVarInsn(Opcodes::ILOAD, 0);
                                 helper->visitInsn(Opcodes::ICONST_2);
                                 helper->visitInsn(Opcodes::IADD);
                                 helper->visitInsn(Opcodes::IRETURN);
                                 helper->visitMaxs(2, 1);
                                 helper->visitEnd();

                                 auto* virtualMethod = cw.visitMethod(Opcodes::ACC_PUBLIC,
                                                                      ref("virtualValue"), ref("(I)I"), ref(""), {});
                                 virtualMethod->visitCode();
                                 virtualMethod->visitVarInsn(Opcodes::ILOAD, 1);
                                 virtualMethod->visitInsn(Opcodes::ICONST_3);
                                 virtualMethod->visitInsn(Opcodes::IADD);
                                 virtualMethod->visitInsn(Opcodes::IRETURN);
                                 virtualMethod->visitMaxs(2, 2);
                                 virtualMethod->visitEnd();

                                 auto* special = cw.visitMethod(Opcodes::ACC_PRIVATE,
                                                                ref("specialValue"), ref("(I)I"), ref(""), {});
                                 special->visitCode();
                                 special->visitVarInsn(Opcodes::ILOAD, 1);
                                 special->visitInsn(Opcodes::ICONST_4);
                                 special->visitInsn(Opcodes::IADD);
                                 special->visitInsn(Opcodes::IRETURN);
                                 special->visitMaxs(2, 2);
                                 special->visitEnd();

                                 auto* run = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                            ref("run"), ref("(I)I"), ref(""), {});
                                 run->visitCode();
                                 run->visitVarInsn(Opcodes::ILOAD, 0);
                                 run->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                      ref("CallsStaticSpecial"), ref("helper"), ref("(I)I"), false);
                                 run->visitTypeInsn(Opcodes::NEW, ref("CallsStaticSpecial"));
                                 run->visitInsn(Opcodes::DUP);
                                 run->visitMethodInsn(Opcodes::INVOKESPECIAL,
                                                      ref("CallsStaticSpecial"), ref("<init>"), ref("()V"), false);
                                 run->visitVarInsn(Opcodes::ILOAD, 0);
                                 run->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                      ref("CallsStaticSpecial"), ref("virtualValue"), ref("(I)I"), false);
                                 run->visitInsn(Opcodes::IADD);
                                 run->visitTypeInsn(Opcodes::NEW, ref("CallsStaticSpecial"));
                                 run->visitInsn(Opcodes::DUP);
                                 run->visitMethodInsn(Opcodes::INVOKESPECIAL,
                                                      ref("CallsStaticSpecial"), ref("<init>"), ref("()V"), false);
                                 run->visitVarInsn(Opcodes::ILOAD, 0);
                                 run->visitMethodInsn(Opcodes::INVOKESPECIAL,
                                                      ref("CallsStaticSpecial"), ref("specialValue"), ref("(I)I"), false);
                                 run->visitInsn(Opcodes::IADD);
                                 run->visitInsn(Opcodes::IRETURN);
                                 run->visitMaxs(4, 1);
                                 run->visitEnd();
                                 (void)a;
                             });
}

inline SampleBundle genVirtualInterface(ca::str::Utf8StringArena& arena)
{
    auto ref = [](const char* text) { return ZUtf8StringRef::from_static(text); };
    auto interfaceBytes = buildVisitorClass(arena,
                                            "calls/ICall",
                                            Opcodes::ACC_PUBLIC | Opcodes::ACC_INTERFACE | Opcodes::ACC_ABSTRACT,
                                            "java/lang/Object",
                                            {},
                                            [&](auto& cw, auto&) {
                                                auto* method = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_ABSTRACT,
                                                                               ref("apply"), ref("(I)I"), ref(""), {});
                                                method->visitEnd();
                                            },
                                            0);
    auto implBytes = buildVisitorClass(arena,
                                       "calls/CallImpl",
                                       Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                                       "java/lang/Object",
                                       {"calls/ICall"},
                                       [&](auto& cw, auto&) {
                                           auto* init = cw.visitMethod(Opcodes::ACC_PUBLIC, ref("<init>"), ref("()V"), ref(""), {});
                                           init->visitCode();
                                           init->visitVarInsn(Opcodes::ALOAD, 0);
                                           init->visitMethodInsn(Opcodes::INVOKESPECIAL, ref("java/lang/Object"), ref("<init>"), ref("()V"), false);
                                           init->visitInsn(Opcodes::RETURN);
                                           init->visitMaxs(1, 1);
                                           init->visitEnd();
                                           auto* apply = cw.visitMethod(Opcodes::ACC_PUBLIC, ref("apply"), ref("(I)I"), ref(""), {});
                                           apply->visitCode();
                                           apply->visitVarInsn(Opcodes::ILOAD, 1);
                                           apply->visitInsn(Opcodes::ICONST_2);
                                           apply->visitInsn(Opcodes::IMUL);
                                           apply->visitInsn(Opcodes::IRETURN);
                                           apply->visitMaxs(2, 2);
                                           apply->visitEnd();
                                       });
    auto callerBytes = buildVisitorClass(arena,
                                         "calls/CallInterface",
                                         Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                                         "java/lang/Object",
                                         {},
                                         [&](auto& cw, auto&) {
                                             auto* run = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                                        ref("run"), ref("(I)I"), ref(""), {});
                                             run->visitCode();
                                             run->visitTypeInsn(Opcodes::NEW, ref("calls/CallImpl"));
                                             run->visitInsn(Opcodes::DUP);
                                             run->visitMethodInsn(Opcodes::INVOKESPECIAL, ref("calls/CallImpl"), ref("<init>"), ref("()V"), false);
                                             run->visitVarInsn(Opcodes::ILOAD, 0);
                                             run->visitMethodInsn(Opcodes::INVOKEINTERFACE, ref("calls/ICall"), ref("apply"), ref("(I)I"), true);
                                             run->visitInsn(Opcodes::IRETURN);
                                             run->visitMaxs(3, 1);
                                             run->visitEnd();
                                         });
    return SampleBundle{{{"calls/ICall.class", std::move(interfaceBytes)},
                         {"calls/CallImpl.class", std::move(implBytes)},
                         {"calls/CallInterface.class", std::move(callerBytes)}}};
}

inline std::vector<ca::u8> genRecursion(ca::str::Utf8StringArena& arena)
{
    return buildVisitorClass(arena,
                             "CallsRecursion",
                             Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                             "java/lang/Object",
                             {},
                             [](auto& cw, auto&) {
                                 auto ref = [](const char* text) { return ZUtf8StringRef::from_static(text); };
                                 auto* sum = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                            ref("sum"), ref("(I)I"), ref(""), {});
                                 sum->visitCode();
                                 auto* recurse = new jbytecode::core::Label();
                                 sum->visitVarInsn(Opcodes::ILOAD, 0);
                                 sum->visitJumpInsn(Opcodes::IFLE, recurse);
                                 sum->visitVarInsn(Opcodes::ILOAD, 0);
                                 sum->visitVarInsn(Opcodes::ILOAD, 0);
                                 sum->visitInsn(Opcodes::ICONST_1);
                                 sum->visitInsn(Opcodes::ISUB);
                                 sum->visitMethodInsn(Opcodes::INVOKESTATIC, ref("CallsRecursion"), ref("sum"), ref("(I)I"), false);
                                 sum->visitInsn(Opcodes::IADD);
                                 sum->visitInsn(Opcodes::IRETURN);
                                 sum->visitLabel(recurse);
                                 sum->visitInsn(Opcodes::ICONST_0);
                                 sum->visitInsn(Opcodes::IRETURN);
                                 sum->visitMaxs(3, 1);
                                 sum->visitEnd();
                             });
}

inline std::vector<ca::u8> genStaticFields(ca::str::Utf8StringArena& arena)
{
    return buildVisitorClass(arena,
                             "FieldsStatic",
                             Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                             "java/lang/Object",
                             {},
                             [](auto& cw, auto&) {
                                 auto ref = [](const char* text) { return ZUtf8StringRef::from_static(text); };
                                 cw.visitField(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                               ref("counter"), ref("I"), ref(""), nullptr)->visitEnd();
                                 auto* run = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                            ref("run"), ref("(I)I"), ref(""), {});
                                 run->visitCode();
                                 run->visitVarInsn(Opcodes::ILOAD, 0);
                                 run->visitFieldInsn(Opcodes::PUTSTATIC, ref("FieldsStatic"), ref("counter"), ref("I"));
                                 run->visitFieldInsn(Opcodes::GETSTATIC, ref("FieldsStatic"), ref("counter"), ref("I"));
                                 run->visitInsn(Opcodes::ICONST_3);
                                 run->visitInsn(Opcodes::IADD);
                                 run->visitInsn(Opcodes::DUP);
                                 run->visitFieldInsn(Opcodes::PUTSTATIC, ref("FieldsStatic"), ref("counter"), ref("I"));
                                 run->visitFieldInsn(Opcodes::GETSTATIC, ref("FieldsStatic"), ref("counter"), ref("I"));
                                 run->visitInsn(Opcodes::IRETURN);
                                 run->visitMaxs(2, 1);
                                 run->visitEnd();
                             });
}

inline std::vector<ca::u8> genInstanceFields(ca::str::Utf8StringArena& arena)
{
    return buildVisitorClass(arena,
                             "FieldsInstance",
                             Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                             "java/lang/Object",
                             {},
                             [](auto& cw, auto&) {
                                 auto ref = [](const char* text) { return ZUtf8StringRef::from_static(text); };
                                 cw.visitField(Opcodes::ACC_PUBLIC, ref("value"), ref("I"), ref(""), nullptr)->visitEnd();
                                 auto* init = cw.visitMethod(Opcodes::ACC_PUBLIC, ref("<init>"), ref("()V"), ref(""), {});
                                 init->visitCode();
                                 init->visitVarInsn(Opcodes::ALOAD, 0);
                                 init->visitMethodInsn(Opcodes::INVOKESPECIAL, ref("java/lang/Object"), ref("<init>"), ref("()V"), false);
                                 init->visitInsn(Opcodes::RETURN);
                                 init->visitMaxs(1, 1);
                                 init->visitEnd();
                                 auto* run = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                            ref("run"), ref("(I)I"), ref(""), {});
                                 run->visitCode();
                                 run->visitTypeInsn(Opcodes::NEW, ref("FieldsInstance"));
                                 run->visitInsn(Opcodes::DUP);
                                 run->visitMethodInsn(Opcodes::INVOKESPECIAL, ref("FieldsInstance"), ref("<init>"), ref("()V"), false);
                                 run->visitInsn(Opcodes::DUP);
                                 run->visitVarInsn(Opcodes::ILOAD, 0);
                                 run->visitInsn(Opcodes::ICONST_1);
                                 run->visitInsn(Opcodes::IADD);
                                 run->visitFieldInsn(Opcodes::PUTFIELD, ref("FieldsInstance"), ref("value"), ref("I"));
                                 run->visitFieldInsn(Opcodes::GETFIELD, ref("FieldsInstance"), ref("value"), ref("I"));
                                 run->visitInsn(Opcodes::IRETURN);
                                 run->visitMaxs(4, 1);
                                 run->visitEnd();
                             });
}

inline SampleBundle single(const char* path, std::vector<ca::u8> bytes)
{
    return singleArtifact(path, std::move(bytes));
}

} // namespace mtest_set::wave1
