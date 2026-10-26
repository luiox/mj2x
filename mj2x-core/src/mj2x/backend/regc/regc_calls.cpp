/// @file regc_calls.cpp
/// @brief RegC 发射器方法调用/字段与类缓存 helper（regc_emitter_internal.hpp 的内部接口）。
///
/// @details
/// 调用路径：static/instance 缓存名登记、native-to-native 直接调用、
/// 缓存化 JNI 调用（jvalue 数组 + CallXxxMethodA）、resolved native dispatch。
/// 从原 regc_emitter.cpp 纯搬移，生成代码零差异。
#include "regc_emitter_internal.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/core/jvm_opcodes.hpp"
#include "mj2x/core/jvm_type.hpp"
#include "libca/str/format.hpp"
#include "mj2x/codegen/c_expr.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"

namespace mj2x {


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
    // 三段式键构建统一走 methodKey（缓存名键与方法查表键同格式，N6 下沉）。
    return methodKey(first.to_std_string(), second.to_std_string(), third.to_std_string());
}

ca::str::Utf8StringRef addStaticMethodCache(RegCFunctionEmitState& state,
                                            const RegInstruction&  ins)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->staticMethodCache(
            ins.method.owner, ins.method.name, ins.method.desc);
    }

    const std::string key = cacheKey(ins.method.owner, ins.method.name, ins.method.desc);
    auto              it  = state.staticMethodCacheNames.find(key);
    if (it != state.staticMethodCacheNames.end()) return it->second.ref();

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
    return state.staticMethodCacheNames.emplace(key, cacheName.clone()).first->second.ref();
}

ca::str::Utf8StringRef addInstanceMethodCache(RegCFunctionEmitState& state,
                                              const RegInstruction&  ins)
{
    if (state.cachePool == nullptr) return ca::str::Utf8StringRef{};
    return state.cachePool->instanceMethodCache(ins.method.owner, ins.method.name, ins.method.desc);
}

/// 判定调用是否确定性分派：静态/特殊调用总是确定；虚调用在目标方法 final、
/// owner 类 final（无子类可覆盖）、或 owner 的所有编译输入子类（含递归）中
/// 无同名同描述方法覆写时确定（owner 自身声明的非 final 方法也走覆写分析
/// ——新版 javac 不给匿名类/其方法设 ACC_FINAL，靠 final 标志会漏判）。
/// 接口调用保持动态分派。类层级闭包只含编译输入：owner 是编译类时不可能
/// 存在库（JDK）子类，分析安全。
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
        // 声明为 final → 确定；声明非 final（如新版 javac 不给匿名类/方法设
        // ACC_FINAL）→ 继续做子类覆写分析，而不是直接判动态。
        if ((member.methodNode->access & Opcodes::ACC_FINAL) != 0) return true;
        break;
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
    // 直调档位（CodeOptions.directCallTier）：C 直调不产生 JVM 栈帧，
    // getStackTrace/StackWalker/反射等栈观察语义失真（issue #814）。档位按
    // 栈失真范围划分而非"开不开"——Off 之外每档都以失真换 JNI 边界开销。
    //
    // 两个免门控例外（任何档位都直调）：
    // - 确定性分派的虚调用：final 方法 / final owner / 编译输入无覆写时，
    //   resolved 路径命中 implClass 后本来就直调 fn（无 JVM 帧，语义与直调
    //   完全一致），运行时 IsInstanceOf 检查是纯冗余。
    // - 同类内调用：栈失真限制在类内，IntraClass 档起允许。
    const bool deterministicVirtual =
        ins.method.invokeOpcode == Opcodes::INVOKEVIRTUAL &&
        isDeterministicDispatch(ins, classIndex);
    const bool sameClass = ins.method.owner == state.symbol.owner;
    if (!allowsDirectCall(state.directCallTier, sameClass, deterministicVirtual)) return false;
    if (convertedFunctions == nullptr) return false;
    if (isSelfStaticCall(state, ins)) return false;
    const std::string name = ins.method.name.to_std_string();
    if (name.empty()) return false;
    // <clinit> 保持 JNI（类加载语义）；<init> 允许直调：INVOKESPECIAL 精确
    // 分派 + 已转换目标（构造器函数体），receiver 是分配对象（已知类），
    // 直调与 JNI 调用语义一致，省 resolved 的 GetObjectClass+IsSameObject。
    if (name[0] == '<' && name != "<init>") return false;
    // javac 合成枚举 values()/valueOf() 让路站点 intrinsic（#887 Enums 承接卡）：
    // 直调体会执行 3 次 JNI（getstatic + instance-cache clone + checkcast），
    // intrinsic 走站点缓存仅 2/1 次（类型检查可省——$VALUES 恒为 [Lowner;、
    // Enum.valueOf 返回恒为 owner）；且 intrinsic 同样无 JVM 栈帧，issue #814
    // 直调门控的栈观察权衡不变。仅让路给预扫描/按类补扫/SSA 自登记验证过的
    // 合成方法；未登记（非 javac 形状/非枚举 owner）维持直调。
    if (state.enumSyntheticMethods != nullptr && !state.enumSyntheticMethods->empty() &&
        ins.method.invokeOpcode == Opcodes::INVOKESTATIC &&
        (name == "values" || name == "valueOf")) {
        const std::string desc = ins.method.desc.to_std_string();
        const bool        syntheticShape =
            (name == "values" && desc.rfind("()[L", 0) == 0 && !desc.empty() &&
             desc.back() == ';') ||
            (name == "valueOf" && desc.rfind("(Ljava/lang/String;)L", 0) == 0);
        if (syntheticShape &&
            state.enumSyntheticMethods->count(
                methodKey(ins.method.owner.to_std_string(), name, desc)) != 0) {
            return false;
        }
    }
    std::string       key = methodKey(ins.method.owner.to_std_string(), name,
                                      ins.method.desc.to_std_string());
    auto              it  = convertedFunctions->find(key);
    if ((it == convertedFunctions->end() || it->second.empty()) && classIndex != nullptr) {
        // 继承方法：调用点 owner 是 receiver 静态类型，convertedFunctions 键
        // 是声明类——沿超类链（限编译输入）找声明类后重查。
        std::string cur        = ins.method.owner.to_std_string();
        bool        reResolved = false;
        for (int depth = 0; depth < 32 && !reResolved; ++depth) {
            CompilerClass* cls = classIndex->getClass(cur);
            if (cls == nullptr || cls->classNode == nullptr) break;
            const std::string super = cls->classNode->superName.to_std_string();
            if (super.empty()) break;
            CompilerClass* superCls = classIndex->getClass(super);
            if (superCls != nullptr && superCls->classNode != nullptr) {
                for (const auto& member : superCls->methods) {
                    if (member.methodNode == nullptr) continue;
                    if (member.methodNode->name.to_std_string() != name) continue;
                    if (member.methodNode->desc.to_std_string() !=
                        ins.method.desc.to_std_string()) {
                        continue;
                    }
                    cur        = super;
                    reResolved = true;
                    break;
                }
            }
            if (!reResolved) cur = super;
        }
        if (reResolved) {
            key = methodKey(cur, name, ins.method.desc.to_std_string());
            it  = convertedFunctions->find(key);
        }
    }
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
    // 返回值冲突（C2040）。声明登记到 TU 头部注册表——块作用域 static 函数
    // 声明违反 C11 6.7.1p6（GCC 拒绝，MSVC 放行），见 declareDirectCallTarget。
    declareDirectCallTarget(state, out, ins, it->second);
    if (deterministicVirtual && !ins.args.empty()) {
        // 免门控路径带 NULL receiver 守卫：JNI 调用点在 receiver 为 NULL 时
        // 抛 NPE（emitCachedCall 语义），直调必须显式复现。
        const std::string receiver = operandExpr(ins.args[0]).to_std_string();
        out.iff("((" + receiver + ") == NULL)",
                [&](CSourceStream& u) {
                    u.emitLine("_mj2x_regc_throw_null(\"instance method call\");");
                    if (ins.hasDest) {
                        u.emitLine(destName + " = " + std::string(defaultValue(ins.dest.type)) +
                                   ";");
                    }
                },
                [&](CSourceStream& u) {
                    u.appendLineStart();
                    if (ins.hasDest) {
                        u.append(destName).append(" = ");
                    }
                    u.append(it->second)
                        .append("(")
                        .append(directCallArgs(ins.args))
                        .append(");")
                        .newLine();
                });
        return true;
    }
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
/// 登记一个调用目标间接化站点，返回 (站点表名, 组内索引)。
///
/// 表按返回形态（dispatcher 后缀）分组：同签名共享一张表、组内独立计数。
/// 登记顺序即指令遍历序（确定性），产物可复现。
std::pair<std::string, int32_t> addIndirectCallSite(RegCFunctionEmitState& state,
                                                    const RegInstruction& ins,
                                                    const char* suffix)
{
    std::string tableName;
    auto        nameIt = state.indirectSiteTableNames.find(suffix);
    if (nameIt == state.indirectSiteTableNames.end()) {
        tableName = "_mj2x_ct_" + std::to_string(state.indirectSiteTableIndex++);
        nameIt    = state.indirectSiteTableNames.emplace(suffix, tableName).first;
        state.indirectSiteTables.emplace(suffix, std::vector<RegCIndirectCallSite>{});
    }
    auto& table = state.indirectSiteTables[suffix];
    // 同 (owner, name, desc, invokeKind) 复用同一条目：同一目标的多个调用点
    // 指向同一条目，缓存也只解析一次。
    const std::string owner = ins.method.owner.to_std_string();
    const std::string name  = ins.method.name.to_std_string();
    const std::string desc  = ins.method.desc.to_std_string();
    const int32_t     kind  = ins.method.invokeOpcode == Opcodes::INVOKESTATIC      ? 1
                              : ins.method.invokeOpcode == Opcodes::INVOKESPECIAL   ? 2
                                                                                    : 0;
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].owner == owner && table[i].name == name && table[i].desc == desc &&
            table[i].invokeKind == kind) {
            return std::make_pair(nameIt->second, static_cast<int32_t>(i));
        }
    }
    const int32_t index = static_cast<int32_t>(table.size());
    table.push_back(RegCIndirectCallSite{owner, name, desc, kind});
    return std::make_pair(nameIt->second, index);
}

void emitCachedCall(CSourceStream& out, RegCFunctionEmitState& state, const RegInstruction& ins,
                    bool staticCall, const ca::str::Utf8StringRef& cacheName,
                    const std::vector<ca::str::Utf8String>& expressions, bool ownerMethodOnly)
{
    const char               returnTag = descriptorReturnTag(ins.method.desc);
    const std::vector<char>  paramTags = descriptorParamTags(ins.method.desc);
    const bool               hasDest   = ins.hasDest;
    const std::string        destName  = hasDest ? regName(ins.dest.id) : std::string();
    const ca::str::Utf8String argsArg  = paramTags.empty() ? ca::str::Utf8String("NULL")
                                                          : ca::str::Utf8String("_mj2x_regc_va");
    // INVOKESPECIAL（super.m() / X.super.m() / 私有 / 未融合 <init>）：owner 声明
    // 解析 + CallNonvirtual 精确分派。虚分派会命中 receiver 实际类的覆写——
    // X.super.m() 的调用方自身覆写该方法时直接无限递归（java_corpus
    // InterfaceDefaultMethods regc 首曝：Greeter.super.greet() 虚分派回 main
    // 的 greet 覆写 → StackOverflowError）。
    const bool specialCall = !staticCall && ins.method.invokeOpcode == Opcodes::INVOKESPECIAL;

    out.block([&](CSourceStream& o) {
        if (!staticCall) {
            o.declare_var("jobject", "_mj2x_regc_rcv",
                          codegen::exprCast("jobject",
                                            codegen::exprParen(expressions[0].to_std_string())));
        }
        if (!paramTags.empty()) {
            o.declare_var("jvalue", ca::str::format_std("_mj2x_regc_va[{}]", paramTags.size()));
        }
        // 调用目标间接化：调用点只带 (站点表, 组内索引)，目标身份字符串移出
        // 调用点，由可分发运行时的 per-signature dispatcher 解析并缓存。
        if (ins.indirectCall) {
            const char*  suffix  = hasDest ? helperSuffix(ins.dest.type) : "void";
            const size_t argBase = staticCall ? 0 : 1;
            for (size_t i = 0; i < paramTags.size(); ++i) {
                o.assign(codegen::exprMember(
                             codegen::exprIndex("_mj2x_regc_va", ca::str::format_std("{}", i)),
                             jvalueSlotName(paramTags[i])),
                         std::string(jniValueCast(paramTags[i])) +
                             codegen::exprParen(expressions[argBase + i].to_std_string()));
            }
            const auto   site = addIndirectCallSite(state, ins, suffix);
            const std::string dispatchCall = codegen::exprCall(
                std::string("mj2x_regc_dispatch_") + suffix,
                {"_mj2x_regc_current_context()", site.first,
                 ca::str::format_std("{}", site.second),
                 staticCall ? "NULL" : "_mj2x_regc_rcv", argsArg.to_std_string()});
            const auto   emitDispatch = [&](CSourceStream& u) {
                if (hasDest) {
                    u.assign(destName,
                             jniResultCast(returnTag, ins.dest.type).to_std_string() +
                                 dispatchCall);
                }
                else {
                    u.emitLine(dispatchCall + ";");
                }
            };
            if (!staticCall) {
                // 与 cached 帮手同口径：receiver 为 NULL 抛 NPE。
                o.iff(codegen::exprBinop("==", "_mj2x_regc_rcv", "NULL"),
                      [&](CSourceStream& u) {
                          u.emitLine("_mj2x_regc_throw_null(\"instance method call\");");
                          if (hasDest) {
                              u.assign(destName, std::string(defaultValue(ins.dest.type)));
                          }
                      },
                      emitDispatch);
            }
            else {
                emitDispatch(o);
            }
            return;
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
                                   jniResultCast(returnTag, ins.dest.type).to_std_string() + callExpr);
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
                                   specialCall || ownerMethodOnly
                                       ? "_mj2x_regc_cached_owner_method"
                                       : "_mj2x_regc_cached_instance_method",
                                   {"&" + cacheName.to_std_string(), "_mj2x_regc_rcv"}));
                      const std::string callExpr =
                          specialCall
                              ? codegen::exprCall(
                                    std::string("(*_mj2x_regc_env)->CallNonvirtual") +
                                        jniCallFamily(returnTag) + "MethodA",
                                    // JNI 参数序：obj 在前、clazz 在后（Call* 在前
                                    // 加 receiver，CallNonvirtual* 在 obj 后插
                                    // jclass）。clazz/obj 对调会把 jclass 当
                                    // receiver → ICCE。
                                    {"_mj2x_regc_env", "_mj2x_regc_rcv",
                                     codegen::exprMember(cacheName.to_std_string(), "ownerClass"),
                                     "_mj2x_regc_m", argsArg.to_std_string()})
                              : codegen::exprCall(
                                    std::string("(*_mj2x_regc_env)->Call") +
                                        jniCallFamily(returnTag) + "MethodA",
                                    {"_mj2x_regc_env", "_mj2x_regc_rcv", "_mj2x_regc_m",
                                     argsArg.to_std_string()});
                      const std::string methodGuard =
                          specialCall
                              ? codegen::exprBinop(
                                    "&&",
                                    codegen::exprBinop("!=", "_mj2x_regc_m", "NULL"),
                                    codegen::exprBinop(
                                        "!=",
                                        codegen::exprMember(cacheName.to_std_string(), "ownerClass"),
                                        "NULL"))
                              : codegen::exprBinop("!=", "_mj2x_regc_m", "NULL");
                      u.iff(methodGuard,
                            [&](CSourceStream& v) {
                                if (hasDest) {
                                    v.assign(destName,
                                             jniResultCast(returnTag, ins.dest.type).to_std_string() +
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

/// 发射「分配 + 构造」融合调用：JNI NewObjectA 在分配的同时调用 <init>
/// 构造器（1 个 JNI），替代 AllocObject + <init> 直调/解析（2 个 JNI）。
/// 类（FindClass 缓存）与 <init> methodID（owner 解析缓存）都只解析一次，
/// 之后每迭代 0 JNI 解析。要求：Allocate 与 <init> 同块紧邻、receiver 是
/// 分配结果、参数无 native 状态（保守不融合复杂物化场景）。
void emitAllocateWithInit(CSourceStream& out, RegCFunctionEmitState& state,
                          const RegInstruction& alloc, const RegInstruction& initCall,
                          const ca::str::Utf8StringRef& allocCacheName,
                          const std::vector<ca::str::Utf8String>& expressions)
{
    const std::vector<char>    paramTags = descriptorParamTags(initCall.method.desc);
    const ca::str::Utf8StringRef initCacheName = addInstanceMethodCache(state, initCall);
    const std::string        destName      = regName(alloc.dest.id);
    const ca::str::Utf8String argsArg = paramTags.empty() ? ca::str::Utf8String("NULL")
                                                         : ca::str::Utf8String("_mj2x_regc_va");
    out.block([&](CSourceStream& o) {
        if (!paramTags.empty()) {
            o.declare_var("jvalue", ca::str::format_std("_mj2x_regc_va[{}]", paramTags.size()));
        }
        o.declare_var("jclass", "_mj2x_regc_cls",
                      codegen::exprCall("_mj2x_regc_cached_class",
                                        {"&" + allocCacheName.to_std_string()}));
        o.declare_var("jmethodID", "_mj2x_regc_m",
                      codegen::exprCall("_mj2x_regc_cached_owner_method",
                                        {"&" + initCacheName.to_std_string(),
                                         "_mj2x_regc_cls"}));
        for (size_t i = 0; i < paramTags.size(); ++i) {
            o.assign(codegen::exprMember(codegen::exprIndex("_mj2x_regc_va",
                                                            ca::str::format_std("{}", i)),
                                         jvalueSlotName(paramTags[i])),
                     std::string(jniValueCast(paramTags[i])) +
                         codegen::exprParen(expressions[i + 1].to_std_string()));
        }
        const std::string newExpr = codegen::exprCall(
            "(*_mj2x_regc_env)->NewObjectA",
            {"_mj2x_regc_env", "_mj2x_regc_cls", "_mj2x_regc_m", argsArg.to_std_string()});
        o.iff(codegen::exprBinop(
                  "&&", codegen::exprBinop("!=", "_mj2x_regc_cls", "NULL"),
                  codegen::exprBinop("!=", "_mj2x_regc_m", "NULL")),
              [&](CSourceStream& u) {
                  u.assign(destName,
                           jniResultCast(descriptorReturnTag(initCall.method.desc),
                                                      alloc.dest.type).to_std_string() +
                               newExpr);
              },
              [&](CSourceStream& u) { u.assign(destName, "NULL"); });
    });
}

/// 判定 Allocate 指令与紧邻 <init> 调用是否可融合为 NewObjectA：同块紧邻由
/// 调用方保证（索引 i 与 i+1）；这里检查指令语义条件——对象分配（非数组）、
/// dest 无 native 状态（Random/StringBuilder 等不走 JNI 分配）、<init> 构造
/// 调用且 receiver 是分配结果、参数无 native 状态/物化需求。
bool canFuseAllocateInit(const RegCFunctionEmitState& state, const RegInstruction& alloc,
                         const RegInstruction& call)
{
    if (state.cachePool == nullptr) return false;   // 无缓存池（单测环境）走旧路径
    if (alloc.allocationKind != SSAAllocationKind::Object) return false;
    // native 状态分配（Random/StringBuilder/StringBuffer）不走 JNI 分配路径，
    // 融合会破坏其 native 状态计划（登记发生在 Allocate 发射时，此处预判）。
    if (alloc.typeDesc == "java/util/Random") return false;
    if (alloc.typeDesc == "java/lang/StringBuilder" || alloc.typeDesc == "java/lang/StringBuffer") {
        return false;
    }
    // POD struct 降级：这类分配根本不产生 jobject（走 C struct 声明），融合成
    // NewObjectA 会让 Allocate 分支永远拿不到它。不排除的话降级覆盖面为零。
    if (!state.podStructRoots.empty() && isPodClassOwner(state.classIndex, alloc.typeDesc)) {
        return false;
    }
    if (state.nativeObjects.count(alloc.dest.id.value) != 0) return false;
    if (call.kind != RegInstructionKind::Call) return false;
    if (call.method.name != "<init>") return false;
    if (call.method.invokeOpcode != Opcodes::INVOKESPECIAL) return false;
    if (call.args.empty()) return false;
    const RegOperand& rcv = call.args[0];
    if (rcv.kind != RegOperandKind::Register) return false;
    if (rcv.reg.value != alloc.dest.id.value) return false;
    if (call.args.size() < 1) return false;
    for (size_t i = 1; i < call.args.size(); ++i) {
        if (nativeObjectFor(state, call.args[i]) != nullptr) return false;
        // 参数引用分配结果自身（如 dup 后把对象传给构造器）时禁止融合：
        // NewObjectA 前参数求值会读到未初始化的分配槽（原语义为分配对象）。
        if (call.args[i].kind == RegOperandKind::Register &&
            call.args[i].reg.value == alloc.dest.id.value) {
            return false;
        }
    }
    return true;
}

/// 发射动态实例调用的 resolved native dispatch：按 receiver 实际类解析 jmethodID，
/// 若该调用点解析出的实现是已转换方法则直接调用其 regc_* 函数体（跳过 JNI
/// 分派），否则回退 CallXxxMethodA。站点级缓存持有 receiver 类 global ref。
/// 检查链（快路径优先）：implClass 的 IsInstanceOf（1 JNI 命中已转换实现类，
/// 省 GetObjectClass + IsSameObject）→ implClass1 → GetObjectClass + 双槽
/// receiverClass 精确类（未转换实现场景兜底）→ refresh。
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
                   s.emitLine("jmethodID _mj2x_regc_m = NULL;");
                    if (!paramTags.empty()) {
                        s.emitLine(ca::str::format_std("jvalue _mj2x_regc_va[{}];",
                                                       paramTags.size()));
                    }
                    // 检查链（快路径优先，GetObjectClass 只在未命中时执行）：
                    // 1) implClass 的 IsInstanceOf —— receiver 是已转换实现类（或其
                    //    未覆写子类）时 1 个 JNI 直接命中（省 GetObjectClass +
                    //    IsSameObject；implClass 仅在解析到已转换实现时非 NULL，
                    //    fn 为 NULL 时回退 CallXxxMethodA 虚分派保语义）；
                    // 2) implClass1 同理（双实现交替 receiver 场景）；
                    // 3) 仍未命中：GetObjectClass 取 receiver 实际类，先查双槽
                    //    receiverClass 精确类（未转换实现场景兜底，成本与旧形态
                    //    相同不退化），最后才 refresh。
                    s.iff(codegen::exprBinop(
                              "&&",
                              codegen::exprBinop("!=",
                                                 codegen::exprMember(site, "implClass"),
                                                 "NULL"),
                              shortCircuitRefCheck("(*_mj2x_regc_env)->IsInstanceOf",
                                                   "_mj2x_regc_rcv",
                                                   codegen::exprMember(site, "implClass"))),
                          [&](CSourceStream& u) {
                              u.emitLine("_mj2x_regc_m = " +
                                         codegen::exprMember(site, "method") + ";");
                          },
                          [&](CSourceStream& t2) {
                              t2.iff(codegen::exprBinop(
                                         "&&",
                                         codegen::exprBinop("!=",
                                                            codegen::exprMember(site,
                                                                                "implClass1"),
                                                            "NULL"),
                                         shortCircuitRefCheck(
                                             "(*_mj2x_regc_env)->IsInstanceOf",
                                             "_mj2x_regc_rcv",
                                             codegen::exprMember(site, "implClass1"))),
                                     [&](CSourceStream& u) {
                                         u.emitLine("_mj2x_regc_m = " +
                                                    codegen::exprMember(site, "method1") + ";");
                                     },
                                     [&](CSourceStream& t3) {
                                         // 未命中快路径：取 receiver 类走精确类检查
                                         // （双槽）与 refresh 兜底。
                                         t3.emitLine(
                                             "jclass _mj2x_regc_rc = " +
                                             codegen::exprCall("(*_mj2x_regc_env)"
                                                               "->GetObjectClass",
                                                               {"_mj2x_regc_env",
                                                                "_mj2x_regc_rcv"}) +
                                             ";");
                                         t3.iff(codegen::exprBinop("!=", "_mj2x_regc_rc",
                                                                  "NULL"),
                                                [&](CSourceStream& t4) {
                                                    t4.iff(codegen::exprBinop(
                                                               "&&",
                                                               codegen::exprBinop(
                                                                   "!=",
                                                                   codegen::exprMember(
                                                                       site,
                                                                       "receiverClass"),
                                                                   "NULL"),
                                                               shortCircuitRefCheck(
                                                                   "(*_mj2x_regc_env)"
                                                                   "->IsSameObject",
                                                                   codegen::exprMember(
                                                                       site,
                                                                       "receiverClass"),
                                                                   "_mj2x_regc_rc")),
                                                           [&](CSourceStream& u) {
                                                               u.emitLine(
                                                                   "_mj2x_regc_m = " +
                                                                   codegen::exprMember(
                                                                       site, "method") +
                                                                   ";");
                                                           },
                                                           [&](CSourceStream& t5) {
                                                               t5.iff(codegen::exprBinop(
                                                                          "&&",
                                                                          codegen::exprBinop(
                                                                              "!=",
                                                                              codegen::exprMember(
                                                                                  site,
                                                                                  "receiverClass1"),
                                                                              "NULL"),
                                                                          shortCircuitRefCheck(
                                                                              "(*_mj2x_"
                                                                              "regc_env)"
                                                                              "->IsSameObject",
                                                                              codegen::exprMember(
                                                                                  site,
                                                                                  "receiverClass1"),
                                                                              "_mj2x_"
                                                                              "regc_rc")),
                                                                      [&](CSourceStream& u) {
                                                                          u.emitLine(
                                                                              "_mj2x_"
                                                                              "regc_m = " +
                                                                              codegen::exprMember(
                                                                                  site,
                                                                                  "method1") +
                                                                              ";");
                                                                      },
                                                                      [&](CSourceStream& u) {
                                                                          u.emitLine(
                                                                              "_mj2x_regc_m = " +
                                                                              codegen::exprCall(
                                                                                  "_mj2x_"
                                                                                  "regc_"
                                                                                  "resolved_call_"
                                                                                  "site_refresh",
                                                                                  {"&" + site,
                                                                                   "_mj2x_"
                                                                                   "regc_rcv",
                                                                                   "_mj2x_"
                                                                                   "regc_rc",
                                                                                   cStringLiteral(
                                                                                       ins
                                                                                           .method
                                                                                           .name),
                                                                                   cStringLiteral(
                                                                                       ins
                                                                                           .method
                                                                                           .desc)}) +
                                                                              ";");
                                                                      });
                                                           });
                                                    if (!omitDeleteLocalRef) {
                                                        t4.emitLine(
                                                            codegen::exprCall(
                                                                "(*_mj2x_regc_env)"
                                                                "->DeleteLocalRef",
                                                                {"_mj2x_regc_env",
                                                                 "_mj2x_regc_rc"}) +
                                                            ";");
                                                    }
                                                });
                                     });
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

ca::str::Utf8StringRef addStaticFieldCache(RegCFunctionEmitState& state,
                                           const RegInstruction&  ins)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->staticFieldCache(ins.field.owner, ins.field.name, ins.field.desc);
    }

    const std::string key = cacheKey(ins.field.owner, ins.field.name, ins.field.desc);
    auto              it  = state.staticFieldCacheNames.find(key);
    if (it != state.staticFieldCacheNames.end()) return it->second.ref();

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
    return state.staticFieldCacheNames.emplace(key, cacheName.clone()).first->second.ref();
}

ca::str::Utf8StringRef addInstanceFieldCache(RegCFunctionEmitState& state,
                                             const RegInstruction&  ins)
{
    if (state.cachePool == nullptr) return ca::str::Utf8StringRef{};
    return state.cachePool->instanceFieldCache(ins.field.owner, ins.field.name, ins.field.desc);
}

ca::str::Utf8StringRef addClassCache(RegCFunctionEmitState& state, const ca::str::Utf8StringRef& desc)
{
    if (state.cachePool != nullptr) {
        return state.cachePool->classCache(desc);
    }

    const std::string descKey = desc.to_std_string();
    auto              it      = state.classCacheNames.find(descKey);
    if (it != state.classCacheNames.end()) return it->second.ref();

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
    return state.classCacheNames.emplace(descKey, cacheName.clone()).first->second.ref();
}


}   // namespace mj2x
