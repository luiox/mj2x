#include "invokedynamic_preprocessor.hpp"

#include "libca/core/any.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

// StringConcatFactory recipe 标记（见 JDK java.lang.invoke.StringConcatFactory）：
//    = 动态参数占位（按顺序取下一个 indy 实参）
//    = 常量占位（按顺序取下一个 bsmArg 静态常量）
//   其余字符 = 字面量文本
constexpr char kTagArg   = '\x01';
constexpr char kTagConst = '\x02';

// 字符串 → Utf8StringRef（经 arena intern）。arena 归 CompilerIndex 持有，比预处理期长命。
ca::str::Utf8StringRef intern(ca::str::Utf8StringArena& arena, const char* s)
{
    return arena.intern(s);
}

// Utf8StringRef → std::string。
std::string toStd(const ca::str::Utf8StringRef& ref)
{
    return std::string(reinterpret_cast<const char*>(ref.data()), ref.byte_length());
}

// Any → std::string（recipe / 字符串常量可能以 std::string 或 Utf8StringRef 承载）。
bool anyAsString(const ca::core::Any& value, std::string& out)
{
    if (auto* p = value.as<std::string>()) {
        out = *p;
        return true;
    }
    if (auto* p = value.as<ca::str::Utf8StringRef>()) {
        out = toStd(*p);
        return true;
    }
    return false;
}

// 某个 Type sort 占几个 slot（long/double = 2，其余 = 1）。
int slotSize(int sort)
{
    return (sort == Type::LONG || sort == Type::DOUBLE) ? 2 : 1;
}

// 按 Type sort 选 xLOAD / xSTORE 操作码。
int loadOpcode(int sort)
{
    switch (sort) {
    case Type::LONG: return Opcodes::LLOAD;
    case Type::FLOAT: return Opcodes::FLOAD;
    case Type::DOUBLE: return Opcodes::DLOAD;
    case Type::ARRAY:
    case Type::OBJECT: return Opcodes::ALOAD;
    default: return Opcodes::ILOAD;   // boolean/char/byte/short/int 都以 int 存栈
    }
}

int storeOpcode(int sort)
{
    switch (sort) {
    case Type::LONG: return Opcodes::LSTORE;
    case Type::FLOAT: return Opcodes::FSTORE;
    case Type::DOUBLE: return Opcodes::DSTORE;
    case Type::ARRAY:
    case Type::OBJECT: return Opcodes::ASTORE;
    default: return Opcodes::ISTORE;
    }
}

// StringBuilder.append 的重载描述符（按参数 Type sort 选）。
const char* appendDesc(const Type& argType)
{
    switch (argType.getSort()) {
    case Type::BOOLEAN: return "(Z)Ljava/lang/StringBuilder;";
    case Type::CHAR: return "(C)Ljava/lang/StringBuilder;";
    case Type::BYTE:
    case Type::SHORT:
    case Type::INT: return "(I)Ljava/lang/StringBuilder;";
    case Type::LONG: return "(J)Ljava/lang/StringBuilder;";
    case Type::FLOAT: return "(F)Ljava/lang/StringBuilder;";
    case Type::DOUBLE: return "(D)Ljava/lang/StringBuilder;";
    case Type::ARRAY:
    case Type::OBJECT: {
        const std::string desc = toStd(argType.getDescriptor());
        if (desc == "Ljava/lang/String;") return "(Ljava/lang/String;)Ljava/lang/StringBuilder;";
        return "(Ljava/lang/Object;)Ljava/lang/StringBuilder;";
    }
    default: return "(Ljava/lang/Object;)Ljava/lang/StringBuilder;";
    }
}

// 在 seq 尾部追加「append 一个字符串字面量」的字节码：LDC <str> + INVOKEVIRTUAL
// StringBuilder.append(String)。栈上此前须有 StringBuilder 引用，append 返回自身，
// 栈保持 StringBuilder 顶（链式）。
void emitAppendString(ca::str::Utf8StringArena& arena, InsnList& seq, const std::string& literal)
{
    seq.add(new LdcInsnNode(literal));
    seq.add(new MethodInsnNode(Opcodes::INVOKEVIRTUAL, intern(arena, "java/lang/StringBuilder"),
                               intern(arena, "append"),
                               intern(arena, "(Ljava/lang/String;)Ljava/lang/StringBuilder;"),
                               false));
}

// 展开单个 makeConcat[WithConstants] indy 站点。返回是否改写成功；失败（不支持的形态）
// 保持 indy 原样、交由 SSA fail-closed。
//
// 策略：indy 执行前 N 个动态实参已在操作数栈上（栈底=第1参，栈顶=第N参）。改写为：
//   1. 逆序 xSTORE 到新分配的局部槽（栈顶=最后一个参数先存），清空这些参数；
//   2. new StringBuilder / dup / <init>；
//   3. 按 recipe 从左到右：字面量→append(String)；动态占位→xLOAD 对应槽+append(<type>)；
//      常量占位→LDC/append；
//   4. toString()，栈顶恢复为单个 String（与 indy 返回一致）。
// 单块直线代码，不引入分支/label，StackMapTable 帧不受影响；仅 maxLocals 增大。
bool expandConcatSite(ca::str::Utf8StringArena& arena, MethodNode* m, InvokeDynamicInsnNode* indy,
                      bool withConstants)
{
    const std::string indyDesc = toStd(indy->desc);
    std::vector<Type> argTypes  = Type::getArgumentTypes(arena.intern(indyDesc.c_str()));

    // recipe：makeConcatWithConstants 的 bsmArgs[0] 是 recipe；makeConcat 无 recipe，
    // 等价于「全部动态参数顺序拼接」。
    std::string recipe;
    size_t      nextConst = 1;   // withConstants 时常量从 bsmArgs[1] 起
    if (withConstants) {
        if (indy->bsmArgs.empty() || !anyAsString(indy->bsmArgs[0], recipe)) {
            return false;   // 无 recipe 常量 → 不支持形态
        }
    }
    else {
        // makeConcat：合成全 kTagArg 的 recipe。
        recipe.assign(argTypes.size(), kTagArg);
    }

    // 为每个动态参数分配局部槽（从当前 maxLocals 起）。
    std::vector<int> argSlot(argTypes.size());
    int              slot = m->maxLocals;
    for (size_t i = 0; i < argTypes.size(); ++i) {
        argSlot[i] = slot;
        slot += slotSize(argTypes[i].getSort());
    }

    // 展开序列写进临时 list，再整段 insertBefore(indy) + remove(indy)。
    InsnList seq;

    // 1. 逆序把栈上参数存进局部槽（栈顶是最后一个参数）。
    for (size_t r = argTypes.size(); r > 0; --r) {
        const size_t i = r - 1;
        seq.add(new VarInsnNode(storeOpcode(argTypes[i].getSort()), argSlot[i]));
    }

    // 2. new StringBuilder / dup / <init>()V。
    seq.add(new TypeInsnNode(Opcodes::NEW, intern(arena, "java/lang/StringBuilder")));
    seq.add(new InsnNode(Opcodes::DUP));
    seq.add(new MethodInsnNode(Opcodes::INVOKESPECIAL, intern(arena, "java/lang/StringBuilder"),
                               intern(arena, "<init>"), intern(arena, "()V"), false));

    // 3. 遍历 recipe。
    size_t      nextArg = 0;
    std::string pending;   // 累积的字面量文本
    auto        flushPending = [&]() {
        if (!pending.empty()) {
            emitAppendString(arena, seq, pending);
            pending.clear();
        }
    };
    for (char c : recipe) {
        if (c == kTagArg) {
            flushPending();
            if (nextArg >= argTypes.size()) return false;   // recipe 与描述符不一致
            const Type& t = argTypes[nextArg];
            seq.add(new VarInsnNode(loadOpcode(t.getSort()), argSlot[nextArg]));
            seq.add(new MethodInsnNode(Opcodes::INVOKEVIRTUAL,
                                       intern(arena, "java/lang/StringBuilder"),
                                       intern(arena, "append"), intern(arena, appendDesc(t)),
                                       false));
            ++nextArg;
        }
        else if (c == kTagConst) {
            flushPending();
            std::string constStr;
            if (nextConst >= indy->bsmArgs.size() ||
                !anyAsString(indy->bsmArgs[nextConst], constStr)) {
                return false;   // 非字符串常量占位 → 暂不支持
            }
            emitAppendString(arena, seq, constStr);
            ++nextConst;
        }
        else {
            pending.push_back(c);
        }
    }
    flushPending();

    // 4. toString()。
    seq.add(new MethodInsnNode(Opcodes::INVOKEVIRTUAL, intern(arena, "java/lang/StringBuilder"),
                               intern(arena, "toString"),
                               intern(arena, "()Ljava/lang/String;"), false));

    // 插入到 indy 之前并删除 indy。
    m->instructions.insert(indy, seq);
    m->instructions.remove(indy);

    // maxLocals 增大以容纳新槽；maxStack 需容纳 StringBuilder+dup+参数，保守 +3。
    if (slot > m->maxLocals) m->maxLocals = slot;
    m->maxStack += 3;
    return true;
}

// 判断 indy 是否为 StringConcatFactory 站点，并区分 makeConcat / makeConcatWithConstants。
// 返回 true 时通过 outWithConstants 输出是否带常量。
bool isStringConcatSite(const InvokeDynamicInsnNode* indy, bool& outWithConstants)
{
    const std::string bsmOwner = toStd(indy->bsm.getOwner());
    const std::string bsmName  = toStd(indy->bsm.getName());
    if (bsmOwner != "java/lang/invoke/StringConcatFactory") return false;
    if (bsmName == "makeConcatWithConstants") {
        outWithConstants = true;
        return true;
    }
    if (bsmName == "makeConcat") {
        outWithConstants = false;
        return true;
    }
    return false;
}

// 按 Type sort 选 xRETURN 操作码。
int returnOpcode(int sort)
{
    switch (sort) {
    case Type::VOID: return Opcodes::RETURN;
    case Type::LONG: return Opcodes::LRETURN;
    case Type::FLOAT: return Opcodes::FRETURN;
    case Type::DOUBLE: return Opcodes::DRETURN;
    case Type::ARRAY:
    case Type::OBJECT: return Opcodes::ARETURN;
    default: return Opcodes::IRETURN;
    }
}

// 判断 indy 是否为 LambdaMetafactory.metafactory 站点（阶段2 只认最简 metafactory，
// 不认 altMetafactory——后者带 serializable/marker interface 等额外标志）。
bool isLambdaMetafactorySite(const InvokeDynamicInsnNode* indy)
{
    return toStd(indy->bsm.getOwner()) == "java/lang/invoke/LambdaMetafactory" &&
           toStd(indy->bsm.getName()) == "metafactory";
}

// 在 index 的非 library 类里按 owner/name/desc 找方法（用于给私有目标方法提权）。
MethodNode* findMethodInIndex(CompilerIndex& index, const std::string& owner,
                              const std::string& name, const std::string& desc)
{
    auto* cls = index.getClass(owner);
    if (!cls || !cls->classNode) return nullptr;
    for (auto* m : cls->classNode->methods) {
        if (m->name.to_std_string() == name && m->desc.to_std_string() == desc) return m;
    }
    return nullptr;
}

// 合成一个实现 functional interface 的类：
//   public final class <synthName> implements <ifaceInternal> {
//     <synthName>() { super(); }
//     public <samName><samDesc> { return <impl>(args...); }   // impl 走 INVOKESTATIC
//   }
// 仅覆盖无状态（无捕获字段）+ impl tag==H_INVOKESTATIC + samDesc 与 impl desc 一致。
std::unique_ptr<ClassNode> buildLambdaClassNode(ca::str::Utf8StringArena& arena,
                                                const std::string& synthName,
                                                const std::string& ifaceInternal,
                                                const std::string& samName,
                                                const std::string& samDesc, const Handle& impl)
{
    auto cn     = std::make_unique<ClassNode>();
    cn->version = Opcodes::V1_8;
    cn->access =
        Opcodes::ACC_PUBLIC | Opcodes::ACC_FINAL | Opcodes::ACC_SUPER | Opcodes::ACC_SYNTHETIC;
    cn->name      = arena.intern(synthName.c_str());
    cn->superName = arena.intern("java/lang/Object");
    cn->interfaces.push_back(arena.intern(ifaceInternal.c_str()));

    std::vector<ca::str::Utf8StringRef> noExc;

    // <init>()V : aload_0 / invokespecial Object.<init> / return
    auto* init = new MethodNode(Opcodes::ACC_PUBLIC, arena.intern("<init>"), arena.intern("()V"),
                                arena.intern(""), noExc);
    init->instructions.add(new VarInsnNode(Opcodes::ALOAD, 0));
    init->instructions.add(new MethodInsnNode(Opcodes::INVOKESPECIAL,
                                              arena.intern("java/lang/Object"),
                                              arena.intern("<init>"), arena.intern("()V"), false));
    init->instructions.add(new InsnNode(Opcodes::RETURN));
    init->maxStack  = 1;
    init->maxLocals = 1;
    cn->methods.push_back(init);

    // SAM 方法：加载全部参数（实例方法，slot 0 = this，参数从 slot 1 起）→ invokestatic impl
    // → xreturn。samDesc 与 impl desc 一致（MVP 约束），参数直接透传。
    auto* sam = new MethodNode(Opcodes::ACC_PUBLIC, arena.intern(samName.c_str()),
                               arena.intern(samDesc.c_str()), arena.intern(""), noExc);
    std::vector<Type> samArgs = Type::getArgumentTypes(arena.intern(samDesc.c_str()));
    int               slot    = 1;
    int               maxStack = 0;
    for (const auto& t : samArgs) {
        sam->instructions.add(new VarInsnNode(loadOpcode(t.getSort()), slot));
        slot += slotSize(t.getSort());
        maxStack += slotSize(t.getSort());
    }
    sam->instructions.add(new MethodInsnNode(Opcodes::INVOKESTATIC, intern(arena, toStd(impl.getOwner()).c_str()),
                                             intern(arena, toStd(impl.getName()).c_str()),
                                             intern(arena, toStd(impl.getDesc()).c_str()), false));
    const Type samRet = Type::getReturnType(arena.intern(samDesc.c_str()));
    sam->instructions.add(new InsnNode(returnOpcode(samRet.getSort())));
    sam->maxStack  = maxStack > 0 ? maxStack : 1;
    sam->maxLocals = slot;
    cn->methods.push_back(sam);

    return cn;
}

// 待注册的合成类（在 getClasses 遍历结束后统一 addClass/upsertJarResource，避免
// 遍历中改 map 使迭代器失效）。
struct PendingLambdaClass
{
    std::string                internalName;   // e.g. mj2x/lambda/Lambda$0
    std::unique_ptr<ClassNode> node;
};

// 展开单个无状态 lambda indy 站点。返回是否改写成功。
// 失败（捕获 lambda / 非 INVOKESTATIC impl / desc 不匹配）保持 indy 原样、交 SSA fail-closed。
bool expandStatelessLambdaSite(CompilerIndex& index, MethodNode* m, InvokeDynamicInsnNode* indy,
                               int synthCounter, std::vector<PendingLambdaClass>& pending)
{
    auto& arena = index.arena();

    // 无捕获：indy desc 必须无参（捕获值会作为 indy 参数压栈）。
    const std::string indyDesc = toStd(indy->desc);
    if (!Type::getArgumentTypes(arena.intern(indyDesc.c_str())).empty()) return false;

    // functional interface = indy 返回类型（须是对象类型）。
    const Type ifaceType = Type::getReturnType(arena.intern(indyDesc.c_str()));
    if (ifaceType.getSort() != Type::OBJECT) return false;
    const std::string ifaceInternal = toStd(ifaceType.getInternalName());

    // bsmArgs：[0]=samMethodType(Type), [1]=implMethodHandle(Handle), [2]=instantiatedMethodType。
    if (indy->bsmArgs.size() < 2) return false;
    auto* samTypePtr = indy->bsmArgs[0].as<Type>();
    auto* implPtr    = indy->bsmArgs[1].as<Handle>();
    if (!samTypePtr || !implPtr) return false;
    if (samTypePtr->getSort() != Type::METHOD) return false;

    // MVP：impl 必须是 INVOKESTATIC，且 desc 与 SAM desc 一致（无装箱/桥接）。
    if (implPtr->getTag() != Opcodes::H_INVOKESTATIC) return false;
    const std::string samDesc  = toStd(samTypePtr->getDescriptor());
    const std::string implDesc = toStd(implPtr->getDesc());
    if (samDesc != implDesc) return false;

    // 若 impl 目标在本 index 的非 library 类且为 private，提权（去 ACC_PRIVATE + 置 public），
    // 否则合成类跨类 INVOKESTATIC 触发 IllegalAccessError。库方法（已 public）无需处理。
    if (auto* target = findMethodInIndex(index, toStd(implPtr->getOwner()),
                                         toStd(implPtr->getName()), implDesc)) {
        if (target->access & Opcodes::ACC_PRIVATE) {
            target->access &= ~Opcodes::ACC_PRIVATE;
            target->access |= Opcodes::ACC_PUBLIC;
        }
    }

    const std::string synthName = "mj2x/lambda/Lambda$" + std::to_string(synthCounter);

    // 站点改写：new synth / dup / invokespecial synth.<init>()V（无捕获 → 空构造）。
    InsnList seq;
    seq.add(new TypeInsnNode(Opcodes::NEW, intern(arena, synthName.c_str())));
    seq.add(new InsnNode(Opcodes::DUP));
    seq.add(new MethodInsnNode(Opcodes::INVOKESPECIAL, intern(arena, synthName.c_str()),
                               intern(arena, "<init>"), intern(arena, "()V"), false));
    m->instructions.insert(indy, seq);
    m->instructions.remove(indy);
    m->maxStack += 2;

    PendingLambdaClass pc;
    pc.internalName = synthName;
    pc.node = buildLambdaClassNode(arena, synthName, ifaceInternal, toStd(indy->name), samDesc,
                                   *implPtr);
    pending.push_back(std::move(pc));
    return true;
}

void processImpl(CompilerIndex& index)
{
    auto& arena          = index.arena();
    int   expanded       = 0;
    int   lambdaExpanded = 0;
    int   remainingIndy  = 0;
    int   synthCounter   = 0;

    std::vector<PendingLambdaClass> pendingLambdaClasses;

    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;
        for (auto* m : cls.classNode->methods) {
            // 先收集本方法的可展开站点（边遍历边改 list 会失效）。
            std::vector<InvokeDynamicInsnNode*> concatSites;
            std::vector<InvokeDynamicInsnNode*> lambdaSites;
            for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
                if (insn->getType() != AbstractInsnNode::INVOKE_DYNAMIC_INSN) continue;
                auto* indy = static_cast<InvokeDynamicInsnNode*>(insn);
                bool  wc   = false;
                if (isStringConcatSite(indy, wc)) concatSites.push_back(indy);
                else if (isLambdaMetafactorySite(indy)) lambdaSites.push_back(indy);
            }
            for (auto* indy : concatSites) {
                bool wc = false;
                isStringConcatSite(indy, wc);
                if (expandConcatSite(arena, m, indy, wc)) ++expanded;
            }
            for (auto* indy : lambdaSites) {
                if (expandStatelessLambdaSite(index, m, indy, synthCounter,
                                              pendingLambdaClasses)) {
                    ++lambdaExpanded;
                    ++synthCounter;
                }
            }
            // 统计仍残留的 indy（展开失败或非 concat/lambda 形态）。
            for (auto* insn = m->instructions.getFirst(); insn; insn = insn->nextInsn) {
                if (insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) ++remainingIndy;
            }
        }
    }

    // 遍历结束后统一注册合成 lambda 类（遍历中改 getClasses map 会使迭代器失效）。
    // 单类输出走 addClass（sidecar），JAR 输出走 upsertJarResource。isLibrary=true 使
    // RegC 跳过编译（合成类保留纯字节码，由 OutputWriter 写出）。
    for (auto& pc : pendingLambdaClasses) {
        if (index.isJarInput()) {
            ClassWriter cw(arena, 0);
            pc.node->accept(&cw);
            index.upsertJarResource(pc.internalName + ".class", cw.toByteArray());
        }
        else {
            CompilerClass synthCls;
            synthCls.classNode = std::move(pc.node);
            synthCls.isLibrary = true;
            synthCls.modified  = true;
            index.addClass(pc.internalName, std::move(synthCls));
        }
    }

    if (expanded > 0) {
        printf("[InvokeDynamicPreproc] Expanded %d StringConcatFactory site(s) into "
               "StringBuilder bytecode (RegC-native).\n",
               expanded);
    }
    if (lambdaExpanded > 0) {
        printf("[InvokeDynamicPreproc] Expanded %d stateless lambda site(s) into synthesized "
               "functional-interface class(es) (RegC-native).\n",
               lambdaExpanded);
    }
    if (remainingIndy > 0) {
        printf("[InvokeDynamicPreproc] %d invokedynamic site(s) remain unexpanded "
               "(lambda / custom bootstrap); StackC lowers via runtime bridge, RegC/SSA "
               "still fail-closed until later expansion phases.\n",
               remainingIndy);
    }
}

}   // namespace

void InvokeDynamicPreprocessor::process(CompilerIndex& index)
{
    processImpl(index);
}

}   // namespace mj2x
