// wave4_samples.hpp - G17 jvmtp 1.7.1 正确性语义回归样本（#1117）。
//
// 反推自 jvmtp 1.7.1 changelog 的 5 项正确性修复语义点（调研 ledger：
// morpher#760 issuecomment-5969219464），按「竞品 bug 换回归样本」惯例自建
// 合法语义样本（零竞品产物拷贝），mj2x 矩阵列逐语义点按 JVM 语义对拍：
//   G17.1 MathNanZero          Math.max/min float/double 的 NaN 位型与 ±0.0 符号
//                             （修复③同款缺陷面；曾暴露 mj2x CRT 裸三目
//                              返回错误符号零，已修）
//   G17.2 StringIntern         ldc 常量字符串 intern 恒等（修复④同款；
//                              曾暴露 StackC 逐次 NewString / RegC 缓存未
//                              intern 的恒等破坏，已修）
//   G17.3 ArrayStoreCheck      协变数组存储 ArrayStoreException（修复②）
//   G17.4 SiblingMethods       多方法兄弟方法完整性（修复①：帧计算按方法
//                              隔离，-Xverify:all 加载期校验兜底）
//   G17.5 AnnotationProbe      注解可见性（修复⑤：注解样本不崩 + 反射可读）
// 全部为干净字节码（生成器不产混淆样本），行为契约进 core_fixture_registry。

#pragma once

#include <limits>
#include <string>

#include "sample_builder.hpp"

namespace mtest_set::wave4 {

using ca::str::ZUtf8StringRef;
using jbytecode::core::Label;
using jbytecode::core::Opcodes;

inline auto ref(const char* text)
{
    return ZUtf8StringRef::from_static(text);
}

/// LDC 常量字符串装箱：Utf8StringRef 视图（ClassWriter 同步 intern 进符号表，
/// 视图寿命无关），走 method_writer 的 Utf8StringRef 分支，不落 std::string
/// 兼容兜底（debug 构建有护栏告警）。
inline ca::str::Utf8StringRef textRef(const char* text)
{
    return ca::str::Utf8StringRef::from_cstr(text);
}

/// boolean 方法收尾体：栈上两个引用 IF_ACMPEQ → true，否则 false
///（不含 visitMaxs/visitEnd——由 makeMethod 统一终结）。
inline void emitRefIdentityTail(MethodNode* m, Label* isTrue)
{
    m->visitJumpInsn(Opcodes::IF_ACMPEQ, isTrue);
    m->visitInsn(Opcodes::ICONST_0);
    m->visitInsn(Opcodes::IRETURN);
    m->visitLabel(isTrue);
    m->visitInsn(Opcodes::ICONST_1);
    m->visitInsn(Opcodes::IRETURN);
}

/// 完整方法生命周期：visitCode → body → visitMaxs(0,0)（COMPUTE_FRAMES 填充）
/// → visitEnd。
inline MethodNode* makeMethod(const char* name, const char* desc, int access,
                              const std::function<void(MethodNode*)>& body)
{
    auto* m = new MethodNode(access, ref(name), ref(desc), ref(""), /*exceptions*/ {});
    m->visitCode();
    body(m);
    m->visitMaxs(0, 0);
    m->visitEnd();
    return m;
}

// ---------------------------------------------------------------------------
// G17.1：Math.max/min NaN 位型与 ±0.0 符号矩阵（float/double）+ Math.round
// 边界（#1144）。手写字节码保证 INVOKESTATIC Math.* 不被常量折叠吃掉；±0.0 用
// toHexString 区分符号，NaN 用 isNaN 与 doubleToRawLongBits 断言位型；round
// 边界取 HotSpot 实测发散形状——floor(x+0.5) 公式在 0.49999997f 给 1（正确
// 0）、在 8388609.0f 给 8388610（正确 8388609），锁定 OpenJDK 位运算口径。
// ---------------------------------------------------------------------------
inline std::vector<ca::u8> genMathNanZero(ca::str::Utf8StringArena& arena)
{
    const double dnan = std::numeric_limits<double>::quiet_NaN();
    const float  fnan = std::numeric_limits<float>::quiet_NaN();

    std::vector<MethodNode*> methods;

    // NaN 判定面：max/min 双侧 NaN 都必须传染。
    auto emitDoubleNan = [&](bool isMax, bool nanFirst) {
        return [isMax, nanFirst, dnan](MethodNode* m) {
            m->visitLdcInsn(ca::core::Any(nanFirst ? dnan : 1.0));
            m->visitLdcInsn(ca::core::Any(nanFirst ? 1.0 : dnan));
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Math"),
                               ref(isMax ? "max" : "min"), ref("(DD)D"), false);
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Double"), ref("isNaN"),
                               ref("(D)Z"), false);
            m->visitInsn(Opcodes::IRETURN);
        };
    };
    auto emitFloatNan = [&](bool isMax, bool nanFirst) {
        return [isMax, nanFirst, fnan](MethodNode* m) {
            m->visitLdcInsn(ca::core::Any(nanFirst ? fnan : 1.0f));
            m->visitLdcInsn(ca::core::Any(nanFirst ? 1.0f : fnan));
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Math"),
                               ref(isMax ? "max" : "min"), ref("(FF)F"), false);
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Float"), ref("isNaN"),
                               ref("(F)Z"), false);
            m->visitInsn(Opcodes::IRETURN);
        };
    };
    methods.push_back(makeMethod("maxDNanL", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitDoubleNan(true, true)));
    methods.push_back(makeMethod("maxDNanR", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitDoubleNan(true, false)));
    methods.push_back(makeMethod("minDNanL", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitDoubleNan(false, true)));
    methods.push_back(makeMethod("minDNanR", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitDoubleNan(false, false)));
    methods.push_back(makeMethod("maxFNanL", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitFloatNan(true, true)));
    methods.push_back(makeMethod("maxFNanR", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitFloatNan(true, false)));
    methods.push_back(makeMethod("minFNanL", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitFloatNan(false, true)));
    methods.push_back(makeMethod("minFNanR", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitFloatNan(false, false)));

    // ±0.0 符号面：IEEE 比较视 ±0.0 相等，结果符号必须按 JVM 语义取。
    auto emitZeroSign = [&](bool isFloat, bool isMax, bool posFirst) {
        return [isFloat, isMax, posFirst](MethodNode* m) {
            const char* mathOwner  = "java/lang/Math";
            const char* hexOwner   = isFloat ? "java/lang/Float" : "java/lang/Double";
            const char* mathDesc   = isFloat ? "(FF)F" : "(DD)D";
            const char* mathMethod = isMax ? "max" : "min";
            if (isFloat) {
                m->visitLdcInsn(ca::core::Any(posFirst ? 0.0f : -0.0f));
                m->visitLdcInsn(ca::core::Any(posFirst ? -0.0f : 0.0f));
            }
            else {
                m->visitLdcInsn(ca::core::Any(posFirst ? 0.0 : -0.0));
                m->visitLdcInsn(ca::core::Any(posFirst ? -0.0 : 0.0));
            }
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref(mathOwner), ref(mathMethod),
                               ref(mathDesc), false);
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref(hexOwner), ref("toHexString"),
                               ref(isFloat ? "(F)Ljava/lang/String;" : "(D)Ljava/lang/String;"),
                               false);
            m->visitInsn(Opcodes::ARETURN);
        };
    };
    methods.push_back(makeMethod("maxDZeroPN", "()Ljava/lang/String;",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitZeroSign(false, true, true)));
    methods.push_back(makeMethod("maxDZeroNP", "()Ljava/lang/String;",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitZeroSign(false, true, false)));
    methods.push_back(makeMethod("minDZeroPN", "()Ljava/lang/String;",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitZeroSign(false, false, true)));
    methods.push_back(makeMethod("minDZeroNP", "()Ljava/lang/String;",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitZeroSign(false, false, false)));
    methods.push_back(makeMethod("maxFZeroPN", "()Ljava/lang/String;",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitZeroSign(true, true, true)));
    methods.push_back(makeMethod("maxFZeroNP", "()Ljava/lang/String;",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitZeroSign(true, true, false)));
    methods.push_back(makeMethod("minFZeroPN", "()Ljava/lang/String;",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitZeroSign(true, false, true)));
    methods.push_back(makeMethod("minFZeroNP", "()Ljava/lang/String;",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 emitZeroSign(true, false, false)));

    // NaN 位型保留面：max(NaN, x) 必须返回操作数本身（raw bits 相等）。
    {
        auto* isTrue = new Label();
        auto* m = makeMethod("maxDNanBits", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                             [dnan, isTrue](MethodNode* m) {
                                 m->visitLdcInsn(ca::core::Any(dnan));
                                 m->visitLdcInsn(ca::core::Any(1.0));
                                 m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Math"),
                                                    ref("max"), ref("(DD)D"), false);
                                 m->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                    ref("java/lang/Double"),
                                                    ref("doubleToRawLongBits"), ref("(D)J"),
                                                    false);
                                 m->visitLdcInsn(ca::core::Any(dnan));
                                 m->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                    ref("java/lang/Double"),
                                                    ref("doubleToRawLongBits"), ref("(D)J"),
                                                    false);
                                 m->visitInsn(Opcodes::LCMP);
                                 m->visitJumpInsn(Opcodes::IFEQ, isTrue);
                                 m->visitInsn(Opcodes::ICONST_0);
                                 m->visitInsn(Opcodes::IRETURN);
                                 m->visitLabel(isTrue);
                                 m->visitInsn(Opcodes::ICONST_1);
                                 m->visitInsn(Opcodes::IRETURN);
                             });
        methods.push_back(m);
    }

    // int/long 回归面：整型 max/min 本就正确，锁不回退。
    methods.push_back(makeMethod("maxI", "()I", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 [](MethodNode* m) {
                                     m->visitInsn(Opcodes::ICONST_3);
                                     m->visitIntInsn(Opcodes::BIPUSH, 7);
                                     m->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                        ref("java/lang/Math"), ref("max"),
                                                        ref("(II)I"), false);
                                     m->visitInsn(Opcodes::IRETURN);
                                 }));
    methods.push_back(makeMethod("minI", "()I", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 [](MethodNode* m) {
                                     m->visitIntInsn(Opcodes::BIPUSH, -2);
                                     m->visitIntInsn(Opcodes::BIPUSH, -9);
                                     m->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                        ref("java/lang/Math"), ref("min"),
                                                        ref("(II)I"), false);
                                     m->visitInsn(Opcodes::IRETURN);
                                 }));
    methods.push_back(makeMethod("maxL", "()J", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 [](MethodNode* m) {
                                     m->visitLdcInsn(
                                         ca::core::Any(static_cast<ca::i64>(1) << 40));
                                     m->visitLdcInsn(ca::core::Any(static_cast<ca::i64>(-1)));
                                     m->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                        ref("java/lang/Math"), ref("max"),
                                                        ref("(JJ)J"), false);
                                     m->visitInsn(Opcodes::LRETURN);
                                 }));
    methods.push_back(makeMethod("minL", "()J", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 [](MethodNode* m) {
                                     m->visitLdcInsn(
                                         ca::core::Any(static_cast<ca::i64>(1) << 40));
                                     m->visitLdcInsn(ca::core::Any(static_cast<ca::i64>(-1)));
                                     m->visitMethodInsn(Opcodes::INVOKESTATIC,
                                                        ref("java/lang/Math"), ref("min"),
                                                        ref("(JJ)J"), false);
                                     m->visitInsn(Opcodes::LRETURN);
                                 }));

    // round 边界面（#1144）：floor(x+0.5) 公式的两个经典发散形状 + D 侧同款，
    // 期望值 = HotSpot 17.0.20.1 实测（Math.round 直调 CRT 口径对拍锁）。
    methods.push_back(makeMethod(
        "roundFHalfBelow", "()I", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        [](MethodNode* m) {
            m->visitLdcInsn(ca::core::Any(0.49999997f));   // 最大的小于 0.5 的 float
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Math"), ref("round"),
                               ref("(F)I"), false);
            m->visitInsn(Opcodes::IRETURN);
        }));
    methods.push_back(makeMethod(
        "roundFOddBig", "()I", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        [](MethodNode* m) {
            m->visitLdcInsn(ca::core::Any(8388609.0f));    // [2^23,2^24) 奇整数
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Math"), ref("round"),
                               ref("(F)I"), false);
            m->visitInsn(Opcodes::IRETURN);
        }));
    methods.push_back(makeMethod(
        "roundFNegOddBig", "()I", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        [](MethodNode* m) {
            m->visitLdcInsn(ca::core::Any(-8388609.0f));
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Math"), ref("round"),
                               ref("(F)I"), false);
            m->visitInsn(Opcodes::IRETURN);
        }));
    methods.push_back(makeMethod(
        "roundDHalfBelow", "()J", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        [](MethodNode* m) {
            m->visitLdcInsn(ca::core::Any(0.49999999999999994));  // 最大的小于 0.5 的 double
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Math"), ref("round"),
                               ref("(D)J"), false);
            m->visitInsn(Opcodes::LRETURN);
        }));
    methods.push_back(makeMethod(
        "roundDOddBig", "()J", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        [](MethodNode* m) {
            m->visitLdcInsn(ca::core::Any(4503599627370497.0));   // [2^52,2^53) 奇整数
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Math"), ref("round"),
                               ref("(D)J"), false);
            m->visitInsn(Opcodes::LRETURN);
        }));

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("MathNanZero");
    cn.superName = ref("java/lang/Object");
    cn.methods   = std::move(methods);
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

// ---------------------------------------------------------------------------
// G17.2：字符串 intern 恒等（修复④）。四点：
//   ldcSame         同方法两次 ldc 同一常量 → 同一实例
//   internVsLdc     String.intern() 结果 == ldc 常量
//   newStringIntern new String(...).intern() == ldc 常量
//   concatIntern    运行期拼接结果 intern == 同内容 ldc 常量
// ---------------------------------------------------------------------------
inline std::vector<ca::u8> genStringIntern(ca::str::Utf8StringArena& arena)
{
    std::vector<MethodNode*> methods;

    // 两个 ldc 常量 + IF_ACMPEQ。
    methods.push_back(makeMethod(
        "ldcSame", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
            m->visitLdcInsn(ca::core::Any(textRef("intern-probe")));
            m->visitVarInsn(Opcodes::ASTORE, 0);
            m->visitLdcInsn(ca::core::Any(textRef("intern-probe")));
            m->visitVarInsn(Opcodes::ASTORE, 1);
            m->visitVarInsn(Opcodes::ALOAD, 0);
            m->visitVarInsn(Opcodes::ALOAD, 1);
            emitRefIdentityTail(m, new Label());
        }));
    // intern() 结果与 ldc 常量恒等。
    methods.push_back(makeMethod(
        "internVsLdc", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
            m->visitLdcInsn(ca::core::Any(textRef("intern-x")));
            m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/String"), ref("intern"),
                               ref("()Ljava/lang/String;"), false);
            m->visitVarInsn(Opcodes::ASTORE, 0);
            m->visitLdcInsn(ca::core::Any(textRef("intern-x")));
            m->visitVarInsn(Opcodes::ASTORE, 1);
            m->visitVarInsn(Opcodes::ALOAD, 0);
            m->visitVarInsn(Opcodes::ALOAD, 1);
            emitRefIdentityTail(m, new Label());
        }));
    // new String(...).intern() 与 ldc 常量恒等。
    methods.push_back(makeMethod(
        "newStringIntern", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
            m->visitTypeInsn(Opcodes::NEW, ref("java/lang/String"));
            m->visitInsn(Opcodes::DUP);
            m->visitLdcInsn(ca::core::Any(textRef("intern-x")));
            m->visitMethodInsn(Opcodes::INVOKESPECIAL, ref("java/lang/String"), ref("<init>"),
                               ref("(Ljava/lang/String;)V"), false);
            m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/String"), ref("intern"),
                               ref("()Ljava/lang/String;"), false);
            m->visitVarInsn(Opcodes::ASTORE, 0);
            m->visitLdcInsn(ca::core::Any(textRef("intern-x")));
            m->visitVarInsn(Opcodes::ASTORE, 1);
            m->visitVarInsn(Opcodes::ALOAD, 0);
            m->visitVarInsn(Opcodes::ALOAD, 1);
            emitRefIdentityTail(m, new Label());
        }));
    // 运行期拼接（StringBuilder）→ intern 与 ldc 常量恒等。
    methods.push_back(makeMethod(
        "concatIntern", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
            m->visitTypeInsn(Opcodes::NEW, ref("java/lang/StringBuilder"));
            m->visitInsn(Opcodes::DUP);
            m->visitMethodInsn(Opcodes::INVOKESPECIAL, ref("java/lang/StringBuilder"),
                               ref("<init>"), ref("()V"), false);
            m->visitLdcInsn(ca::core::Any(textRef("int")));
            m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/StringBuilder"),
                               ref("append"),
                               ref("(Ljava/lang/String;)Ljava/lang/StringBuilder;"), false);
            m->visitLdcInsn(ca::core::Any(textRef("ern-x")));
            m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/StringBuilder"),
                               ref("append"),
                               ref("(Ljava/lang/String;)Ljava/lang/StringBuilder;"), false);
            m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/StringBuilder"),
                               ref("toString"), ref("()Ljava/lang/String;"), false);
            m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/String"), ref("intern"),
                               ref("()Ljava/lang/String;"), false);
            m->visitVarInsn(Opcodes::ASTORE, 0);
            m->visitLdcInsn(ca::core::Any(textRef("intern-x")));
            m->visitVarInsn(Opcodes::ASTORE, 1);
            m->visitVarInsn(Opcodes::ALOAD, 0);
            m->visitVarInsn(Opcodes::ALOAD, 1);
            emitRefIdentityTail(m, new Label());
        }));

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("StringIntern");
    cn.superName = ref("java/lang/Object");
    cn.methods   = std::move(methods);
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

// ---------------------------------------------------------------------------
// G17.3：协变数组存储 ArrayStoreException（修复②）。非法存储必须抛 ASE，
// 合法协变存储与 primitive 存储不受影响，异常类型名精确可读。
// ---------------------------------------------------------------------------
inline std::vector<ca::u8> genArrayStoreCheck(ca::str::Utf8StringArena& arena)
{
    std::vector<MethodNode*> methods;

    // Object[] a = new String[1]; try { a[0] = Integer.valueOf(5); return false; }
    // catch (ArrayStoreException e) { return true; }
    methods.push_back(makeMethod(
        "covariantStringAstoreThrows", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        [](MethodNode* m) {
            auto* start   = new Label();
            auto* end     = new Label();
            auto* handler = new Label();
            m->visitTryCatchBlock(start, end, handler, ref("java/lang/ArrayStoreException"));
            m->visitInsn(Opcodes::ICONST_1);
            m->visitTypeInsn(Opcodes::ANEWARRAY, ref("java/lang/String"));
            m->visitVarInsn(Opcodes::ASTORE, 0);
            m->visitLabel(start);
            m->visitVarInsn(Opcodes::ALOAD, 0);
            m->visitInsn(Opcodes::ICONST_0);
            m->visitInsn(Opcodes::ICONST_5);
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Integer"), ref("valueOf"),
                               ref("(I)Ljava/lang/Integer;"), false);
            m->visitInsn(Opcodes::AASTORE);
            m->visitInsn(Opcodes::ICONST_0);
            m->visitInsn(Opcodes::IRETURN);
            m->visitLabel(end);
            m->visitLabel(handler);
            m->visitInsn(Opcodes::POP);
            m->visitInsn(Opcodes::ICONST_1);
            m->visitInsn(Opcodes::IRETURN);
        }));
    // 合法协变存储：Object[] a = new String[1]; a[0] = "ok"; → "ok".length()
    methods.push_back(makeMethod("validCovariantStore", "()I",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
                                     m->visitInsn(Opcodes::ICONST_1);
                                     m->visitTypeInsn(Opcodes::ANEWARRAY,
                                                      ref("java/lang/String"));
                                     m->visitVarInsn(Opcodes::ASTORE, 0);
                                     m->visitVarInsn(Opcodes::ALOAD, 0);
                                     m->visitInsn(Opcodes::ICONST_0);
                                     m->visitLdcInsn(ca::core::Any(textRef("ok")));
                                     m->visitInsn(Opcodes::AASTORE);
                                     m->visitVarInsn(Opcodes::ALOAD, 0);
                                     m->visitInsn(Opcodes::ICONST_0);
                                     m->visitInsn(Opcodes::AALOAD);
                                     m->visitTypeInsn(Opcodes::CHECKCAST,
                                                      ref("java/lang/String"));
                                     m->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                        ref("java/lang/String"), ref("length"),
                                                        ref("()I"), false);
                                     m->visitInsn(Opcodes::IRETURN);
                                 }));
    // 二级协变：Number[] n = new Integer[1]; n[0] = Long.valueOf(1) → ASE。
    methods.push_back(makeMethod(
        "covariantLongIntoIntegerThrows", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
        [](MethodNode* m) {
            auto* start   = new Label();
            auto* end     = new Label();
            auto* handler = new Label();
            m->visitTryCatchBlock(start, end, handler, ref("java/lang/ArrayStoreException"));
            m->visitInsn(Opcodes::ICONST_1);
            m->visitTypeInsn(Opcodes::ANEWARRAY, ref("java/lang/Integer"));
            m->visitVarInsn(Opcodes::ASTORE, 0);
            m->visitLabel(start);
            m->visitVarInsn(Opcodes::ALOAD, 0);
            m->visitInsn(Opcodes::ICONST_0);
            m->visitInsn(Opcodes::LCONST_1);
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Long"), ref("valueOf"),
                               ref("(J)Ljava/lang/Long;"), false);
            m->visitInsn(Opcodes::AASTORE);
            m->visitInsn(Opcodes::ICONST_0);
            m->visitInsn(Opcodes::IRETURN);
            m->visitLabel(end);
            m->visitLabel(handler);
            m->visitInsn(Opcodes::POP);
            m->visitInsn(Opcodes::ICONST_1);
            m->visitInsn(Opcodes::IRETURN);
        }));
    // primitive 数组存储照常工作。
    methods.push_back(makeMethod("primitiveStoreOk", "()I",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
                                     m->visitInsn(Opcodes::ICONST_1);
                                     m->visitIntInsn(Opcodes::NEWARRAY, Opcodes::T_INT);
                                     m->visitVarInsn(Opcodes::ASTORE, 0);
                                     m->visitVarInsn(Opcodes::ALOAD, 0);
                                     m->visitInsn(Opcodes::ICONST_0);
                                     m->visitIntInsn(Opcodes::BIPUSH, 7);
                                     m->visitInsn(Opcodes::IASTORE);
                                     m->visitVarInsn(Opcodes::ALOAD, 0);
                                     m->visitInsn(Opcodes::ICONST_0);
                                     m->visitInsn(Opcodes::IALOAD);
                                     m->visitInsn(Opcodes::IRETURN);
                                 }));
    // 捕获的异常类型名精确等于 java.lang.ArrayStoreException。
    methods.push_back(makeMethod(
        "aseTypeName", "()Z", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
            auto* start   = new Label();
            auto* end     = new Label();
            auto* handler = new Label();
            m->visitTryCatchBlock(start, end, handler, ref("java/lang/ArrayStoreException"));
            m->visitInsn(Opcodes::ICONST_1);
            m->visitTypeInsn(Opcodes::ANEWARRAY, ref("java/lang/String"));
            m->visitVarInsn(Opcodes::ASTORE, 0);
            m->visitLabel(start);
            m->visitVarInsn(Opcodes::ALOAD, 0);
            m->visitInsn(Opcodes::ICONST_0);
            m->visitInsn(Opcodes::ICONST_5);
            m->visitMethodInsn(Opcodes::INVOKESTATIC, ref("java/lang/Integer"), ref("valueOf"),
                               ref("(I)Ljava/lang/Integer;"), false);
            m->visitInsn(Opcodes::AASTORE);
            m->visitInsn(Opcodes::ICONST_0);
            m->visitInsn(Opcodes::IRETURN);
            m->visitLabel(end);
            m->visitLabel(handler);
            m->visitVarInsn(Opcodes::ASTORE, 1);
            m->visitVarInsn(Opcodes::ALOAD, 1);
            m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/Object"), ref("getClass"),
                               ref("()Ljava/lang/Class;"), false);
            m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/Class"), ref("getName"),
                               ref("()Ljava/lang/String;"), false);
            m->visitLdcInsn(ca::core::Any(textRef("java.lang.ArrayStoreException")));
            m->visitMethodInsn(Opcodes::INVOKEVIRTUAL, ref("java/lang/String"), ref("equals"),
                               ref("(Ljava/lang/Object;)Z"), false);
            m->visitInsn(Opcodes::IRETURN);
        }));

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("ArrayStoreCheck");
    cn.superName = ref("java/lang/Object");
    cn.methods   = std::move(methods);
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

// ---------------------------------------------------------------------------
// G17.4：兄弟方法完整性（修复①）。一个类内 7 个 static + 1 个 instance 方法
// 覆盖不同栈帧形状（long/float 局部、try-catch、数组循环、StringBuilder 拼接、
// 共享常量池），任一方法的帧计算串味都会在 -Xverify:all 加载期或行为断言爆掉。
// ---------------------------------------------------------------------------
inline std::vector<ca::u8> genSiblingMethods(ca::str::Utf8StringArena& arena)
{
    std::vector<MethodNode*> methods;

    // s1: ("sibling-alpha".length() << 1) + x → 13*2+3 = 29
    methods.push_back(makeMethod("s1", "(I)I", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 [](MethodNode* m) {
                                     m->visitLdcInsn(ca::core::Any(textRef("sibling-alpha")));
                                     m->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                        ref("java/lang/String"), ref("length"),
                                                        ref("()I"), false);
                                     m->visitInsn(Opcodes::ICONST_1);
                                     m->visitInsn(Opcodes::ISHL);
                                     m->visitVarInsn(Opcodes::ILOAD, 0);
                                     m->visitInsn(Opcodes::IADD);
                                     m->visitInsn(Opcodes::IRETURN);
                                 }));
    // s2: v * 1000003 + 7 → 5000022
    methods.push_back(makeMethod("s2", "(J)J", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 [](MethodNode* m) {
                                     m->visitVarInsn(Opcodes::LLOAD, 0);
                                     m->visitLdcInsn(
                                         ca::core::Any(static_cast<ca::i64>(1000003)));
                                     m->visitInsn(Opcodes::LMUL);
                                     m->visitLdcInsn(ca::core::Any(static_cast<ca::i64>(7)));
                                     m->visitInsn(Opcodes::LADD);
                                     m->visitInsn(Opcodes::LRETURN);
                                 }));
    // s3: f * 1.5f + 0.25f → 3.25f
    methods.push_back(makeMethod("s3", "(F)F", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 [](MethodNode* m) {
                                     m->visitVarInsn(Opcodes::FLOAD, 0);
                                     m->visitLdcInsn(ca::core::Any(1.5f));
                                     m->visitInsn(Opcodes::FMUL);
                                     m->visitLdcInsn(ca::core::Any(0.25f));
                                     m->visitInsn(Opcodes::FADD);
                                     m->visitInsn(Opcodes::FRETURN);
                                 }));
    // s4: s.charAt(1) - s.charAt(0) → 'a' - 'z' = -25
    methods.push_back(makeMethod("s4", "(Ljava/lang/String;)I",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
                                     m->visitVarInsn(Opcodes::ALOAD, 0);
                                     m->visitInsn(Opcodes::ICONST_1);
                                     m->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                        ref("java/lang/String"), ref("charAt"),
                                                        ref("(I)C"), false);
                                     m->visitVarInsn(Opcodes::ALOAD, 0);
                                     m->visitInsn(Opcodes::ICONST_0);
                                     m->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                        ref("java/lang/String"), ref("charAt"),
                                                        ref("(I)C"), false);
                                     m->visitInsn(Opcodes::ISUB);
                                     m->visitInsn(Opcodes::IRETURN);
                                 }));
    // s5: try { 100 / x } catch (ArithmeticException e) { -1 }
    methods.push_back(makeMethod(
        "s5", "(I)I", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
            auto* start   = new Label();
            auto* end     = new Label();
            auto* handler = new Label();
            m->visitTryCatchBlock(start, end, handler, ref("java/lang/ArithmeticException"));
            m->visitLabel(start);
            m->visitIntInsn(Opcodes::BIPUSH, 100);
            m->visitVarInsn(Opcodes::ILOAD, 0);
            m->visitInsn(Opcodes::IDIV);
            m->visitInsn(Opcodes::IRETURN);
            m->visitLabel(end);
            m->visitLabel(handler);
            m->visitInsn(Opcodes::POP);
            m->visitInsn(Opcodes::ICONST_M1);
            m->visitInsn(Opcodes::IRETURN);
        }));
    // s6: a=new int[n]; a[i]=i*i 两轮循环求和 → n=4 时 14
    methods.push_back(makeMethod("s6", "(I)I", Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                 [](MethodNode* m) {
                                     auto* fillCond = new Label();
                                     auto* fillDone = new Label();
                                     auto* sumCond  = new Label();
                                     auto* sumDone  = new Label();
                                     m->visitVarInsn(Opcodes::ILOAD, 0);
                                     m->visitIntInsn(Opcodes::NEWARRAY, Opcodes::T_INT);
                                     m->visitVarInsn(Opcodes::ASTORE, 1);
                                     m->visitInsn(Opcodes::ICONST_0);
                                     m->visitVarInsn(Opcodes::ISTORE, 2);
                                     m->visitLabel(fillCond);
                                     m->visitVarInsn(Opcodes::ILOAD, 2);
                                     m->visitVarInsn(Opcodes::ILOAD, 0);
                                     m->visitJumpInsn(Opcodes::IF_ICMPGE, fillDone);
                                     m->visitVarInsn(Opcodes::ALOAD, 1);
                                     m->visitVarInsn(Opcodes::ILOAD, 2);
                                     m->visitVarInsn(Opcodes::ILOAD, 2);
                                     m->visitVarInsn(Opcodes::ILOAD, 2);
                                     m->visitInsn(Opcodes::IMUL);
                                     m->visitInsn(Opcodes::IASTORE);
                                     m->visitIincInsn(2, 1);
                                     m->visitJumpInsn(Opcodes::GOTO, fillCond);
                                     m->visitLabel(fillDone);
                                     m->visitInsn(Opcodes::ICONST_0);
                                     m->visitVarInsn(Opcodes::ISTORE, 3);
                                     m->visitInsn(Opcodes::ICONST_0);
                                     m->visitVarInsn(Opcodes::ISTORE, 2);
                                     m->visitLabel(sumCond);
                                     m->visitVarInsn(Opcodes::ILOAD, 2);
                                     m->visitVarInsn(Opcodes::ILOAD, 0);
                                     m->visitJumpInsn(Opcodes::IF_ICMPGE, sumDone);
                                     m->visitVarInsn(Opcodes::ILOAD, 3);
                                     m->visitVarInsn(Opcodes::ALOAD, 1);
                                     m->visitVarInsn(Opcodes::ILOAD, 2);
                                     m->visitInsn(Opcodes::IALOAD);
                                     m->visitInsn(Opcodes::IADD);
                                     m->visitVarInsn(Opcodes::ISTORE, 3);
                                     m->visitIincInsn(2, 1);
                                     m->visitJumpInsn(Opcodes::GOTO, sumCond);
                                     m->visitLabel(sumDone);
                                     m->visitVarInsn(Opcodes::ILOAD, 3);
                                     m->visitInsn(Opcodes::IRETURN);
                                 }));
    // s7: StringBuilder 拼接 "sibling-shared"（与 ldc 常量池共享内容）。
    methods.push_back(makeMethod("s7", "()Ljava/lang/String;",
                                 Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, [](MethodNode* m) {
                                     m->visitTypeInsn(Opcodes::NEW,
                                                      ref("java/lang/StringBuilder"));
                                     m->visitInsn(Opcodes::DUP);
                                     m->visitMethodInsn(Opcodes::INVOKESPECIAL,
                                                        ref("java/lang/StringBuilder"),
                                                        ref("<init>"), ref("()V"), false);
                                     m->visitLdcInsn(ca::core::Any(textRef("sibling")));
                                     m->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                        ref("java/lang/StringBuilder"),
                                                        ref("append"),
                                                        ref("(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                                                        false);
                                     m->visitLdcInsn(ca::core::Any(textRef("-shared")));
                                     m->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                        ref("java/lang/StringBuilder"),
                                                        ref("append"),
                                                        ref("(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                                                        false);
                                     m->visitMethodInsn(Opcodes::INVOKEVIRTUAL,
                                                        ref("java/lang/StringBuilder"),
                                                        ref("toString"),
                                                        ref("()Ljava/lang/String;"), false);
                                     m->visitInsn(Opcodes::ARETURN);
                                 }));
    // inst: instance 方法读 static final 字段 + 参数 → 11+4 = 15。
    methods.push_back(makeMethod("inst", "(I)I", Opcodes::ACC_PUBLIC, [](MethodNode* m) {
        m->visitFieldInsn(Opcodes::GETSTATIC, ref("SiblingMethods"), ref("base"), ref("I"));
        m->visitVarInsn(Opcodes::ILOAD, 1);
        m->visitInsn(Opcodes::IADD);
        m->visitInsn(Opcodes::IRETURN);
    }));
    // <init>：默认构造（driver new SiblingMethods() 必需）。
    methods.push_back(makeMethod("<init>", "()V", Opcodes::ACC_PUBLIC, [](MethodNode* m) {
        m->visitVarInsn(Opcodes::ALOAD, 0);
        m->visitMethodInsn(Opcodes::INVOKESPECIAL, ref("java/lang/Object"), ref("<init>"),
                           ref("()V"), false);
        m->visitInsn(Opcodes::RETURN);
    }));

    ClassNode cn;
    cn.version   = Opcodes::V1_8;
    cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    cn.name      = ref("SiblingMethods");
    cn.superName = ref("java/lang/Object");
    cn.methods   = std::move(methods);
    // static final int base = 11（ConstantValue attribute）。
    cn.fields.push_back(new FieldNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC | Opcodes::ACC_FINAL,
                                      ref("base"), ref("I"), ref(""), ca::core::Any(11)));
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    cn.accept(&writer);
    return writer.toByteArray();
}

// ---------------------------------------------------------------------------
// G17.5：注解可见性（修复⑤）。自建 @Retention(RUNTIME) 注解（带 int/String
// 元素与 AnnotationDefault），样本方法分别带注解 / 无注解 / @Deprecated；
// 保护后反射可读性逐项对拍 JVM（注解面零反编译产物，纯语义锁）。
// ---------------------------------------------------------------------------
inline SampleBundle genAnnotationProbe(ca::str::Utf8StringArena& arena)
{
    // ---- RetainedAnno：@interface，RUNTIME retention，value/name 带默认值 ----
    std::vector<ca::u8> annoBytes;
    {
        ClassNode cn;
        cn.version = Opcodes::V1_8;
        cn.access  = Opcodes::ACC_PUBLIC | Opcodes::ACC_ANNOTATION | Opcodes::ACC_INTERFACE |
                    Opcodes::ACC_ABSTRACT;
        cn.name      = ref("RetainedAnno");
        cn.superName = ref("java/lang/Object");
        cn.interfaces.push_back(ref("java/lang/annotation/Annotation"));
        auto* retention = cn.visitAnnotation(ref("Ljava/lang/annotation/Retention;"), true);
        retention->visitEnum(ref("value"), ref("Ljava/lang/annotation/RetentionPolicy;"),
                             ref("RUNTIME"));
        retention->visitEnd();

        const auto abstractFlags = Opcodes::ACC_PUBLIC | Opcodes::ACC_ABSTRACT;
        auto* value = new MethodNode(abstractFlags, ref("value"), ref("()I"), ref(""),
                                     /*exceptions*/ {});
        {
            auto* dv = value->visitAnnotationDefault();
            dv->visit(ca::str::Utf8StringRef(), ca::core::Any(42));
            dv->visitEnd();
        }
        value->visitEnd();
        cn.methods.push_back(value);
        auto* name = new MethodNode(abstractFlags, ref("name"), ref("()Ljava/lang/String;"),
                                    ref(""), /*exceptions*/ {});
        {
            auto* dv = name->visitAnnotationDefault();
            dv->visit(ca::str::Utf8StringRef(), ca::core::Any(textRef("probe")));
            dv->visitEnd();
        }
        name->visitEnd();
        cn.methods.push_back(name);

        ClassWriter writer(arena, ClassWriter::COMPUTE_MAXS);
        cn.accept(&writer);
        annoBytes = writer.toByteArray();
    }

    // ---- AnnotationProbe：annotated(@RetainedAnno(7, "m1")) / plain / legacy(@Deprecated) ----
    std::vector<ca::u8> probeBytes;
    {
        ClassNode cn;
        cn.version   = Opcodes::V1_8;
        cn.access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
        cn.name      = ref("AnnotationProbe");
        cn.superName = ref("java/lang/Object");

        auto* annotated =
            new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, ref("annotated"),
                           ref("()I"), ref(""), /*exceptions*/ {});
        {
            auto* av = annotated->visitAnnotation(ref("LRetainedAnno;"), true);
            av->visit(ref("value"), ca::core::Any(7));
            av->visit(ref("name"), ca::core::Any(textRef("m1")));
            av->visitEnd();
            annotated->visitCode();
            annotated->visitIntInsn(Opcodes::BIPUSH, 7);
            annotated->visitInsn(Opcodes::IRETURN);
            annotated->visitMaxs(0, 0);
            annotated->visitEnd();
        }
        cn.methods.push_back(annotated);

        auto* plain = new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC, ref("plain"),
                                     ref("()I"), ref(""), /*exceptions*/ {});
        plain->visitCode();
        plain->visitInsn(Opcodes::ICONST_1);
        plain->visitInsn(Opcodes::IRETURN);
        plain->visitMaxs(0, 0);
        plain->visitEnd();
        cn.methods.push_back(plain);

        // @Deprecated 与 javac 同形：ACC_DEPRECATED 访问位（method_writer 由
        // 此发射 JVMS `Deprecated` 属性）+ RuntimeVisibleAnnotations 条目。
        // 注：类型描述符是 Ljava/lang/Deprecated;（java.lang 包）——写错包名
        // 时 HotSpot 对解析失败的注解条目静默忽略，反射恒 false（#1117 样本
        // 自检时定位的坑，防回退）。
        auto* legacy =
            new MethodNode(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC | Opcodes::ACC_DEPRECATED,
                           ref("legacy"), ref("()I"), ref(""), /*exceptions*/ {});
        {
            legacy->visitAnnotation(ref("Ljava/lang/Deprecated;"), true)->visitEnd();
            legacy->visitCode();
            legacy->visitInsn(Opcodes::ICONST_3);
            legacy->visitInsn(Opcodes::IRETURN);
            legacy->visitMaxs(0, 0);
            legacy->visitEnd();
        }
        cn.methods.push_back(legacy);

        ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
        cn.accept(&writer);
        probeBytes = writer.toByteArray();
    }

    SampleBundle bundle;
    bundle.artifacts.push_back({"RetainedAnno.class", std::move(annoBytes)});
    bundle.artifacts.push_back({"AnnotationProbe.class", std::move(probeBytes)});
    return bundle;
}

}   // namespace mtest_set::wave4
