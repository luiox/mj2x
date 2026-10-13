// static_value_samples.hpp — G16 static final 常量折叠靶样本生成（#960）
//
// 覆盖：生产者直带 ConstantValue 的 static final 字段、<clinit> 常量写入形态
// （blank final + iconst/putstatic）。两者是 static-value-fold 管线
// （StaticFieldDefaultInliner + StaticValueFoldPass）的折叠靶：消费方测试先断言
// 原始 bytes 含 GETSTATIC，过管线后断言 GETSTATIC 折为 ldc 且 JVM 行为不变。

#pragma once

#include "sample_builder.hpp"

namespace mtest_set::static_value {

/// `static final int CV = 42`（ConstantValue 直带）+ `static int readCv()` 经
/// GETSTATIC 读取。
inline std::vector<ca::u8> genCvConstant(ca::str::Utf8StringArena& arena)
{
    return buildVisitorClass(arena, "SvfCvConstant", Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                             "java/lang/Object", {},
                             [](ClassWriter& w, Utf8StringArena&) {
                                 w.visitField(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC |
                                                  Opcodes::ACC_FINAL,
                                              ZUtf8StringRef::from_static("CV"),
                                              ZUtf8StringRef::from_static("I"),
                                              Utf8StringRef(),
                                              ca::core::Any(42))
                                     ->visitEnd();
                                 auto* mv = w.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                          ZUtf8StringRef::from_static("readCv"),
                                                          ZUtf8StringRef::from_static("()I"),
                                                          Utf8StringRef(),
                                                          {});
                                 mv->visitCode();
                                 mv->visitFieldInsn(Opcodes::GETSTATIC,
                                                    ZUtf8StringRef::from_static("SvfCvConstant"),
                                                    ZUtf8StringRef::from_static("CV"),
                                                    ZUtf8StringRef::from_static("I"));
                                 mv->visitInsn(Opcodes::IRETURN);
                                 mv->visitMaxs(1, 0);
                                 mv->visitEnd();
                             });
}

/// `static final int DELAYED`（blank final，无 ConstantValue）+ `<clinit>` 内
/// ICONST_5 + PUTSTATIC + `static int readDelayed()` 经 GETSTATIC 读取。
inline std::vector<ca::u8> genClinitConstant(ca::str::Utf8StringArena& arena)
{
    return buildVisitorClass(arena, "SvfClinitConstant", Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER,
                             "java/lang/Object", {},
                             [](ClassWriter& w, Utf8StringArena&) {
                                 w.visitField(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC |
                                                  Opcodes::ACC_FINAL,
                                              ZUtf8StringRef::from_static("DELAYED"),
                                              ZUtf8StringRef::from_static("I"),
                                              Utf8StringRef(),
                                              ca::core::Any())
                                     ->visitEnd();
                                 auto* clinit =
                                     w.visitMethod(Opcodes::ACC_STATIC,
                                                   ZUtf8StringRef::from_static("<clinit>"),
                                                   ZUtf8StringRef::from_static("()V"),
                                                   Utf8StringRef(),
                                                   {});
                                 clinit->visitCode();
                                 clinit->visitInsn(Opcodes::ICONST_5);
                                 clinit->visitFieldInsn(Opcodes::PUTSTATIC,
                                                        ZUtf8StringRef::from_static("SvfClinitConstant"),
                                                        ZUtf8StringRef::from_static("DELAYED"),
                                                        ZUtf8StringRef::from_static("I"));
                                 clinit->visitInsn(Opcodes::RETURN);
                                 clinit->visitMaxs(1, 0);
                                 clinit->visitEnd();
                                 auto* mv = w.visitMethod(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                                          ZUtf8StringRef::from_static("readDelayed"),
                                                          ZUtf8StringRef::from_static("()I"),
                                                          Utf8StringRef(),
                                                          {});
                                 mv->visitCode();
                                 mv->visitFieldInsn(Opcodes::GETSTATIC,
                                                    ZUtf8StringRef::from_static("SvfClinitConstant"),
                                                    ZUtf8StringRef::from_static("DELAYED"),
                                                    ZUtf8StringRef::from_static("I"));
                                 mv->visitInsn(Opcodes::IRETURN);
                                 mv->visitMaxs(1, 0);
                                 mv->visitEnd();
                             });
}

}   // namespace mtest_set::static_value
