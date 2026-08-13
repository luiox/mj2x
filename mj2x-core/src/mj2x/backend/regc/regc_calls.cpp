/// @file regc_calls.cpp
/// @brief RegC 发射器方法调用/字段与类缓存 helper（regc_emitter_internal.hpp 的内部接口）。
///
/// @details
/// 调用路径：static/instance 缓存名登记、native-to-native 直接调用、
/// 缓存化 JNI 调用（jvalue 数组 + CallXxxMethodA）、resolved native dispatch。
/// 从原 regc_emitter.cpp 纯搬移，生成代码零差异。
#include "regc_emitter_internal.hpp"

#include "jbytecode/jbytecode.hpp"
#include "libca/str/format.hpp"
#include "mj2x/codegen/c_expr.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"

namespace mj2x {

using namespace jbytecode::core;

using codegen::cStringLiteral;
using codegen::CSourceStream;

/// 生成「指针相等短路」的 JNI 对象关系检查表达式：
///   (lhs == rhs || (*env)->JniFn(env, lhs, rhs))
/// 短路仅在指针相等时生效——此时 JNI 语义必为 true（同一对象 self-equal），
/// 因此语义与纯 JNI 调用完全一致，不引入任何行为差异。实证：HotSpot 的
/// jobject 是间接句柄（local/global handle 指针不同），该比较恒为 false，
/// 本优化在 HotSpot 上不省 JNI（代价 ~1 次 C 比较/检查）；保留价值：非句柄
/// 式 JVM 上若句柄指针相等则跳过 JNI 调用，且恒等比较无副作用。
std::string shortCircuitRefCheck(const char* jniFn, const std::string& lhs,
                                 const std::string& rhs)
{
    // 返回 (lhs == rhs || (*env)->JniFn(env, lhs, rhs))：外层括号保证与
    // `&&` 组合时优先级正确（A != NULL && (ptr_eq || JNI(...))），且 A==NULL
    // 时整体为 false，绝不调用 JNI（IsInstanceOf 的 clazz 参数 NULL 是 JNI
    // 未定义行为，曾导致 JVM 崩溃——issue #8xx 回归教训）。
    return codegen::exprParen(codegen::exprBinop(
        "||",
        codegen::exprBinop("==", lhs, rhs),
        codegen::exprCall(jniFn, {"_mj2x_regc_env", lhs, rhs})));
}

/// 查寄存器对应的 native 包装对象（StringBuilder/StringBuffer），非寄存器或未登记返回
/// nullptr。
const RegCNativeObjectState* nativeObjectFor(const RegCFunctionEmitState& state,
                                             const RegOperand&            operand)
{
    if (operand.kind != RegOperandKind::Register) return nullptr;
    auto it = state.nativeObjects.find(operand.reg.value);
    if (it == state.nativeObjects.end()) return nullptr;
    return &it->second;
}

bool isStaticCall(const RegInstruction& ins)
{
    return ins.method.invokeOpcode == Opcodes::INVOKESTATIC;
}

bool isSelfStaticCall(const RegCFunctionEmitState& state, const RegInstruction& ins)
{
    return isStaticCall(ins) &&
           ca::str::Utf8StringRef::from_string_view(state.symbol.owner) == ins.method.owner &&
           ca::str::Utf8StringRef::from_string_view(state.symbol.name) == ins.method.name &&
           ca::str::Utf8StringRef::from_string_view(state.symbol.desc) == ins.method.desc;
}

std::string cacheKey(const ca::str::Utf8StringRef& first, const ca::str::Utf8StringRef& second,
                     const ca::str::Utf8StringRef& third)
{
    return first.to_std_string() + "\n" + second.to_std_string() + "\n" + third.to_std_string();
}

ca::str::Utf8String addStaticMethodCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->staticMethodCache(
            ins.method.owner, ins.method.name, ins.method.desc);
    }

    const std::string key = cacheKey(ins.method.owner, ins.method.name, ins.method.desc);
    auto              it  = state.staticMethodCacheNames.find(key);
    if (it != state.staticMethodCacheNames.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_cache_");
    cacheNameBuilder.append(state.functionName);
    cacheNameBuilder.append("_");
    cacheNameBuilder.append(ca::str::format_std("{}", state.staticMethodCacheIndex++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (state.commentLevel >= RegCCommentLevel::Summary) {
        decl.append("// cache: static ");
        decl.append(codegen::escapeCommentIdent(ins.method.owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(ins.method.name));
        decl.append(codegen::escapeCommentIdent(ins.method.desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_static_method_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(ins.method.owner));
    decl.append(", ");
    decl.append(cStringLiteral(ins.method.name));
    decl.append(", ");
    decl.append(cStringLiteral(ins.method.desc));
    decl.append(", NULL, NULL};\n");
    state.staticMethodCacheDeclarations.push_back(decl.build());
    state.staticMethodCacheNames.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String addInstanceMethodCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool == nullptr) return ca::str::Utf8String();
    return state.cachePool->instanceMethodCache(ins.method.owner, ins.method.name, ins.method.desc);
}

/// 判定调用是否确定性分派：静态/特殊调用总是确定；虚调用仅当目标方法 final
/// 或 owner 类 final（无子类可覆盖）时确定；虚调用在 owner 的所有编译输入
/// 子类中无同名同描述方法覆写时也确定（protected/package 方法常见场景，如
/// AbstractWorker.bias——子类只覆写 apply）。接口调用保持动态分派。类层级
/// 闭包只含编译输入：owner 是编译类时不可能存在库（JDK）子类，分析安全。
bool isDeterministicDispatch(const RegInstruction& ins, CompilerIndex* classIndex)
{
    switch (ins.method.invokeOpcode) {
    case Opcodes::INVOKESTATIC:
    case Opcodes::INVOKESPECIAL:
        return true;
    case Opcodes::INVOKEVIRTUAL:
        break;
    default:
        return false;
    }
    if (classIndex == nullptr) return false;
    const CompilerClass* ownerClass =
        classIndex->getClass(ins.method.owner.to_std_string());
    if (ownerClass == nullptr || ownerClass->classNode == nullptr) return false;
    if ((ownerClass->classNode->access & Opcodes::ACC_FINAL) != 0) return true;
    for (const auto& member : ownerClass->methods) {
        if (member.methodNode == nullptr) continue;
        if (member.methodNode->name != ins.method.name) continue;
        if (member.methodNode->desc != ins.method.desc) continue;
        return (member.methodNode->access & Opcodes::ACC_FINAL) != 0;
    }
    // 无覆写分析：owner 的每个编译输入子类（含递归）都不声明该方法 → 确定。
    for (const auto& [clsName, cls] : classIndex->getClasses()) {
        if (clsName == ins.method.owner.to_std_string() || cls.isLibrary) continue;
        const CompilerClass* cur = &cls;
        std::string          curName = clsName;
        bool                 isSub = false;
        while (cur != nullptr && cur->classNode != nullptr) {
            curName = cur->classNode->superName.to_std_string();
            if (curName.empty()) break;
            if (curName == ins.method.owner.to_std_string()) {
                isSub = true;
                break;
            }
            cur = classIndex->getClass(curName);
        }
        if (!isSub) continue;
        for (const auto& member : cls.methods) {
            if (member.methodNode == nullptr) continue;
            if (member.methodNode->name != ins.method.name) continue;
            if (member.methodNode->desc != ins.method.desc) continue;
            return false;   // 有子类覆写 → 动态分派
        }
    }
    return true;
}

/// 发射 native-to-native 直接调用（跳过 JNI 方法分派）。要求：目标方法已成功
/// 转换、分派确定、无 invokedynamic 站点、无 native 状态参数逃逸。未命中时
/// 返回 false，调用方继续走 intrinsic/通用 JNI 路径。
bool tryEmitDirectCall(CSourceStream& out, const RegInstruction& ins,
                       RegCFunctionEmitState& state, CompilerIndex* classIndex,
                       const std::unordered_map<std::string, std::string>* convertedFunctions,
                       const std::unordered_set<std::string>* indyFunctions)
{
    if (convertedFunctions == nullptr) return false;
    if (isSelfStaticCall(state, ins)) return false;
    const std::string name = ins.method.name.to_std_string();
    if (name.empty() || name[0] == '<') return false;   // <init>/<clinit> 保持 JNI
    const std::string key = ins.method.owner.to_std_string() + "\n" + name + "\n" +
                            ins.method.desc.to_std_string();
    const auto it = convertedFunctions->find(key);
    if (it == convertedFunctions->end() || it->second.empty()) return false;
    if (!isDeterministicDispatch(ins, classIndex)) return false;
    if (indyFunctions != nullptr && indyFunctions->count(key) != 0) return false;
    // native 状态参数在寄存器里是 NULL，直接调用会破坏被调方 ABI；回退通用路径。
    for (const auto& arg : ins.args) {
        if (nativeObjectFor(state, arg) != nullptr) return false;
    }
    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();
    // 前向声明：目标函数定义可能在本调用点之后（同类方法转换顺序不保证，
    // 类内预登记使后转换方法也可直调）。无声明时 C 隐式 int() 声明与 REF
    // 返回值冲突（C2040）。参数类型与 emitFunction 参数发射一致（receiver
    // = void* r0，后续按实参 SSAType）。
    out.appendLineStart("static ")
        .append(ins.hasDest ? cType(ins.dest.type) : "void")
        .append(" ")
        .append(it->second)
        .append("(");
    for (size_t i = 0; i < ins.args.size(); ++i) {
        if (i) out.append(", ");
        out.append(cType(ins.args[i].type)).append(" r").append(ca::str::format_std("{}", i));
    }
    out.append(");").newLine();
    out.appendLineStart();
    if (ins.hasDest) {
        out.append(destName).append(" = ");
    }
    out.append(it->second)
        .append("(")
        .append(directCallArgs(ins.args))
        .append(");")
        .newLine();
    return true;
}

/// 发射缓存化的 JNI 调用点。与 varargs 帮手相比省去了每次调用的描述符
/// 扫描（_mj2x_regc_count_params / _mj2x_regc_skip_type）与 va_list 搬运，
/// 直接按已知参数类型填充 jvalue 数组并走 CallXxxMethodA。行为与原有
/// _mj2x_regc_call_cached_* 帮手一致：方法/类解析失败时目标寄存器落默认值，
/// 实例调用 receiver 为 NULL 时抛 NullPointerException。
void emitCachedCall(CSourceStream& out, const RegInstruction& ins, bool staticCall,
                    const ca::str::Utf8StringRef& cacheName,
                    const std::vector<ca::str::Utf8String>& expressions, bool ownerMethodOnly)
{
    const char               returnTag = descriptorReturnTag(ins.method.desc);
    const std::vector<char>  paramTags = descriptorParamTags(ins.method.desc);
    const bool               hasDest   = ins.hasDest;
    const std::string        destName  = hasDest ? regName(ins.dest.id) : std::string();
    const ca::str::Utf8String argsArg  = paramTags.empty() ? ca::str::Utf8String("NULL")
                                                          : ca::str::Utf8String("_mj2x_regc_va");

    out.block([&](CSourceStream& o) {
        if (!staticCall) {
            o.declare_var("jobject", "_mj2x_regc_rcv",
                          codegen::exprCast("jobject",
                                            codegen::exprParen(expressions[0].to_std_string())));
        }
        if (!paramTags.empty()) {
            o.declare_var("jvalue", ca::str::format_std("_mj2x_regc_va[{}]", paramTags.size()));
        }
        o.declare_var("jmethodID", "_mj2x_regc_m");
        if (staticCall) {
            for (size_t i = 0; i < paramTags.size(); ++i) {
                o.assign(codegen::exprMember(codegen::exprIndex("_mj2x_regc_va",
                                                                ca::str::format_std("{}", i)),
                                             jvalueSlotName(paramTags[i])),
                         std::string(jniValueCast(paramTags[i])) +
                             codegen::exprParen(expressions[i].to_std_string()));
            }
            o.assign("_mj2x_regc_m",
                     codegen::exprCall("_mj2x_regc_cached_static_method",
                                       {"&" + cacheName.to_std_string()}));
            const std::string callExpr = codegen::exprCall(
                std::string("(*_mj2x_regc_env)->CallStatic") + jniCallFamily(returnTag) + "MethodA",
                {"_mj2x_regc_env", codegen::exprMember(cacheName.to_std_string(), "ownerClass"),
                 "_mj2x_regc_m", argsArg.to_std_string()});
            o.iff(codegen::exprBinop(
                      "&&", codegen::exprBinop("!=", "_mj2x_regc_m", "NULL"),
                      codegen::exprBinop("!=", codegen::exprMember(cacheName.to_std_string(),
                                                                   "ownerClass"),
                                         "NULL")),
                  [&](CSourceStream& u) {
                      if (hasDest) {
                          u.assign(destName,
                                   std::string(jniResultCast(returnTag, ins.dest.type)) + callExpr);
                      }
                      else {
                          u.emitLine(callExpr + ";");
                      }
                  },
                  [&](CSourceStream& u) {
                      if (hasDest) {
                          u.assign(destName, std::string(defaultValue(ins.dest.type)));
                      }
                  });
        }
        else {
            o.iff(codegen::exprBinop("==", "_mj2x_regc_rcv", "NULL"),
                  [&](CSourceStream& u) {
                      u.emitLine("_mj2x_regc_throw_null(\"instance method call\");");
                      if (hasDest) {
                          u.assign(destName, std::string(defaultValue(ins.dest.type)));
                      }
                  },
                  [&](CSourceStream& u) {
                      for (size_t i = 0; i < paramTags.size(); ++i) {
                          u.assign(codegen::exprMember(
                                       codegen::exprIndex("_mj2x_regc_va",
                                                          ca::str::format_std("{}", i)),
                                       jvalueSlotName(paramTags[i])),
                                   std::string(jniValueCast(paramTags[i])) +
                                       codegen::exprParen(expressions[i + 1].to_std_string()));
                      }
                      u.assign("_mj2x_regc_m",
                               codegen::exprCall(
                                   ownerMethodOnly ? "_mj2x_regc_cached_owner_method"
                                                   : "_mj2x_regc_cached_instance_method",
                                   {"&" + cacheName.to_std_string(), "_mj2x_regc_rcv"}));
                      const std::string callExpr = codegen::exprCall(
                          std::string("(*_mj2x_regc_env)->Call") + jniCallFamily(returnTag) +
                              "MethodA",
                          {"_mj2x_regc_env", "_mj2x_regc_rcv", "_mj2x_regc_m",
                           argsArg.to_std_string()});
                      u.iff(codegen::exprBinop("!=", "_mj2x_regc_m", "NULL"),
                            [&](CSourceStream& v) {
                                if (hasDest) {
                                    v.assign(destName,
                                             std::string(jniResultCast(returnTag, ins.dest.type)) +
                                                 callExpr);
                                }
                                else {
                                    v.emitLine(callExpr + ";");
                                }
                            },
                            [&](CSourceStream& v) {
                                if (hasDest) {
                                    v.assign(destName, std::string(defaultValue(ins.dest.type)));
                                }
                            });
                  });
        }
    });
}

/// 发射动态实例调用的 resolved native dispatch：按 receiver 实际类解析 jmethodID，
/// 若该调用点解析出的实现是已转换方法则直接调用其 regc_* 函数体（跳过 JNI
/// 分派），否则回退 CallXxxMethodA。站点级缓存持有 receiver 类 global ref，
/// 同类重复调用只做 GetObjectClass + IsSameObject 检查。
/// omitDeleteLocalRef：方法已发射 PushLocalFrame 帧时省略每次 GetObjectClass
/// 后的 DeleteLocalRef（帧内 local ref 由 PopLocalFrame 批量释放，省 1 个 JNI
/// 调用/调用点；帧判定见 regc_emitter.cpp fnProducesLocalRefsIR）。
void emitResolvedInstanceCall(CSourceStream& out, const RegInstruction& ins,
                              const ca::str::Utf8StringRef& siteName,
                              const std::vector<ca::str::Utf8String>& expressions,
                              bool omitDeleteLocalRef)
{
    const char              returnTag = descriptorReturnTag(ins.method.desc);
    const std::vector<char> paramTags = descriptorParamTags(ins.method.desc);
    const bool              hasDest   = ins.hasDest;
    const std::string       destName  = hasDest ? regName(ins.dest.id) : std::string();
    const std::string       site      = siteName.to_std_string();
    const std::string       argsArg   = paramTags.empty() ? std::string("NULL")
                                                         : std::string("_mj2x_regc_va");

    // 函数指针签名与实参列表（直接调用专用）：与 regc_* 函数体 ABI 一致。
    std::string fnPointerSig = std::string(cType(ins.dest.type)) + " (*)(";
    std::vector<std::string> callArguments;
    callArguments.push_back(codegen::exprCast("mj2x_ref", codegen::exprParen("_mj2x_regc_rcv")));
    fnPointerSig += "mj2x_ref";
    for (size_t i = 0; i < paramTags.size(); ++i) {
        fnPointerSig += ", ";
        fnPointerSig += cType(ins.args[i + 1].type);
        callArguments.push_back(codegen::exprCast(
            cType(ins.args[i + 1].type), codegen::exprParen(expressions[i + 1].to_std_string())));
    }
    fnPointerSig += ")";
    // void 返回值用 void 签名（hasDest=false 时 cType 也是 "void"，但显式构建
    // 更清晰，避免从 dest 签名里抠参数子串）。
    std::string voidPointerSig = "void (*)(";
    voidPointerSig += "mj2x_ref";
    for (size_t i = 0; i < paramTags.size(); ++i) {
        voidPointerSig += ", ";
        voidPointerSig += cType(ins.args[i + 1].type);
    }
    voidPointerSig += ")";

    const std::string fnPtrCallExpr =
        codegen::exprCallArgs(codegen::exprParen(codegen::exprCast(fnPointerSig,
                                                                  codegen::exprMember(site, "fn"))),
                              callArguments);
    const std::string fnPtrCallExpr1 =
        codegen::exprCallArgs(codegen::exprParen(codegen::exprCast(fnPointerSig,
                                                                   codegen::exprMember(site,
                                                                                       "fn1"))),
                              callArguments);
    const std::string voidFnPtrCallExpr =
        codegen::exprCallArgs(codegen::exprParen(codegen::exprCast(voidPointerSig,
                                                                  codegen::exprMember(site, "fn"))),
                              callArguments);
    const std::string voidFnPtrCallExpr1 =
        codegen::exprCallArgs(codegen::exprParen(codegen::exprCast(voidPointerSig,
                                                                   codegen::exprMember(site,
                                                                                       "fn1"))),
                              callArguments);
    const std::string jniCallExpr = codegen::exprCall(
        std::string("(*_mj2x_regc_env)->Call") + jniCallFamily(returnTag) + "MethodA",
        {"_mj2x_regc_env", "_mj2x_regc_rcv", "_mj2x_regc_m", argsArg});

    out.block([&](CSourceStream& o) {
        o.emitLine("jobject _mj2x_regc_rcv = " +
                   codegen::exprCast("jobject",
                                     codegen::exprParen(expressions[0].to_std_string())) +
                   ";");
        o.iff(codegen::exprBinop("==", "_mj2x_regc_rcv", "NULL"),
              [&](CSourceStream& s) {
                  s.emitLine("_mj2x_regc_throw_null(\"instance method call\");");
                  if (hasDest) {
                      s.emitLine(destName + " = " + std::string(defaultValue(ins.dest.type)) + ";");
                  }
              },
              [&](CSourceStream& s) {
                  s.emitLine("jclass _mj2x_regc_rc = " +
                             codegen::exprCall("(*_mj2x_regc_env)->GetObjectClass",
                                               {"_mj2x_regc_env", "_mj2x_regc_rcv"}) +
                             ";");
                  s.emitLine("jmethodID _mj2x_regc_m = NULL;");
                   if (!paramTags.empty()) {
                       s.emitLine(ca::str::format_std("jvalue _mj2x_regc_va[{}];",
                                                      paramTags.size()));
                   }
                   s.iff(codegen::exprBinop("!=", "_mj2x_regc_rc", "NULL"),
                         [&](CSourceStream& t) {
                             // 检查顺序：receiverClass 精确匹配（IsSameObject，HotSpot 下
                             // 等价指针比较）优先——双槽各自命中一个精确 receiver 类，
                             // 交替 receiver 时每次调用只需 1 次检查（issue #780 双槽
                             // 缓存的正确用法）；implClass 的 IsInstanceOf 作为兜底，
                             // 保留「已转换实现类的未覆盖子类」语义（子类 receiver 精确
                             // 匹配 miss 后仍能命中 implClass，不改变分派结果）。
                             t.iff(codegen::exprBinop(
                                       "&&",
                                       codegen::exprBinop("!=",
                                                          codegen::exprMember(site,
                                                                              "receiverClass"),
                                                          "NULL"),
                                       shortCircuitRefCheck(
                                           "(*_mj2x_regc_env)->IsSameObject",
                                           codegen::exprMember(site, "receiverClass"),
                                           "_mj2x_regc_rc")),
                                   [&](CSourceStream& u) {
                                       u.emitLine("_mj2x_regc_m = " +
                                                  codegen::exprMember(site, "method") + ";");
                                   },
                                   [&](CSourceStream& t2) {
                                       // 槽 1（交替 receiver 优化，issue #780）：槽 0 未命中
                                       // 时检查上一 receiver 的精确类（同样 IsSameObject）。
                                       t2.iff(codegen::exprBinop(
                                                  "&&",
                                                  codegen::exprBinop(
                                                      "!=",
                                                      codegen::exprMember(site,
                                                                          "receiverClass1"),
                                                      "NULL"),
                                                   shortCircuitRefCheck(
                                                       "(*_mj2x_regc_env)->IsSameObject",
                                                       codegen::exprMember(site,
                                                                           "receiverClass1"),
                                                       "_mj2x_regc_rc")),
                                              [&](CSourceStream& u) {
                                                  u.emitLine("_mj2x_regc_m = " +
                                                             codegen::exprMember(site,
                                                                                 "method1") +
                                                             ";");
                                              },
                                              [&](CSourceStream& t3) {
                                                  // 快路径 2（单 JNI）：receiver 是已转换
                                                  // 实现类（或其未覆盖子类）→ 直接复用
                                                  // method/fn。implClass 只在 fn != NULL 时设置。
                                                  t3.iff(codegen::exprBinop(
                                                            "&&",
                                                            codegen::exprBinop(
                                                                "!=",
                                                                codegen::exprMember(
                                                                    site, "implClass"),
                                                                "NULL"),
                                                                shortCircuitRefCheck(
                                                                    "(*_mj2x_regc_env)"
                                                                    "->IsInstanceOf",
                                                                    "_mj2x_regc_rcv",
                                                                    codegen::exprMember(
                                                                        site, "implClass"))),
                                                        [&](CSourceStream& u) {
                                                            u.emitLine(
                                                                "_mj2x_regc_m = " +
                                                                codegen::exprMember(site,
                                                                                    "method") +
                                                                ";");
                                                        },
                                                        [&](CSourceStream& t4) {
                                                            t4.iff(
                                                                codegen::exprBinop(
                                                                    "&&",
                                                                    codegen::exprBinop(
                                                                        "!=",
                                                                        codegen::exprMember(
                                                                            site,
                                                                            "implClass1"),
                                                                        "NULL"),
                                                                        shortCircuitRefCheck(
                                                                            "(*_mj2x_regc_env)"
                                                                            "->IsInstanceOf",
                                                                            "_mj2x_regc_rcv",
                                                                            codegen::exprMember(
                                                                                site,
                                                                                "implClass1"))),
                                                                  [&](CSourceStream& u) {
                                                                      u.emitLine(
                                                                        "_mj2x_regc_m = " +
                                                                        codegen::exprMember(
                                                                            site, "method1") +
                                                                        ";");
                                                                },
                                                                [&](CSourceStream& u) {
                                                                    u.emitLine(
                                                                        "_mj2x_regc_m = " +
                                                                        codegen::exprCall(
                                                                            "_mj2x_regc_"
                                                                            "resolved_call_site_"
                                                                            "refresh",
                                                                            {"&" + site,
                                                                             "_mj2x_regc_rcv",
                                                                             "_mj2x_regc_rc",
                                                                             cStringLiteral(
                                                                                 ins.method.name),
                                                                             cStringLiteral(
                                                                                 ins.method
                                                                                     .desc)}) +
                                                                        ";");
                                                                });
                                                        });
                                              });
                                   });
                   if (!omitDeleteLocalRef) {
                       t.emitLine(codegen::exprCall("(*_mj2x_regc_env)->DeleteLocalRef",
                                                    {"_mj2x_regc_env", "_mj2x_regc_rc"}) +
                                  ";");
                   }
                        });
                   for (size_t i = 0; i < paramTags.size(); ++i) {
                      s.emitLine(codegen::exprMember(
                                     codegen::exprIndex("_mj2x_regc_va",
                                                        ca::str::format_std("{}", i)),
                                     jvalueSlotName(paramTags[i])) +
                                 " = " +
                                 std::string(jniValueCast(paramTags[i])) +
                                 codegen::exprParen(expressions[i + 1].to_std_string()) +
                                 ";");
                  }
                  if (hasDest) {
                      s.iff(codegen::exprBinop("==", "_mj2x_regc_m", "NULL"),
                            [&](CSourceStream& t) {
                                t.emitLine(destName + " = " +
                                           std::string(defaultValue(ins.dest.type)) + ";");
                            },
                            [&](CSourceStream& t) {
                                // fn 必须与 m 同槽（issue #780 错配教训）：m 来自槽 0
                                // （method 或 refresh 后交换）用 fn，来自槽 1 用 fn1。
                                t.iff(codegen::exprBinop(
                                          "==", "_mj2x_regc_m",
                                          codegen::exprMember(site, "method")),
                                      [&](CSourceStream& u) {
                                          u.iff(codegen::exprBinop(
                                                    "!=",
                                                    codegen::exprMember(site, "fn"), "NULL"),
                                                [&](CSourceStream& v) {
                                                    v.emitLine(destName + " = " +
                                                               jniResultCast(returnTag,
                                                                             ins.dest.type)
                                                                   .to_std_string() +
                                                               fnPtrCallExpr + ";");
                                                },
                                                [&](CSourceStream& v) {
                                                    v.emitLine(destName + " = " +
                                                               jniResultCast(returnTag,
                                                                             ins.dest.type)
                                                                   .to_std_string() +
                                                               jniCallExpr + ";");
                                                });
                                      },
                                      [&](CSourceStream& u) {
                                          u.iff(codegen::exprBinop(
                                                    "!=",
                                                    codegen::exprMember(site, "fn1"), "NULL"),
                                                [&](CSourceStream& v) {
                                                    v.emitLine(destName + " = " +
                                                               jniResultCast(returnTag,
                                                                             ins.dest.type)
                                                                   .to_std_string() +
                                                               fnPtrCallExpr1 + ";");
                                                },
                                                [&](CSourceStream& v) {
                                                    v.emitLine(destName + " = " +
                                                               jniResultCast(returnTag,
                                                                             ins.dest.type)
                                                                   .to_std_string() +
                                                               jniCallExpr + ";");
                                                });
                                      });
                            });
                  }
                  else {
                      s.iff(codegen::exprBinop("!=", "_mj2x_regc_m", "NULL"),
                            [&](CSourceStream& t) {
                                t.iff(codegen::exprBinop(
                                          "==", "_mj2x_regc_m",
                                          codegen::exprMember(site, "method")),
                                      [&](CSourceStream& u) {
                                          u.iff(codegen::exprBinop(
                                                    "!=",
                                                    codegen::exprMember(site, "fn"), "NULL"),
                                                [&](CSourceStream& v) {
                                                    v.emitLine(voidFnPtrCallExpr + ";");
                                                },
                                                [&](CSourceStream& v) {
                                                    v.emitLine(jniCallExpr + ";");
                                                });
                                      },
                                      [&](CSourceStream& u) {
                                          u.iff(codegen::exprBinop(
                                                    "!=",
                                                    codegen::exprMember(site, "fn1"), "NULL"),
                                                [&](CSourceStream& v) {
                                                    v.emitLine(voidFnPtrCallExpr1 + ";");
                                                },
                                                [&](CSourceStream& v) {
                                                    v.emitLine(jniCallExpr + ";");
                                                });
                                      });
                            });
                  }
              });
    });
}

ca::str::Utf8String addStaticFieldCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->staticFieldCache(ins.field.owner, ins.field.name, ins.field.desc);
    }

    const std::string key = cacheKey(ins.field.owner, ins.field.name, ins.field.desc);
    auto              it  = state.staticFieldCacheNames.find(key);
    if (it != state.staticFieldCacheNames.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_field_cache_");
    cacheNameBuilder.append(state.functionName);
    cacheNameBuilder.append("_");
    cacheNameBuilder.append(ca::str::format_std("{}", state.staticFieldCacheIndex++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (state.commentLevel >= RegCCommentLevel::Summary) {
        decl.append("// cache: static field ");
        decl.append(codegen::escapeCommentIdent(ins.field.owner));
        decl.append(".");
        decl.append(codegen::escapeCommentIdent(ins.field.name));
        decl.append(":");
        decl.append(codegen::escapeCommentIdent(ins.field.desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_static_field_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(ins.field.owner));
    decl.append(", ");
    decl.append(cStringLiteral(ins.field.name));
    decl.append(", ");
    decl.append(cStringLiteral(ins.field.desc));
    decl.append(", NULL, NULL};\n");
    state.staticFieldCacheDeclarations.push_back(decl.build());
    state.staticFieldCacheNames.emplace(key, cacheName.clone());
    return cacheName;
}

ca::str::Utf8String addInstanceFieldCache(RegCFunctionEmitState& state, const RegInstruction& ins)
{
    if (state.cachePool == nullptr) return ca::str::Utf8String();
    return state.cachePool->instanceFieldCache(ins.field.owner, ins.field.name, ins.field.desc);
}

ca::str::Utf8String addClassCache(RegCFunctionEmitState& state, const ca::str::Utf8StringRef& desc)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->classCache(desc);
    }

    const std::string descKey = desc.to_std_string();
    auto              it      = state.classCacheNames.find(descKey);
    if (it != state.classCacheNames.end()) return it->second.clone();

    CSourceStream cacheNameBuilder;
    cacheNameBuilder.append("_mj2x_regc_class_cache_");
    cacheNameBuilder.append(state.functionName);
    cacheNameBuilder.append("_");
    cacheNameBuilder.append(ca::str::format_std("{}", state.classCacheIndex++));
    ca::str::Utf8String cacheName = cacheNameBuilder.build();

    CSourceStream decl;
    if (state.commentLevel >= RegCCommentLevel::Summary) {
        decl.append("// cache: class ");
        decl.append(codegen::escapeCommentIdent(desc));
        decl.append("\n");
    }
    decl.append("static mj2x_regc_class_cache ");
    decl.append(cacheName);
    decl.append(" = {");
    decl.append(cStringLiteral(desc));
    decl.append(", NULL};\n");
    state.classCacheDeclarations.push_back(decl.build());
    state.classCacheNames.emplace(descKey, cacheName.clone());
    return cacheName;
}


}   // namespace mj2x
