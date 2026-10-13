// wave2_samples.hpp - G9/G10/G11 semantic samples.

#pragma once

#include "sample_builder.hpp"

namespace mtest_set::wave2 {

using ca::str::Utf8StringRef;
using ca::str::ZUtf8StringRef;
using jbytecode::core::Label;
using jbytecode::core::Opcodes;

inline auto ref(const char* text)
{
    return ZUtf8StringRef::from_static(text);
}

inline std::vector<ca::u8> genObjectLifecycle(ca::str::Utf8StringArena& arena)
{
    return buildVisitorClass(arena,
                             "ObjectLifecycle",
                             Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                             "java/lang/Object",
                             {},
                             [](auto& cw, auto&) {
                                 cw.visitField(Opcodes::ACC_PRIVATE,
                                               ref("value"), ref("I"), ref(""), nullptr)->visitEnd();
                                 auto* init = cw.visitMethod(Opcodes::ACC_PUBLIC,
                                                             ref("<init>"), ref("(I)V"), ref(""), {});
                                 init->visitCode();
                                 init->visitVarInsn(Opcodes::ALOAD, 0);
                                 init->visitMethodInsn(Opcodes::INVOKESPECIAL,
                                                       ref("java/lang/Object"), ref("<init>"), ref("()V"), false);
                                 init->visitVarInsn(Opcodes::ALOAD, 0);
                                 init->visitVarInsn(Opcodes::ILOAD, 1);
                                 init->visitFieldInsn(Opcodes::PUTFIELD,
                                                      ref("ObjectLifecycle"), ref("value"), ref("I"));
                                 init->visitInsn(Opcodes::RETURN);
                                 init->visitMaxs(2, 2);
                                 init->visitEnd();

                                 auto* value = cw.visitMethod(Opcodes::ACC_PUBLIC,
                                                              ref("value"), ref("()I"), ref(""), {});
                                 value->visitCode();
                                 value->visitVarInsn(Opcodes::ALOAD, 0);
                                 value->visitFieldInsn(Opcodes::GETFIELD,
                                                       ref("ObjectLifecycle"), ref("value"), ref("I"));
                                 value->visitInsn(Opcodes::IRETURN);
                                 value->visitMaxs(1, 1);
                                 value->visitEnd();

                                 auto* run = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                            ref("run"), ref("(I)I"), ref(""), {});
                                 run->visitCode();
                                 run->visitTypeInsn(Opcodes::NEW, ref("ObjectLifecycle"));
                                 run->visitInsn(Opcodes::DUP);
                                 run->visitVarInsn(Opcodes::ILOAD, 0);
                                 run->visitMethodInsn(Opcodes::INVOKESPECIAL,
                                                      ref("ObjectLifecycle"), ref("<init>"), ref("(I)V"), false);
                                 run->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                      ref("ObjectLifecycle"), ref("value"), ref("()I"), false);
                                 run->visitInsn(Opcodes::IRETURN);
                                 run->visitMaxs(3, 1);
                                 run->visitEnd();
                             });
}

inline std::vector<ca::u8> genTypeChecks(ca::str::Utf8StringArena& arena)
{
    return buildSingleStaticMethodClass(
        arena, "TypeChecks", "run", "(Ljava/lang/Object;)I", [](auto* method) {
            auto* notString = new Label();
            method->visitVarInsn(Opcodes::ALOAD, 0);
            method->visitTypeInsn(Opcodes::INSTANCEOF, ref("java/lang/String"));
            method->visitJumpInsn(Opcodes::IFEQ, notString);
            method->visitVarInsn(Opcodes::ALOAD, 0);
            method->visitTypeInsn(Opcodes::CHECKCAST, ref("java/lang/String"));
            method->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                    ref("java/lang/String"), ref("length"), ref("()I"), false);
            method->visitInsn(Opcodes::IRETURN);
            method->visitLabel(notString);
            method->visitInsn(Opcodes::ICONST_M1);
            method->visitInsn(Opcodes::IRETURN);
        });
}

inline std::vector<ca::u8> genExceptionFlow(ca::str::Utf8StringArena& arena)
{
    return buildVisitorClass(arena,
                             "ExceptionFlow",
                             Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                             "java/lang/Object",
                             {},
                             [](auto& cw, auto&) {
                                 auto* run = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                            ref("run"), ref("(I)I"), ref(""), {});
                                 auto* start   = new Label();
                                 auto* normal  = new Label();
                                 auto* end     = new Label();
                                 auto* handler = new Label();
                                 run->visitTryCatchBlock(start, end, handler, ref("java/lang/IllegalArgumentException"));
                                 run->visitCode();
                                 run->visitLabel(start);
                                 run->visitVarInsn(Opcodes::ILOAD, 0);
                                 run->visitJumpInsn(Opcodes::IFNE, normal);
                                 run->visitTypeInsn(Opcodes::NEW, ref("java/lang/IllegalArgumentException"));
                                 run->visitInsn(Opcodes::DUP);
                                 run->visitMethodInsn(Opcodes::INVOKESPECIAL,
                                                      ref("java/lang/IllegalArgumentException"), ref("<init>"), ref("()V"), false);
                                 run->visitInsn(Opcodes::ATHROW);
                                 run->visitLabel(normal);
                                 run->visitIntInsn(Opcodes::BIPUSH, 100);
                                 run->visitVarInsn(Opcodes::ILOAD, 0);
                                 run->visitInsn(Opcodes::IDIV);
                                 run->visitLabel(end);
                                 run->visitInsn(Opcodes::IRETURN);
                                 run->visitLabel(handler);
                                 run->visitInsn(Opcodes::POP);
                                 run->visitIntInsn(Opcodes::BIPUSH, -7);
                                 run->visitInsn(Opcodes::IRETURN);
                                 run->visitMaxs(2, 1);
                                 run->visitEnd();
                             });
}

inline std::vector<ca::u8> genMonitorFlow(ca::str::Utf8StringArena& arena)
{
    return buildVisitorClass(arena,
                             "MonitorFlow",
                             Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                             "java/lang/Object",
                             {},
                             [](auto& cw, auto&) {
                                 auto* run = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                            ref("run"), ref("(Ljava/lang/Object;)I"), ref(""), {});
                                 auto* start   = new Label();
                                 auto* end     = new Label();
                                 auto* handler = new Label();
                                 run->visitTryCatchBlock(start, end, handler, Utf8StringRef());
                                 run->visitCode();
                                 run->visitVarInsn(Opcodes::ALOAD, 0);
                                 run->visitInsn(Opcodes::DUP);
                                 run->visitVarInsn(Opcodes::ASTORE, 1);
                                 run->visitInsn(Opcodes::MONITORENTER);
                                 run->visitLabel(start);
                                 run->visitIntInsn(Opcodes::BIPUSH, 7);
                                 run->visitVarInsn(Opcodes::ISTORE, 2);
                                 run->visitVarInsn(Opcodes::ALOAD, 1);
                                 run->visitInsn(Opcodes::MONITOREXIT);
                                 run->visitVarInsn(Opcodes::ILOAD, 2);
                                 run->visitLabel(end);
                                 run->visitInsn(Opcodes::IRETURN);
                                 run->visitLabel(handler);
                                 run->visitVarInsn(Opcodes::ASTORE, 3);
                                 run->visitVarInsn(Opcodes::ALOAD, 1);
                                 run->visitInsn(Opcodes::MONITOREXIT);
                                 run->visitVarInsn(Opcodes::ALOAD, 3);
                                 run->visitInsn(Opcodes::ATHROW);
                                 run->visitMaxs(2, 4);
                                 run->visitEnd();
                             });
}

inline SampleBundle genReflectionAccess(ca::str::Utf8StringArena& arena)
{
    auto target = buildVisitorClass(arena,
                                    "reflect/ReflectionTarget",
                                    Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                                    "java/lang/Object",
                                    {},
                                    [](auto& cw, auto&) {
                                        cw.visitField(Opcodes::ACC_PUBLIC,
                                                      ref("value"), ref("I"), ref(""), nullptr)->visitEnd();
                                        auto* init = cw.visitMethod(Opcodes::ACC_PUBLIC,
                                                                    ref("<init>"), ref("()V"), ref(""), {});
                                        init->visitCode();
                                        init->visitVarInsn(Opcodes::ALOAD, 0);
                                        init->visitMethodInsn(Opcodes::INVOKESPECIAL,
                                                              ref("java/lang/Object"), ref("<init>"), ref("()V"), false);
                                        init->visitInsn(Opcodes::RETURN);
                                        init->visitMaxs(1, 1);
                                        init->visitEnd();
                                        auto* twice = cw.visitMethod(Opcodes::ACC_PUBLIC,
                                                                     ref("twice"), ref("(I)I"), ref(""), {});
                                        twice->visitCode();
                                        twice->visitVarInsn(Opcodes::ILOAD, 1);
                                        twice->visitInsn(Opcodes::ICONST_2);
                                        twice->visitInsn(Opcodes::IMUL);
                                        twice->visitInsn(Opcodes::IRETURN);
                                        twice->visitMaxs(2, 2);
                                        twice->visitEnd();
                                    });

    auto access = buildVisitorClass(arena,
                                    "reflect/ReflectionAccess",
                                    Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                                    "java/lang/Object",
                                    {},
                                    [](auto& cw, auto&) {
                                        auto* className = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                                         ref("className"), ref("()Ljava/lang/String;"), ref(""), {});
                                        className->visitCode();
                                        className->visitLdcInsn(std::string("reflect.ReflectionTarget"));
                                        className->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                                   ref("java/lang/Class"), ref("forName"),
                                                                   ref("(Ljava/lang/String;)Ljava/lang/Class;"), false);
                                        className->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                                   ref("java/lang/Class"), ref("getName"),
                                                                   ref("()Ljava/lang/String;"), false);
                                        className->visitInsn(Opcodes::ARETURN);
                                        className->visitMaxs(1, 0);
                                        className->visitEnd();

                                        auto* run = cw.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                                   ref("run"), ref("()I"), ref(""), {});
                                        run->visitCode();
                                        run->visitLdcInsn(std::string("reflect.ReflectionTarget"));
                                        run->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                             ref("java/lang/Class"), ref("forName"),
                                                             ref("(Ljava/lang/String;)Ljava/lang/Class;"), false);
                                        run->visitVarInsn(Opcodes::ASTORE, 0);
                                        run->visitVarInsn(Opcodes::ALOAD, 0);
                                        run->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                             ref("java/lang/Class"), ref("newInstance"),
                                                             ref("()Ljava/lang/Object;"), false);
                                        run->visitVarInsn(Opcodes::ASTORE, 1);

                                        run->visitVarInsn(Opcodes::ALOAD, 0);
                                        run->visitLdcInsn(std::string("twice"));
                                        run->visitInsn(Opcodes::ICONST_1);
                                        run->visitTypeInsn(Opcodes::ANEWARRAY, ref("java/lang/Class"));
                                        run->visitInsn(Opcodes::DUP);
                                        run->visitInsn(Opcodes::ICONST_0);
                                        run->visitFieldInsn(Opcodes::GETSTATIC,
                                                            ref("java/lang/Integer"), ref("TYPE"), ref("Ljava/lang/Class;"));
                                        run->visitInsn(Opcodes::AASTORE);
                                        run->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                             ref("java/lang/Class"), ref("getDeclaredMethod"),
                                                             ref("(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;"), false);
                                        run->visitVarInsn(Opcodes::ASTORE, 2);
                                        run->visitVarInsn(Opcodes::ALOAD, 2);
                                        run->visitVarInsn(Opcodes::ALOAD, 1);
                                        run->visitInsn(Opcodes::ICONST_1);
                                        run->visitTypeInsn(Opcodes::ANEWARRAY, ref("java/lang/Object"));
                                        run->visitInsn(Opcodes::DUP);
                                        run->visitInsn(Opcodes::ICONST_0);
                                        run->visitIntInsn(Opcodes::BIPUSH, 6);
                                        run->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                             ref("java/lang/Integer"), ref("valueOf"),
                                                             ref("(I)Ljava/lang/Integer;"), false);
                                        run->visitInsn(Opcodes::AASTORE);
                                        run->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                             ref("java/lang/reflect/Method"), ref("invoke"),
                                                             ref("(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;"), false);
                                        run->visitTypeInsn(Opcodes::CHECKCAST, ref("java/lang/Integer"));
                                        run->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                             ref("java/lang/Integer"), ref("intValue"), ref("()I"), false);
                                        run->visitVarInsn(Opcodes::ISTORE, 3);

                                        run->visitVarInsn(Opcodes::ALOAD, 0);
                                        run->visitLdcInsn(std::string("value"));
                                        run->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                             ref("java/lang/Class"), ref("getDeclaredField"),
                                                             ref("(Ljava/lang/String;)Ljava/lang/reflect/Field;"), false);
                                        run->visitVarInsn(Opcodes::ASTORE, 4);
                                        run->visitVarInsn(Opcodes::ALOAD, 4);
                                        run->visitVarInsn(Opcodes::ALOAD, 1);
                                        run->visitInsn(Opcodes::ICONST_5);
                                        run->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                             ref("java/lang/reflect/Field"), ref("setInt"),
                                                             ref("(Ljava/lang/Object;I)V"), false);
                                        run->visitVarInsn(Opcodes::ILOAD, 3);
                                        run->visitVarInsn(Opcodes::ALOAD, 4);
                                        run->visitVarInsn(Opcodes::ALOAD, 1);
                                        run->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                             ref("java/lang/reflect/Field"), ref("getInt"),
                                                             ref("(Ljava/lang/Object;)I"), false);
                                        run->visitInsn(Opcodes::IADD);
                                        run->visitInsn(Opcodes::IRETURN);
                                        run->visitMaxs(6, 5);
                                        run->visitEnd();
                                    });
    return SampleBundle{{{"reflect/ReflectionTarget.class", std::move(target)},
                         {"reflect/ReflectionAccess.class", std::move(access)}}};
}

inline SampleBundle single(const char* path, std::vector<ca::u8> bytes)
{
    return singleArtifact(path, std::move(bytes));
}

} // namespace mtest_set::wave2
