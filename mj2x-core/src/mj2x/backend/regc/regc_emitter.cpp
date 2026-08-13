#include "regc_emitter_internal.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mj2x/codegen/c_expr.hpp"
#include "mj2x/codegen/c_helpers.hpp"
#include "mj2x/codegen/c_source.hpp"
#include "mj2x/codegen/runtime_protection.hpp"
#include "libca/str/format.hpp"
#include "mj2x/backend/regc/regc_intrinsic_registry.hpp"
#include "mj2x/core/compiler_index.hpp"
#include "mj2x/mir/register_ir/reg_dump.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;

namespace {

// cStringLiteral is shared with StackC via codegen::cStringLiteral; alias it
// here so the existing call sites stay one-line.
using codegen::cStringLiteral;
// 源码拼接统一走 CSourceStream（Utf8 家族三层模型的排版层）：局部片段构建器与
// emit 路径的 sink 都用它，其 append 重载覆盖 Utf8String/Utf8StringRef/字面量/
// string_view，消除了 Utf8StringBuilder 无法直接接 std::string 的缺口。
using codegen::CSourceStream;

/// @brief 纯计算 intrinsic 判定（issue #778 P2 异常检查稀疏化）。
///
/// RuntimeFunction emitKind 的 intrinsic（位操作/Math 等）在 emitIntrinsicCall
/// 兜底分支发射为纯 C 表达式调用，不经过 JNI、不可能抛异常；SSA 阶段无法区分
/// intrinsic 与普通调用，统一标 mayThrow，导致热循环每轮都做 ExceptionCheck。
/// 这里在发射异常分发前复判一次：匹配成功且为 RuntimeFunction 时省略检查
/// （与 emitIntrinsicCall 的发射行为一致——该分支无 native-state 回退）。
bool isPureRuntimeIntrinsic(const RegInstruction& ins)
{
    if (ins.kind != RegInstructionKind::Call) return false;
    const RegCIntrinsicDecision decision = RegCIntrinsicRegistry::builtins().matchCall(ins);
    return decision.kind == RegCIntrinsicDecisionKind::Emit &&
           decision.emitKind == RegCIntrinsicEmitKind::RuntimeFunction;
}

/// Object.getClass() intrinsic（发射为 JNI GetObjectClass）本身不抛 Java 异常，
/// NULL receiver 的 NPE 已在发射点显式处理。无异常区域的方法可省略调用后
/// ExceptionCheck（有异常区域时保守保留，避免 NPE 挂起后错过 catch 分发）。
bool isObjectGetClass(const RegInstruction& ins)
{
    if (ins.kind != RegInstructionKind::Call) return false;
    const RegCIntrinsicDecision decision = RegCIntrinsicRegistry::builtins().matchCall(ins);
    return decision.kind == RegCIntrinsicDecisionKind::Emit &&
           decision.emitKind == RegCIntrinsicEmitKind::ObjectGetClass;
}

/// IR 级"方法体是否产 JNI local ref"判定：PushLocalFrame 帧发射的补充判定
/// （文本级 bodyProducesLocalRefs 不识别 resolved 调用产生的 GetObjectClass
/// ref）与 resolved 调用省略 DeleteLocalRef 的门槛。规则：
/// - Call（非纯计算 intrinsic）：返回 ref 或经物化/helper 产 ref → true
/// - Allocate：true
/// - LoadField/StoreField 的 desc 以 'L'/'[' 开头（引用字段）：true
/// - ArrayLoad/ArrayStore 的 elementType 为 REF：true
/// - Intrinsic 指令（array.length/monitor/cmp.nan）纯计算不产 ref；string
///   literal 物化走 LDC 表达式路径（tag 前缀），不在指令层面 → false
bool fnProducesLocalRefsIR(const RegFunction& fn)
{
    for (const auto& block : fn.blocks) {
        for (const auto& ins : block->instructions) {
            switch (ins.kind) {
                case RegInstructionKind::Call:
                    if (!isPureRuntimeIntrinsic(ins)) return true;
                    break;
                case RegInstructionKind::Allocate: return true;
                case RegInstructionKind::LoadField:
                case RegInstructionKind::StoreField: {
                    const char first =
                        ins.field.desc.is_empty() ? '\0' : (char)ins.field.desc.data()[0];
                    if (first == 'L' || first == '[') return true;
                    break;
                }
                case RegInstructionKind::ArrayLoad:
                case RegInstructionKind::ArrayStore:
                    if (ins.elementType == SSAType::REF) return true;
                    break;
                default: break;
            }
        }
    }
    return false;
}

}   // namespace

void emitRegCHelperPrelude(ca::str::Utf8StringBuilder& out)
{
    emitHelperPrelude(out);
}

RegCEmitter::RegCEmitter(RegCCachePool* cachePool, const codegen::CodeOptions& options,
                         std::vector<RegFunction::InvokeDynamicSite>* fileIndySites,
                         size_t indySiteBase, CompilerIndex* classIndex,
                         const std::unordered_map<std::string, std::string>* convertedFunctions,
                         const std::unordered_set<std::string>* indyFunctions)
    : cachePool_(cachePool)
    , options_(options)
    , fileIndySites_(fileIndySites)
    , indySiteBase_(indySiteBase)
    , classIndex_(classIndex)
    , convertedFunctions_(convertedFunctions)
    , indyFunctions_(indyFunctions)
{}

std::string RegCEmitter::functionNameFor(const SSAFunctionSymbol& symbol)
{
    return functionName(symbol);
}

RegCEmitResult RegCEmitter::emitFunction(const RegFunction& fn)
{
    RegCEmitResult result;
    result.functionName = functionName(fn.symbol);
    if (!fn.entry) {
        result.reason = "RegC requires an entry block";
        return result;
    }
    if (fileIndySites_ != nullptr && !fn.indySites.empty()) {
        fileIndySites_->insert(fileIndySites_->end(), fn.indySites.begin(), fn.indySites.end());
    }

    std::string validationReason;
    if (!validateRegIRNoStackSlots(fn, validationReason)) {
        result.reason = validationReason;
        return result;
    }

    std::map<uint32_t, SSAType> regs;
    if (!collectRegisters(fn, regs, result.reason)) return result;
    const RegCStringBuilderPlan stringBuilderPlan      = analyzeStringBuilders(fn);
    const std::set<uint32_t>    nativeWrapperRegisters = analyzeNativeWrappers(fn);

    RegCFunctionEmitState emitState;
    emitState.functionName                       = result.functionName;
    emitState.symbol                             = fn.symbol;
    emitState.cachePool                          = cachePool_;
    emitState.commentLevel                       = options_.commentLevel;
    emitState.traceCalls                         = options_.traceCallLog;
    emitState.classIndex                         = classIndex_;
    emitState.convertedFunctions                 = convertedFunctions_;
    emitState.indyFunctions                      = indyFunctions_;
    emitState.resolvedDispatch                   = options_.resolvedDispatch;
    emitState.hasLocalFrame                      = fnProducesLocalRefsIR(fn);
    emitState.nativeStringBuilderRegisters       = stringBuilderPlan.nativeRegisters;
    emitState.nativeStringBuilderOwners          = stringBuilderPlan.ownerRegisters;
    emitState.nativeStringBuilderOwnerByRegister = stringBuilderPlan.ownerByRegister;
    emitState.nativeWrapperRegisters             = nativeWrapperRegisters;
    for (uint32_t id : stringBuilderPlan.nativeRegisters) {
        const auto kindIt = stringBuilderPlan.kindByRegister.find(id);
        if (kindIt == stringBuilderPlan.kindByRegister.end()) {
            result.reason = "mutable string plan is missing register kind";
            return result;
        }
        emitState.nativeObjects.emplace(id,
                                        RegCNativeObjectState{kindIt->second, regName(RegId{id})});
    }
    int                dispatchIndex = 0;
    int                edgeTempIndex = 0;
    CSourceStream body;
    body.indent();
    for (const auto& block : fn.blocks) {
        auto activeRegions = activeExceptionRegions(fn, block.get());
        if (options_.commentLevel >= RegCCommentLevel::Summary) {
            std::string header = ca::str::format_std("block_b{}: ", block->id.value);
            if (block.get() == fn.entry) {
                header += "entry";
            }
            else {
                const RegExceptionRegion* handlerRegion = nullptr;
                for (const auto& region : fn.exceptionRegions) {
                    if (region.handler == block.get()) {
                        handlerRegion = &region;
                        break;
                    }
                }
                if (handlerRegion != nullptr) {
                    header += "exception handler of ";
                    header += codegen::escapeCommentIdent(
                        ca::str::Utf8StringRef::from_string_view(handlerRegion->caughtType));
                }
                else {
                    header += "block";
                }
            }
            body.comment(header);
        }
        body.label_(ca::str::format_std("block_b{}", block->id.value));
        for (const auto& ins : block->instructions) {
            if (options_.commentLevel >= RegCCommentLevel::Detailed) {
                body.comment(regInstructionLine(ins).to_std_string());
            }
            if (!emitInstruction(body, ins, emitState, fn, indySiteBase_, result.reason)) return result;
            // 无异常区域（方法无 try/catch）时省略调用后 ExceptionCheck：pending
            // 异常直接泄漏到 JNI 边界由 JVM 处理（与 Java 语义及 os30 一致），省
            // 每次调用的 check 开销；有异常区域时保留（分发到 handler 必需）。
            if (ins.effect.mayThrow && !isPureRuntimeIntrinsic(ins) &&
                !activeRegions.empty()) {
                if (!emitExceptionDispatch(body,
                                           activeRegions,
                                           block.get(),
                                           fn.symbol.returnType,
                                           emitState,
                                           dispatchIndex,
                                           edgeTempIndex,
                                           result.reason))
                    return result;
            }
        }
        if (options_.commentLevel >= RegCCommentLevel::Detailed) {
            const ca::str::Utf8String termLines = regTerminatorLines(*block);
            std::string_view          sv(termLines);
            // regTerminatorLines 每行以 '\n' 结尾；逐行加 "// " 前缀作为注释，避免裸 IR 文本进入 C 流。
            size_t pos = 0;
            while (pos < sv.size()) {
                size_t nl = sv.find('\n', pos);
                if (nl == std::string_view::npos) nl = sv.size();
                body.comment(sv.substr(pos, nl - pos));
                pos = nl + 1;
            }
        }
        if (!emitTerminator(body,
                            block->terminator,
                            block.get(),
                            fn.symbol.returnType,
                            activeRegions,
                            emitState,
                            dispatchIndex,
                            edgeTempIndex,
                            result.reason)) {
            return result;
        }
        body.emitBlankLine();
    }
    body.dedent();

    CSourceStream out;
    for (const auto& declaration : emitState.staticMethodCacheDeclarations) {
        out.append(declaration);
    }
    for (const auto& declaration : emitState.staticFieldCacheDeclarations) {
        out.append(declaration);
    }
    for (const auto& declaration : emitState.classCacheDeclarations) {
        out.append(declaration);
    }
    if (!emitState.staticMethodCacheDeclarations.empty() ||
        !emitState.staticFieldCacheDeclarations.empty() ||
        !emitState.classCacheDeclarations.empty())
        out.append("\n");

    if (options_.commentLevel >= RegCCommentLevel::Summary) {
        out.append("// RegC method: ");
        out.append(codegen::escapeCommentIdent(
            ca::str::Utf8StringRef::from_string_view(fn.symbol.owner)));
        out.append(".");
        out.append(codegen::escapeCommentIdent(
            ca::str::Utf8StringRef::from_string_view(fn.symbol.name)));
        out.append(codegen::escapeCommentIdent(
            ca::str::Utf8StringRef::from_string_view(fn.symbol.desc)));
        out.append("\n");
    }

    out.append("static ");
    out.append(cType(fn.symbol.returnType));
    out.append(" ");
    out.append(result.functionName);
    out.append("(");
    for (size_t i = 0; i < fn.parameters.size(); ++i) {
        if (i) out.append(", ");
        out.append(cType(fn.parameters[i].type));
        out.append(" ");
        {
            const std::string paramName = regName(fn.parameters[i].id);
            out.append(paramName.data(), paramName.size());
        }
    }
    out.append(") {\n");
    out.indent();

    if (fn.symbol.returnType != SSAType::VOID) {
        out.declare_var(cType(fn.symbol.returnType), "_mj2x_regc_result",
                        defaultValue(fn.symbol.returnType));
    }

    // 反侦测检查点注入（--runtime-protection）：FAKE 命中时返回默认值走收尾。
    // 必须放在 _mj2x_regc_result 声明之后（fakeReturn 引用它）。
    if (options_.runtimeProtection.enabled()) {
        std::string fakeReturn = "goto _mj2x_regc_cleanup;";
        if (fn.symbol.returnType != SSAType::VOID) {
            fakeReturn = std::string("_mj2x_regc_result = ") + defaultValue(fn.symbol.returnType) +
                         "; goto _mj2x_regc_cleanup;";
        }
        codegen::emitRuntimeProtectionCheckpoint(out, options_.runtimeProtection,
                                                 std::string("mj2x_rt_cp_") + result.functionName,
                                                 fakeReturn);
    }

    for (const auto& [id, type] : regs) {
        bool isParameter = false;
        for (const auto& param : fn.parameters) {
            if (param.id.value == id) {
                isParameter = true;
                break;
            }
        }
        if (!isParameter) {
            out.declare_var(cType(type), regName(RegId{id}), "0");
        }
    }
    // Declare edge-copy temporaries based on the types actually captured during emit
    // (emitState.edgeTempTypes). This mirrors the emit path exactly, including the
    // exception-dispatch re-emission that a static CFG walk (collectEdgeCopyTempTypes)
    // would undercount when a protected block has multiple mayThrow instructions.
    for (size_t i = 0; i < emitState.edgeTempTypes.size(); ++i) {
        out.declare_var(cType(emitState.edgeTempTypes[i]), ca::str::format_std("edge_tmp_{}", i),
                        "0");
    }
    for (uint32_t owner : emitState.nativeStringBuilderOwners) {
        const auto objectIt = emitState.nativeObjects.find(owner);
        if (objectIt == emitState.nativeObjects.end()) {
            result.reason = "mutable string owner is missing native object state";
            return result;
        }
        out.declare_var(std::string(nativeObjectType(objectIt->second.kind)) + "*",
                        stringBuilderOwnerName(RegId{owner}), "NULL");
        out.declare_var("mj2x_ref", stringBuilderMaterializedName(RegId{owner}), "NULL");
    }
    if (!regs.empty()) out.emitBlankLine();

    // R6 前置：JNI local ref 引用管理。RegC 每个 native 方法在单一 JNI 帧内运行，
    // 产 object 的 helper（call_ref / class_literal / alloc / ref-field load 等）返回的
    // local ref 全程累积不释放，反射密集方法会撑爆 JNI 默认保证的 16 槽 local ref 表。
    // 用 PushLocalFrame(入口) / PopLocalFrame(单出口，promote 返回值) 批量释放，语义等价
    // StackC 的 trackLocalRef/clear，但零 C 数组、无 c2j stack anchor。缓存 class/method
    // 走 NewGlobalRef，不受帧影响。纯标量方法（无产 ref 操作）不发射，保留「零 JNI」形态。
    const ca::str::Utf8String bodySource = body.build();
    const bool                needsLocalFrame =
        bodyProducesLocalRefs(bodySource) || emitState.hasLocalFrame;
    if (needsLocalFrame) {
        out.iff("(*_mj2x_regc_env)->PushLocalFrame(_mj2x_regc_env, 16) != 0",
                [&](CSourceStream& u) {
                    if (fn.symbol.returnType == SSAType::VOID) u.ret();
                    else u.ret("_mj2x_regc_result");
                });
    }
    out.goto_(ca::str::format_std("block_b{}", fn.entry->id.value));
    out.emitBlankLine();
    out.emitRaw(bodySource.to_std_string());

    if (options_.commentLevel >= RegCCommentLevel::Summary) {
        out.comment("_mj2x_regc_cleanup: native object teardown + local frame pop");
    }
    out.label_("_mj2x_regc_cleanup");
    for (uint32_t owner : emitState.nativeStringBuilderOwners) {
        const auto  objectIt = emitState.nativeObjects.find(owner);
        const char* destroySymbol =
            objectIt != emitState.nativeObjects.end() &&
                    objectIt->second.kind == RegCIntrinsicNativeObjectKind::StringBuffer
                ? "mj2x_string_buffer_destroy"
                : "mj2x_string_builder_destroy";
        out.call(destroySymbol, {stringBuilderOwnerName(RegId{owner}).to_std_string()});
    }
    // PopLocalFrame 释放本帧全部 local ref；REF 返回值通过 result 参数 promote 到父帧，
    // 非 REF/VOID 传 NULL。PopLocalFrame 对 pending exception 安全（返回值可能为 NULL）。
    if (needsLocalFrame) {
        if (fn.symbol.returnType == SSAType::REF) {
            out.assign("_mj2x_regc_result",
                       "(mj2x_ref)(*_mj2x_regc_env)->PopLocalFrame("
                       "_mj2x_regc_env, (jobject)_mj2x_regc_result)");
        }
        else {
            out.emitLine("(*_mj2x_regc_env)->PopLocalFrame(_mj2x_regc_env, NULL);");
        }
    }
    if (fn.symbol.returnType == SSAType::VOID) out.ret();
    else out.ret("_mj2x_regc_result");

    out.dedent();
    out.append("}\n");

    const std::string sourceCode = out.build().to_std_string();
    result.source = sourceCode;
    if (!validateRegCNoStackAnchors(result.source, result.reason)) {
        result.ok = false;
        return result;
    }
    result.ok = true;
    result.reason.clear();
    return result;
}

bool emitInvokeDynamic(CSourceStream& out, const RegInstruction& ins,
                       const RegFunction::InvokeDynamicSite& site, size_t globalSiteIndex,
                       std::string& reason)
{
    const ca::str::Utf8StringRef descRef = ca::str::Utf8StringRef::from_cstr(site.desc.c_str());
    auto                         argTypes = Type::getArgumentTypes(descRef);
    const int                    argCount = static_cast<int>(argTypes.size());
    if (argCount != static_cast<int>(ins.args.size())) {
        reason = "indy arg count mismatch";
        return false;
    }
    const std::string destName = ins.hasDest ? regName(ins.dest.id) : std::string();

    bool ok = true;
    out.block([&](CSourceStream& o) {
        o.declare_var("jclass", "_mj2x_indy_obj_cls",
                      "(jclass)_mj2x_regc_class_literal(_mj2x_regc_current_context(), "
                      "\"java/lang/Object\")");
        o.appendLineStart("jobjectArray _mj2x_indy_args = (jobjectArray)(*_mj2x_regc_env)"
                          "->NewObjectArray(_mj2x_regc_env, ")
            .append(ca::str::format_std("{}", argCount))
            .append(", _mj2x_indy_obj_cls, NULL);")
            .newLine();
        o.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
        for (int i = 0; i < argCount; ++i) {
            const ca::str::Utf8String expr = operandExpr(ins.args[i]);
            std::string               boxExpr;
            switch (ins.args[i].type) {
            case SSAType::REF:
                boxExpr = codegen::exprCast("jobject", codegen::exprParen(expr.to_std_string()));
                break;
            case SSAType::BOOL:
            case SSAType::I8:
            case SSAType::I16:
            case SSAType::CHAR:
            case SSAType::I32:
                boxExpr = codegen::exprCall("_mj2x_regc_box_i32",
                                            {codegen::exprCast(
                                                "int32_t",
                                                codegen::exprParen(expr.to_std_string()))});
                break;
            case SSAType::I64:
                boxExpr = codegen::exprCall("_mj2x_regc_box_i64",
                                            {codegen::exprCast(
                                                "int64_t",
                                                codegen::exprParen(expr.to_std_string()))});
                break;
            case SSAType::F32:
                boxExpr = codegen::exprCall("_mj2x_regc_box_f32",
                                            {codegen::exprCast(
                                                "float",
                                                codegen::exprParen(expr.to_std_string()))});
                break;
            case SSAType::F64:
                boxExpr = codegen::exprCall("_mj2x_regc_box_f64",
                                            {codegen::exprCast(
                                                "double",
                                                codegen::exprParen(expr.to_std_string()))});
                break;
            default:
                reason = "unsupported indy argument type";
                ok = false;
                return;
            }
            if (!ok) return;
            o.emitLine(codegen::exprCall("(*_mj2x_regc_env)->SetObjectArrayElement",
                                         {"_mj2x_regc_env", "_mj2x_indy_args",
                                          ca::str::format_std("{}", i), boxExpr}) +
                       ";");
        }
        if (!ok) return;
        o.appendLineStart("jobject _mj2x_indy_target = _mj2x_regc_indy_bsm_")
            .append(ca::str::format_std("{}", globalSiteIndex))
            .append("(_mj2x_regc_owner_class);")
            .newLine();
        o.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
        o.emitLine("if (_mj2x_indy_target == NULL) { goto _mj2x_regc_cleanup; }");
        o.emitLine("jclass _mj2x_indy_mh_cls = (jclass)_mj2x_regc_class_literal("
                   "_mj2x_regc_current_context(), \"java/lang/invoke/MethodHandle\");");
        o.emitLine("jmethodID _mj2x_indy_invoke_mid = (*_mj2x_regc_env)->GetMethodID("
                   "_mj2x_regc_env, _mj2x_indy_mh_cls, \"invokeWithArguments\", "
                   "\"([Ljava/lang/Object;)Ljava/lang/Object;\");");
        o.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
        o.emitLine("jobject _mj2x_indy_result = (jobject)(*_mj2x_regc_env)->CallObjectMethod("
                   "_mj2x_regc_env, _mj2x_indy_target, _mj2x_indy_invoke_mid, _mj2x_indy_args);");
        o.emitLine("if (_mj2x_regc_exception_check()) { goto _mj2x_regc_cleanup; }");
        if (ins.hasDest) {
            std::string resultExpr;
            switch (ins.dest.type) {
            case SSAType::REF:
                resultExpr = codegen::exprCast("mj2x_ref", "_mj2x_indy_result");
                break;
            case SSAType::BOOL:
            case SSAType::I8:
            case SSAType::I16:
            case SSAType::CHAR:
            case SSAType::I32:
                resultExpr = codegen::exprCall("_mj2x_regc_unbox_i32", {"_mj2x_indy_result"});
                break;
            case SSAType::I64:
                resultExpr = codegen::exprCall("_mj2x_regc_unbox_i64", {"_mj2x_indy_result"});
                break;
            case SSAType::F32:
                resultExpr = codegen::exprCall("_mj2x_regc_unbox_f32", {"_mj2x_indy_result"});
                break;
            case SSAType::F64:
                resultExpr = codegen::exprCall("_mj2x_regc_unbox_f64", {"_mj2x_indy_result"});
                break;
            default:
                reason = "unsupported indy result type";
                ok = false;
                return;
            }
            if (!ok) return;
            o.assign(destName, resultExpr);
        }
        else {
            o.emitLine("(void)_mj2x_indy_result;");
        }
    });
    return ok;
}

bool validateRegCNoStackAnchors(const std::string& source, std::string& reason)
{
    const char* anchors[] = {
        "cstack[",
        "cstack",
        "stack[",
        "union jvalue",
        " sp ",
        "sp--",
        "sp++",
        "Cannot invoke",
        "cclasses[",
        "cmethods[",
    };
    for (const char* anchor : anchors) {
        if (source.find(anchor) != std::string::npos) {
            reason = std::string("RegC output contains stack-machine anchor: ") + anchor;
            return false;
        }
    }
    reason.clear();
    return true;
}

}   // namespace mj2x
