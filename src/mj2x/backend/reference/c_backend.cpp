#include "c_backend.hpp"
#include "mj2x/frontend/type_utils.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

// label 标识：LabelNode* 转 int（与 code_converter 的 labelKey 一致；见 dev_plan R1a.3）。
static int labelKey(jbytecode::core::Label* lbl) {
    return static_cast<int>(reinterpret_cast<intptr_t>(lbl));
}

// ============================================================
// JNI name mangling (javah-style)
// ============================================================

static std::string mangleClass(const std::string& internalName) {
    std::string result;
    result.reserve(internalName.size());
    for (char c : internalName) {
        if (c == '/' || c == '.')
            result += '_';
        else
            result += c;
    }
    return result;
}

static std::string mangleMethod(const std::string& className,
                                 const std::string& methodName,
                                 const std::string& methodDesc) {
    std::string result = "Java_";
    result += mangleClass(className);
    result += "_";
    result += mangleClass(methodName); // method names with underscores

    // Append parameter signature for overloaded methods
    // Find start/end of parameter section in desc
    auto paren = methodDesc.find(')');
    if (paren != std::string::npos && paren > 1) {
        result += "__";
        for (size_t i = 1; i < paren; i++) {
            char c = methodDesc[i];
            if (c == '/' || c == '_') {
                result += '_';
                // In JNI mangling, `_` in descriptors becomes `_1`
                result += '1';
            } else if (c == ';') {
                // skip
            } else if (c == '[') {
                result += "_3";
            } else if (c == 'L') {
                result += 'L';
            } else {
                result += c;
            }
        }
    }

    return result;
}

// ============================================================
// Type mapping helpers
// ============================================================

static const char* primitiveToJNIType(PrimitiveKind k) {
    switch (k) {
    case PrimitiveKind::BOOLEAN: return "jboolean";
    case PrimitiveKind::BYTE:    return "jbyte";
    case PrimitiveKind::CHAR:    return "jchar";
    case PrimitiveKind::SHORT:   return "jshort";
    case PrimitiveKind::INT:     return "jint";
    case PrimitiveKind::LONG:    return "jlong";
    case PrimitiveKind::FLOAT:   return "jfloat";
    case PrimitiveKind::DOUBLE:  return "jdouble";
    case PrimitiveKind::VOID:    return "void";
    default:                     return "jobject";
    }
}

static const char* primitiveToJNISignature(PrimitiveKind k) {
    switch (k) {
    case PrimitiveKind::BOOLEAN: return "Z";
    case PrimitiveKind::BYTE:    return "B";
    case PrimitiveKind::CHAR:    return "C";
    case PrimitiveKind::SHORT:   return "S";
    case PrimitiveKind::INT:     return "I";
    case PrimitiveKind::LONG:    return "J";
    case PrimitiveKind::FLOAT:   return "F";
    case PrimitiveKind::DOUBLE:  return "D";
    case PrimitiveKind::VOID:    return "V";
    default:                     return "Ljava/lang/Object;";
    }
}

static const char* primitiveToJNISuffix(PrimitiveKind k) {
    switch (k) {
    case PrimitiveKind::BOOLEAN: return "Boolean";
    case PrimitiveKind::BYTE:    return "Byte";
    case PrimitiveKind::CHAR:    return "Char";
    case PrimitiveKind::SHORT:   return "Short";
    case PrimitiveKind::INT:     return "Int";
    case PrimitiveKind::LONG:    return "Long";
    case PrimitiveKind::FLOAT:   return "Float";
    case PrimitiveKind::DOUBLE:  return "Double";
    default:                     return "Object";
    }
}

static const char* stackAccessor(PrimitiveKind k) {
    switch (k) {
    case PrimitiveKind::INT:     return ".i";
    case PrimitiveKind::LONG:    return ".j";
    case PrimitiveKind::FLOAT:   return ".f";
    case PrimitiveKind::DOUBLE:  return ".d";
    case PrimitiveKind::BOOLEAN: return ".z";
    case PrimitiveKind::BYTE:    return ".b";
    case PrimitiveKind::CHAR:    return ".c";
    case PrimitiveKind::SHORT:   return ".s";
    default:                     return ".l";
    }
}

// ============================================================
// Binary op → C operator
// ============================================================

static const char* binaryOpToC(BinaryOp op) {
    switch (op) {
    case BinaryOp::ADD: return "+";
    case BinaryOp::SUB: return "-";
    case BinaryOp::MUL: return "*";
    case BinaryOp::DIV: return "/";
    case BinaryOp::REM: return "%";
    case BinaryOp::SHL: return "<<";
    case BinaryOp::SHR: return ">>";
    case BinaryOp::USHR: return ">>"; // unsigned shift — same in C for unsigned
    case BinaryOp::AND: return "&";
    case BinaryOp::OR:  return "|";
    case BinaryOp::XOR: return "^";
    default: return "+";
    }
}

static const char* cmpOpToC(CmpOp op) {
    switch (op) {
    case CmpOp::EQ: return "==";
    case CmpOp::NE: return "!=";
    case CmpOp::LT: return "<";
    case CmpOp::GE: return ">=";
    case CmpOp::GT: return ">";
    case CmpOp::LE: return "<=";
    default: return "==";
    }
}

// ============================================================
// Impl
// ============================================================

struct CBackend::Impl {
    std::ostringstream header;
    std::ostringstream source;
    std::unordered_set<std::string> exportedFunctions;

    // Cache current method info for codegen
    std::string currentClassName;
    std::string currentMethodName;
    std::string currentMethodDesc;

    void emitStackInit(int maxStack);
    void emitLocalsInit(const std::string& desc, int access);

    // MIR node → C code
    void emitNode(const MIRNode* node, std::ostringstream& out);
    void emitConstant(const MIRConstant* node, std::ostringstream& out);
    void emitLocalLoad(const MIRLocalLoad* node, std::ostringstream& out);
    void emitLocalStore(const MIRLocalStore* node, std::ostringstream& out);
    void emitIinc(const MIRIinc* node, std::ostringstream& out);
    void emitBinaryOp(const MIRBinaryOp* node, std::ostringstream& out);
    void emitUnaryOp(const MIRUnaryOp* node, std::ostringstream& out);
    void emitBranch(const MIRBranch* node, std::ostringstream& out);
    void emitBranchZero(const MIRBranchZero* node, std::ostringstream& out);
    void emitUncondBranch(const MIRUncondBranch* node, std::ostringstream& out);
    void emitSwitch(const MIRSwitch* node, std::ostringstream& out);
    void emitReturn(const MIRReturn* node, std::ostringstream& out);
    void emitReturnValue(const MIRReturnValue* node, std::ostringstream& out);
    void emitNewObject(const MIRNewObject* node, std::ostringstream& out);
    void emitNewArray(const MIRNewArray* node, std::ostringstream& out);
    void emitFieldGet(const MIRFieldGet* node, std::ostringstream& out);
    void emitFieldPut(const MIRFieldPut* node, std::ostringstream& out);
    void emitStaticGet(const MIRStaticGet* node, std::ostringstream& out);
    void emitStaticPut(const MIRStaticPut* node, std::ostringstream& out);
    void emitInvoke(const MIRInvoke* node, std::ostringstream& out);
    void emitCheckCast(const MIRCheckCast* node, std::ostringstream& out);
    void emitInstanceOf(const MIRInstanceOf* node, std::ostringstream& out);
    void emitMonitor(const MIRMonitor* node, std::ostringstream& out);
    void emitThrow(const MIRThrow* node, std::ostringstream& out);
    void emitArrayLength(const MIRArrayLength* node, std::ostringstream& out);
    void emitArrayLoad(const MIRArrayLoad* node, std::ostringstream& out);
    void emitArrayStore(const MIRArrayStore* node, std::ostringstream& out);
};

CBackend::CBackend()
    : impl_(std::make_unique<Impl>())
{}

CBackend::~CBackend() = default;

// ============================================================
// Main compile method
// ============================================================

BackendResult CBackend::compileMethod(
    const std::string& className,
    CompilerMethod& method,
    const std::vector<Block*>& blocks)
{
    auto* mn = method.methodNode;
    impl_->currentClassName = className;
    auto& cls = impl_->currentClassName;

    impl_->currentMethodName = mn->name.to_std_string();
    impl_->currentMethodDesc = mn->desc.to_std_string();

    std::ostringstream funcOut;

    // JNI function signature
    bool isStatic = (mn->access & Opcodes::ACC_STATIC) != 0;
    auto retType = jvmDescToPrimitive(
        Type::getReturnType(mn->desc).getDescriptor().to_std_string());

    auto funcName = mangleMethod(cls, mn->name.to_std_string(), mn->desc.to_std_string());
    impl_->exportedFunctions.insert(funcName);

    funcOut << "JNIEXPORT " << primitiveToJNIType(retType) << " JNICALL\n";
    funcOut << funcName << "(JNIEnv* env, "
            << (isStatic ? "jclass clazz" : "jobject this");

    // Add method parameters
    auto desc = Type::getMethodType(mn->desc);
    auto argTypes = desc.getArgumentTypes();
    int slot = 0;
    for (const auto& argType : argTypes) {
        auto pk = jvmSortToPrimitive(argType.getSort());
        funcOut << ", " << primitiveToJNIType(pk) << " p" << slot;
        slot += primitiveStackSize(pk);
    }

    funcOut << ") {\n";

    // Stack and locals declaration
    int maxLocalsVal = mn->maxLocals > 0 ? mn->maxLocals : 1;
    funcOut << "    union jvalue stack[" << (mn->maxStack > 0 ? mn->maxStack : 1) << "];\n";
    funcOut << "    union jvalue locals[" << maxLocalsVal << "];\n";
    funcOut << "    int sp = -1;\n\n";

    // Param init
    slot = 0;
    if (!isStatic) {
        funcOut << "    locals[" << slot << "].l = this;\n";
        slot++;
    }
    int paramIdx = 0;
    for (const auto& argType : argTypes) {
        auto pk = jvmSortToPrimitive(argType.getSort());
        funcOut << "    locals[" << slot << "]." << (stackAccessor(pk) + 1)
                << " = p" << paramIdx << ";\n";
        slot += primitiveStackSize(pk);
        paramIdx++;
    }
    funcOut << "\n";

    // Emit blocks
    for (auto* block : blocks) {
        funcOut << "    /* B" << block->id << " */\n";

        for (auto& node : block->nodes) {
            impl_->emitNode(node.get(), funcOut);
        }
        funcOut << "\n";
    }

    funcOut << "}\n\n";
    impl_->source << funcOut.str();

    BackendResult result;
    result.kind = BackendResult::Kind::C_SOURCE;
    result.text = funcOut.str();
    return result;
}

void CBackend::injectIntoClass(
    CompilerClass& clazz,
    CompilerMethod& method,
    const BackendResult& result)
{
    // Mark method as native and clear bytecode
    auto* mn = method.methodNode;
    mn->access |= Opcodes::ACC_NATIVE;
    mn->instructions.clear();
    mn->maxStack = 0;
    mn->maxLocals = 0;
    clazz.modified = true;
}

void CBackend::finalize(const CompilerIndex& index) {
    // Build final source: header + all compiled function code
    std::ostringstream full;
    full << "/* Generated by mj2x CBackend */\n";
    full << "#include <jni.h>\n\n";
    full << impl_->source.str();

    headerCode_ = "/* Generated by mj2x CBackend */\n#include <jni.h>\n\n";
    sourceCode_ = full.str();

    // Write to file if output path is set
    if (!cOutputPath_.empty()) {
        std::ofstream ofs(cOutputPath_, std::ios::binary);
        if (ofs) {
            ofs << sourceCode_;
            printf("[CBackend] Written: %s (%zu bytes)\n",
                   cOutputPath_.c_str(), sourceCode_.size());
        } else {
            printf("[CBackend] ERROR: cannot write %s\n", cOutputPath_.c_str());
        }
    }
}

// ============================================================
// MIR Node → C code
// ============================================================

void CBackend::Impl::emitNode(const MIRNode* node, std::ostringstream& out) {
    switch (node->kind) {
    case MIRKind::LABEL:
        out << "label_" << labelKey(static_cast<const MIRLabel*>(node)->labelNode) << ":\n";
        break;
    case MIRKind::CONSTANT:
        emitConstant(static_cast<const MIRConstant*>(node), out);
        break;
    case MIRKind::LOCAL_LOAD:
        emitLocalLoad(static_cast<const MIRLocalLoad*>(node), out);
        break;
    case MIRKind::LOCAL_STORE:
        emitLocalStore(static_cast<const MIRLocalStore*>(node), out);
        break;
    case MIRKind::IINC:
        emitIinc(static_cast<const MIRIinc*>(node), out);
        break;
    case MIRKind::STACK_DUP:
    case MIRKind::STACK_DUP_X1:
        out << "    stack[sp + 1] = stack[sp]; sp++;\n";
        break;
    case MIRKind::STACK_SWAP: {
        out << "    {\n";
        out << "        union jvalue tmp = stack[sp];\n";
        out << "        stack[sp] = stack[sp - 1];\n";
        out << "        stack[sp - 1] = tmp;\n";
        out << "    }\n";
        break;
    }
    case MIRKind::STACK_DUP2:
        out << "    stack[sp + 1] = stack[sp - 1];\n";
        out << "    stack[sp + 2] = stack[sp];\n";
        out << "    sp += 2;\n";
        break;
    case MIRKind::STACK_DUP_X2:
        out << "    stack[sp + 1] = stack[sp];\n";
        out << "    stack[sp] = stack[sp - 1];\n";
        out << "    stack[sp - 1] = stack[sp + 1];\n";
        out << "    sp++;\n";
        break;
    case MIRKind::BINARY_OP:
        emitBinaryOp(static_cast<const MIRBinaryOp*>(node), out);
        break;
    case MIRKind::UNARY_OP:
        emitUnaryOp(static_cast<const MIRUnaryOp*>(node), out);
        break;
    case MIRKind::CAST:
        out << "    /* CAST */\n";
        break;
    case MIRKind::BRANCH:
        emitBranch(static_cast<const MIRBranch*>(node), out);
        break;
    case MIRKind::BRANCH_ZERO:
        emitBranchZero(static_cast<const MIRBranchZero*>(node), out);
        break;
    case MIRKind::UNCOND_BRANCH:
        emitUncondBranch(static_cast<const MIRUncondBranch*>(node), out);
        break;
    case MIRKind::SWITCH:
        emitSwitch(static_cast<const MIRSwitch*>(node), out);
        break;
    case MIRKind::RETURN:
        out << "    return;\n";
        break;
    case MIRKind::RETURN_VALUE:
        emitReturnValue(static_cast<const MIRReturnValue*>(node), out);
        break;
    case MIRKind::NEW_OBJECT:
        emitNewObject(static_cast<const MIRNewObject*>(node), out);
        break;
    case MIRKind::NEW_ARRAY:
        emitNewArray(static_cast<const MIRNewArray*>(node), out);
        break;
    case MIRKind::ARRAY_LENGTH:
        emitArrayLength(static_cast<const MIRArrayLength*>(node), out);
        break;
    case MIRKind::ARRAY_LOAD:
        emitArrayLoad(static_cast<const MIRArrayLoad*>(node), out);
        break;
    case MIRKind::ARRAY_STORE:
        emitArrayStore(static_cast<const MIRArrayStore*>(node), out);
        break;
    case MIRKind::FIELD_GET:
        emitFieldGet(static_cast<const MIRFieldGet*>(node), out);
        break;
    case MIRKind::FIELD_PUT:
        emitFieldPut(static_cast<const MIRFieldPut*>(node), out);
        break;
    case MIRKind::STATIC_GET:
        emitStaticGet(static_cast<const MIRStaticGet*>(node), out);
        break;
    case MIRKind::STATIC_PUT:
        emitStaticPut(static_cast<const MIRStaticPut*>(node), out);
        break;
    case MIRKind::INVOKE:
        emitInvoke(static_cast<const MIRInvoke*>(node), out);
        break;
    case MIRKind::CHECK_CAST:
        emitCheckCast(static_cast<const MIRCheckCast*>(node), out);
        break;
    case MIRKind::INSTANCE_OF:
        emitInstanceOf(static_cast<const MIRInstanceOf*>(node), out);
        break;
    case MIRKind::MONITOR_ENTER:
    case MIRKind::MONITOR_EXIT:
        emitMonitor(static_cast<const MIRMonitor*>(node), out);
        break;
    case MIRKind::THROW:
        emitThrow(static_cast<const MIRThrow*>(node), out);
        break;
    default:
        break;
    }
}

void CBackend::Impl::emitConstant(const MIRConstant* node, std::ostringstream& out) {
    const char* acc = stackAccessor(node->type);
    if (std::holds_alternative<int64_t>(node->value)) {
        auto v = std::get<int64_t>(node->value);
        if (node->type == PrimitiveKind::LONG)
            out << "    stack[++sp]" << acc << " = " << v << "LL;\n";
        else
            out << "    stack[++sp]" << acc << " = " << static_cast<int32_t>(v) << ";\n";
    } else if (std::holds_alternative<double>(node->value)) {
        out << "    stack[++sp]" << acc << " = " << std::get<double>(node->value) << ";\n";
    } else {
        out << "    stack[++sp]" << acc << " = NULL;\n";
    }
}

void CBackend::Impl::emitLocalLoad(const MIRLocalLoad* node, std::ostringstream& out) {
    out << "    stack[++sp]" << stackAccessor(node->type)
        << " = locals[" << node->varIndex << "]"
        << stackAccessor(node->type) << ";\n";
}

void CBackend::Impl::emitLocalStore(const MIRLocalStore* node, std::ostringstream& out) {
    out << "    locals[" << node->varIndex << "]"
        << stackAccessor(node->type)
        << " = stack[sp--]" << stackAccessor(node->type) << ";\n";
}

void CBackend::Impl::emitIinc(const MIRIinc* node, std::ostringstream& out) {
    out << "    locals[" << node->varIndex << "].i += " << node->increment << ";\n";
}

void CBackend::Impl::emitBinaryOp(const MIRBinaryOp* node, std::ostringstream& out) {
    const char* acc = stackAccessor(node->type);
    out << "    stack[sp - 1]" << acc << " = stack[sp - 1]" << acc
        << " " << binaryOpToC(node->op) << " stack[sp]" << acc << ";\n";
    out << "    sp--;\n";
}

void CBackend::Impl::emitUnaryOp(const MIRUnaryOp* node, std::ostringstream& out) {
    const char* acc = stackAccessor(node->type);
    out << "    stack[sp]" << acc << " = -stack[sp]" << acc << ";\n";
}

void CBackend::Impl::emitBranch(const MIRBranch* node, std::ostringstream& out) {
    const char* acc = stackAccessor(node->type);
    (void)acc;
    out << "    if (stack[sp - 1]" << acc << " " << cmpOpToC(node->op)
        << " stack[sp]" << acc << ") goto label_" << labelKey(node->targetLabel) << ";\n";
    out << "    sp -= 2;\n";
}

void CBackend::Impl::emitBranchZero(const MIRBranchZero* node, std::ostringstream& out) {
    out << "    if (stack[sp].i " << cmpOpToC(node->op)
        << " 0) goto label_" << labelKey(node->targetLabel) << ";\n";
    out << "    sp--;\n";
}

void CBackend::Impl::emitUncondBranch(const MIRUncondBranch* node, std::ostringstream& out) {
    out << "    goto label_" << labelKey(node->targetLabel) << ";\n";
}

void CBackend::Impl::emitSwitch(const MIRSwitch* node, std::ostringstream& out) {
    out << "    switch (stack[sp--].i) {\n";
    for (auto& c : node->cases) {
        out << "    case " << c.key << ": goto label_" << labelKey(c.targetLabel) << ";\n";
    }
    out << "    default: goto label_" << labelKey(node->defaultLabel) << ";\n";
    out << "    }\n";
}

void CBackend::Impl::emitReturnValue(const MIRReturnValue* node, std::ostringstream& out) {
    out << "    return stack[sp--]" << stackAccessor(node->type) << ";\n";
}

void CBackend::Impl::emitNewObject(const MIRNewObject* node, std::ostringstream& out) {
    out << "    {\n";
    out << "        jclass cls_" << (currentMethodName.size() > 8
            ? currentMethodName.substr(0, 8) : currentMethodName)
        << " = (*env)->FindClass(env, \"" << node->typeDesc << "\");\n";
    out << "        jmethodID mid = (*env)->GetMethodID(env, cls, \"<init>\", \"()V\");\n";
    out << "        stack[++sp].l = (*env)->NewObject(env, cls, mid);\n";
    out << "    }\n";
}

void CBackend::Impl::emitNewArray(const MIRNewArray* node, std::ostringstream& out) {
    out << "    stack[sp].l = (*env)->NewObjectArray(env, stack[sp].i, ";
    if (node->typeDesc.empty())
        out << "(*env)->FindClass(env, \"java/lang/Object\")";
    else
        out << "(*env)->FindClass(env, \"" << node->typeDesc << "\")";
    out << ", NULL);\n";
}

void CBackend::Impl::emitArrayLength(const MIRArrayLength* node, std::ostringstream& out) {
    (void)node;
    out << "    stack[sp].i = (*env)->GetArrayLength(env, stack[sp].l);\n";
}

void CBackend::Impl::emitArrayLoad(const MIRArrayLoad* node, std::ostringstream& out) {
    const char* suffix = primitiveToJNISuffix(node->componentType);
    out << "    stack[sp - 1]"
        << stackAccessor(node->componentType)
        << " = (*env)->Get" << suffix << "ArrayRegion(env, "
        << "stack[sp - 1].l, stack[sp].i, &stack[sp - 1]"
        << stackAccessor(node->componentType) << ");\n";
    out << "    sp--;\n";
}

void CBackend::Impl::emitArrayStore(const MIRArrayStore* node, std::ostringstream& out) {
    const char* suffix = primitiveToJNISuffix(node->componentType);
    out << "    (*env)->Set" << suffix << "ArrayRegion(env, "
        << "stack[sp - 2].l, stack[sp - 1].i, 1, &stack[sp]"
        << stackAccessor(node->componentType) << ");\n";
    out << "    sp -= 3;\n";
}

void CBackend::Impl::emitFieldGet(const MIRFieldGet* node, std::ostringstream& out) {
    auto pk = jvmDescToPrimitive(node->desc);
    const char* suffix = primitiveToJNISuffix(pk);
    const char* acc = stackAccessor(pk);
    out << "    stack[sp]" << acc << " = (*env)->Get" << suffix
        << "Field(env, stack[sp].l, NULL);\n"; // FIXME: need cached field ID
}

void CBackend::Impl::emitFieldPut(const MIRFieldPut* node, std::ostringstream& out) {
    auto pk = jvmDescToPrimitive(node->desc);
    const char* suffix = primitiveToJNISuffix(pk);
    const char* acc = stackAccessor(pk);
    out << "    (*env)->Set" << suffix
        << "Field(env, stack[sp - 1].l, NULL, stack[sp]" << acc << ");\n";
    out << "    sp -= 2;\n";
}

void CBackend::Impl::emitStaticGet(const MIRStaticGet* node, std::ostringstream& out) {
    auto pk = jvmDescToPrimitive(node->desc);
    const char* suffix = primitiveToJNISuffix(pk);
    const char* acc = stackAccessor(pk);
    out << "    stack[++sp]" << acc << " = (*env)->GetStatic" << suffix
        << "Field(env, NULL, NULL);\n";
}

void CBackend::Impl::emitStaticPut(const MIRStaticPut* node, std::ostringstream& out) {
    auto pk = jvmDescToPrimitive(node->desc);
    const char* suffix = primitiveToJNISuffix(pk);
    const char* acc = stackAccessor(pk);
    out << "    (*env)->SetStatic" << suffix
        << "Field(env, NULL, NULL, stack[sp]" << acc << ");\n";
    out << "    sp--;\n";
}

void CBackend::Impl::emitInvoke(const MIRInvoke* node, std::ostringstream& out) {
    ca::str::Utf8StringRef descRef = ca::str::Utf8StringRef::from_cstr(node->desc.c_str());
    auto retType = jvmDescToPrimitive(Type::getReturnType(descRef).getDescriptor().to_std_string());
    const char* suffix = primitiveToJNISuffix(retType);

    // Determine invocation kind
    const char* callPrefix = "Call";
    if (node->invokeOpcode == Opcodes::INVOKESTATIC) {
        callPrefix = "CallStatic";
    } else if (node->invokeOpcode == Opcodes::INVOKESPECIAL) {
        callPrefix = "CallNonvirtual";
    }

    // Count argument slots from desc
    auto argTypes = Type::getArgumentTypes(descRef);
    int argSlots = 0;
    for (auto& at : argTypes)
        argSlots += primitiveStackSize(jvmSortToPrimitive(at.getSort()));

    if (retType == PrimitiveKind::VOID) {
        out << "    (*env)->" << callPrefix << "VoidMethod(env, ";
    } else {
        out << "    stack[sp - " << argSlots << "]"
            << stackAccessor(retType) << " = (*env)->"
            << callPrefix << suffix << "Method(env, ";
    }

    // Receiver or class
    if (node->invokeOpcode == Opcodes::INVOKESTATIC) {
        out << "NULL, NULL";
    } else {
        out << "stack[sp - " << argSlots << "].l, NULL";
    }
    out << ");\n";

    // Pop args + receiver
    int popCount = argSlots + (node->invokeOpcode == Opcodes::INVOKESTATIC ? 0 : 1);
    if (retType != PrimitiveKind::VOID)
        popCount--; // result stays on stack
    if (popCount > 0)
        out << "    sp -= " << popCount << ";\n";
}

void CBackend::Impl::emitCheckCast(const MIRCheckCast* node, std::ostringstream& out) {
    out << "    /* CHECKCAST " << node->typeDesc << " */\n";
}

void CBackend::Impl::emitInstanceOf(const MIRInstanceOf* node, std::ostringstream& out) {
    out << "    stack[sp].i = (*env)->IsInstanceOf(env, stack[sp].l, "
        << "(*env)->FindClass(env, \"" << node->typeDesc << "\"));\n";
}

void CBackend::Impl::emitMonitor(const MIRMonitor* node, std::ostringstream& out) {
    if (node->enter)
        out << "    (*env)->MonitorEnter(env, stack[sp--].l);\n";
    else
        out << "    (*env)->MonitorExit(env, stack[sp--].l);\n";
}

void CBackend::Impl::emitThrow(const MIRThrow* node, std::ostringstream& out) {
    (void)node;
    out << "    (*env)->Throw(env, stack[sp--].l);\n";
    out << "    return;\n";
}

} // namespace mj2x

