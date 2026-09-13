#include "invokedynamic_preprocessor.hpp"

#include "libca/core/any.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "libca/log/log_macros.hpp"

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
void emitAppendString(MethodNode& m, ca::str::Utf8StringArena& arena, InsnList& seq,
                      const std::string& literal)
{
    seq.add(m.create<LdcInsnNode>(literal));
    seq.add(m.create<MethodInsnNode>(Opcodes::INVOKEVIRTUAL, intern(arena, "java/lang/StringBuilder"),
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
    InsnList seq(&m->arena());

    // 1. 逆序把栈上参数存进局部槽（栈顶是最后一个参数）。
    for (size_t r = argTypes.size(); r > 0; --r) {
        const size_t i = r - 1;
        seq.add(m->create<VarInsnNode>(storeOpcode(argTypes[i].getSort()), argSlot[i]));
    }

    // 2. new StringBuilder / dup / <init>()V。
    seq.add(m->create<TypeInsnNode>(Opcodes::NEW, intern(arena, "java/lang/StringBuilder")));
    seq.add(m->create<InsnNode>(Opcodes::DUP));
    seq.add(m->create<MethodInsnNode>(Opcodes::INVOKESPECIAL, intern(arena, "java/lang/StringBuilder"),
                               intern(arena, "<init>"), intern(arena, "()V"), false));

    // 3. 遍历 recipe。
    size_t      nextArg = 0;
    std::string pending;   // 累积的字面量文本
    auto        flushPending = [&]() {
        if (!pending.empty()) {
            emitAppendString(*m, arena, seq, pending);
            pending.clear();
        }
    };
    for (char c : recipe) {
        if (c == kTagArg) {
            flushPending();
            if (nextArg >= argTypes.size()) return false;   // recipe 与描述符不一致
            const Type& t = argTypes[nextArg];
            seq.add(m->create<VarInsnNode>(loadOpcode(t.getSort()), argSlot[nextArg]));
            seq.add(m->create<MethodInsnNode>(Opcodes::INVOKEVIRTUAL,
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
            emitAppendString(*m, arena, seq, constStr);
            ++nextConst;
        }
        else {
            pending.push_back(c);
        }
    }
    flushPending();

    // 4. toString()。
    seq.add(m->create<MethodInsnNode>(Opcodes::INVOKEVIRTUAL, intern(arena, "java/lang/StringBuilder"),
                               intern(arena, "toString"),
                               intern(arena, "()Ljava/lang/String;"), false));

    // 插入到 indy 之前并用 removeAndDelete 摘链销毁 indy
    //（对齐 init_clinit_mover 先例）。
    m->instructions.insert(indy, seq);
    m->instructions.removeAndDelete(indy);

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

// ---- 阶段2d：装箱/桥接适配（samMethodType 擦除形态 ↔ implDesc 具体形态）----
// 泛型 functional interface（Function/Supplier/Consumer/Predicate…）经类型擦除后 SAM 形参/
// 返回都是 Object，而 impl 是具体引用或基本类型。LambdaMetafactory 在运行期做的适配这里
// 静态展开进合成 SAM 体：
//   引用收窄        → CHECKCAST
//   引用→基本类型   → CHECKCAST 包装类 + xxxValue() 拆箱
//   基本类型→引用   → 包装类.valueOf 装箱（目标限 Object / 对应包装类）
//   SAM 返回 void   → POP/POP2 丢弃 impl 返回值
// 基本类型互转（i2l 加宽等，需看 instantiatedMethodType）不支持，保持 fail-closed。

bool isPrimitiveSort(int sort)
{
    return sort >= Type::BOOLEAN && sort <= Type::DOUBLE;
}

bool isReferenceSort(int sort)
{
    return sort == Type::OBJECT || sort == Type::ARRAY;
}

const char* wrapperInternal(int sort)
{
    switch (sort) {
    case Type::BOOLEAN: return "java/lang/Boolean";
    case Type::CHAR: return "java/lang/Character";
    case Type::BYTE: return "java/lang/Byte";
    case Type::SHORT: return "java/lang/Short";
    case Type::INT: return "java/lang/Integer";
    case Type::FLOAT: return "java/lang/Float";
    case Type::LONG: return "java/lang/Long";
    case Type::DOUBLE: return "java/lang/Double";
    default: return nullptr;
    }
}

const char* unboxMethodName(int sort)
{
    switch (sort) {
    case Type::BOOLEAN: return "booleanValue";
    case Type::CHAR: return "charValue";
    case Type::BYTE: return "byteValue";
    case Type::SHORT: return "shortValue";
    case Type::INT: return "intValue";
    case Type::FLOAT: return "floatValue";
    case Type::LONG: return "longValue";
    case Type::DOUBLE: return "doubleValue";
    default: return nullptr;
    }
}

const char* unboxMethodDesc(int sort)
{
    switch (sort) {
    case Type::BOOLEAN: return "()Z";
    case Type::CHAR: return "()C";
    case Type::BYTE: return "()B";
    case Type::SHORT: return "()S";
    case Type::INT: return "()I";
    case Type::FLOAT: return "()F";
    case Type::LONG: return "()J";
    case Type::DOUBLE: return "()D";
    default: return nullptr;
    }
}

const char* boxValueOfDesc(int sort)
{
    switch (sort) {
    case Type::BOOLEAN: return "(Z)Ljava/lang/Boolean;";
    case Type::CHAR: return "(C)Ljava/lang/Character;";
    case Type::BYTE: return "(B)Ljava/lang/Byte;";
    case Type::SHORT: return "(S)Ljava/lang/Short;";
    case Type::INT: return "(I)Ljava/lang/Integer;";
    case Type::FLOAT: return "(F)Ljava/lang/Float;";
    case Type::LONG: return "(J)Ljava/lang/Long;";
    case Type::DOUBLE: return "(D)Ljava/lang/Double;";
    default: return nullptr;
    }
}

// CHECKCAST 的类型串：对象用内部名，数组用描述符（JVMS 规定数组 CHECKCAST 写描述符）。
std::string checkcastName(const Type& t)
{
    return t.getSort() == Type::ARRAY ? toStd(t.getDescriptor()) : toStd(t.getInternalName());
}

// 基本类型加宽（JLS §5.1.2）对应的转换字节码 opcode；非受支持加宽返回 -1（含收窄、
// boolean、同型）。栈上 byte/short/char 均以 int 表示，故 int-family → 更宽用 i2x。
// 阶段2f：method reference 的实参/返回基本类型与 SAM 相差一个加宽时，LMF 运行期插入的
// i2l/i2d/l2d/f2d 等在此静态展开。
int wideningOpcode(int from, int to)
{
    const bool fromIntFamily =
        (from == Type::BYTE || from == Type::SHORT || from == Type::CHAR || from == Type::INT);
    if (fromIntFamily) {
        if (to == Type::LONG) return Opcodes::I2L;
        if (to == Type::FLOAT) return Opcodes::I2F;
        if (to == Type::DOUBLE) return Opcodes::I2D;
    }
    else if (from == Type::LONG) {
        if (to == Type::FLOAT) return Opcodes::L2F;
        if (to == Type::DOUBLE) return Opcodes::L2D;
    }
    else if (from == Type::FLOAT) {
        if (to == Type::DOUBLE) return Opcodes::F2D;
    }
    return -1;
}

// 栈顶值能否从 from 适配到 to（对应 LambdaMetafactory 运行期适配的静态可展开子集）。
bool canAdaptValue(const Type& from, const Type& to)
{
    if (toStd(from.getDescriptor()) == toStd(to.getDescriptor())) return true;
    const bool fromRef = isReferenceSort(from.getSort());
    const bool toRef   = isReferenceSort(to.getSort());
    if (fromRef && toRef) return true;                              // CHECKCAST
    if (fromRef && isPrimitiveSort(to.getSort())) return true;      // CHECKCAST 包装类 + 拆箱
    if (isPrimitiveSort(from.getSort()) && toRef) {
        // 装箱目标限 Object / 对应包装类（javac 擦除只产出这两种；其余形态 fail-closed）。
        const std::string toDesc = toStd(to.getDescriptor());
        return toDesc == "Ljava/lang/Object;" ||
               toDesc == std::string("L") + wrapperInternal(from.getSort()) + ";";
    }
    if (isPrimitiveSort(from.getSort()) && isPrimitiveSort(to.getSort())) {
        // 基本类型互转：仅加宽支持（阶段2f），收窄仍 fail-closed。
        return wideningOpcode(from.getSort(), to.getSort()) != -1;
    }
    return false;
}

// 在 seq 尾部发射「栈顶值 from → to」的适配字节码。调用前须先 canAdaptValue 校验。
void emitAdaptValue(MethodNode& m, ca::str::Utf8StringArena& arena, InsnList& seq, const Type& from,
                    const Type& to)
{
    const std::string fromDesc = toStd(from.getDescriptor());
    const std::string toDesc   = toStd(to.getDescriptor());
    if (fromDesc == toDesc) return;
    const bool fromRef = isReferenceSort(from.getSort());
    if (fromRef && isReferenceSort(to.getSort())) {
        // 收窄到 Object 无需 cast（一切引用 <: Object）；其余发 CHECKCAST 保证验证器通过。
        if (toDesc != "Ljava/lang/Object;")
            seq.add(m.create<TypeInsnNode>(Opcodes::CHECKCAST, intern(arena, checkcastName(to).c_str())));
        return;
    }
    if (fromRef) {
        // 引用 → 基本类型：CHECKCAST 包装类（已是包装类则免）+ xxxValue() 拆箱。
        const char*       w     = wrapperInternal(to.getSort());
        const std::string wDesc = std::string("L") + w + ";";
        if (fromDesc != wDesc)
            seq.add(m.create<TypeInsnNode>(Opcodes::CHECKCAST, intern(arena, w)));
        seq.add(m.create<MethodInsnNode>(Opcodes::INVOKEVIRTUAL, intern(arena, w),
                                   intern(arena, unboxMethodName(to.getSort())),
                                   intern(arena, unboxMethodDesc(to.getSort())), false));
        return;
    }
    // 基本类型 → 基本类型：加宽（canAdaptValue 已保证是受支持加宽，阶段2f）。
    if (isPrimitiveSort(to.getSort())) {
        seq.add(m.create<InsnNode>(wideningOpcode(from.getSort(), to.getSort())));
        return;
    }
    // 基本类型 → 引用：包装类.valueOf 装箱（canAdaptValue 已限定目标为 Object/包装类）。
    const char* w = wrapperInternal(from.getSort());
    seq.add(m.create<MethodInsnNode>(Opcodes::INVOKESTATIC, intern(arena, w), intern(arena, "valueOf"),
                               intern(arena, boxValueOfDesc(from.getSort())), false));
}

// impl 返回值能否适配为 SAM 返回值。
bool canAdaptReturn(const Type& implRet, const Type& samRet)
{
    if (samRet.getSort() == Type::VOID) return true;    // 丢弃返回值（POP/POP2）
    if (implRet.getSort() == Type::VOID) return false;  // SAM 要值而 impl 无值
    return canAdaptValue(implRet, samRet);
}

// 发射 SAM 返回序列：适配栈顶 impl 返回值（或丢弃）后按 samRet 发 xRETURN。
void emitReturnAdapt(MethodNode& m, ca::str::Utf8StringArena& arena, InsnList& seq,
                     const Type& implRet, const Type& samRet)
{
    if (samRet.getSort() == Type::VOID) {
        if (implRet.getSort() != Type::VOID)
            seq.add(m.create<InsnNode>(slotSize(implRet.getSort()) == 2 ? Opcodes::POP2 : Opcodes::POP));
        seq.add(m.create<InsnNode>(Opcodes::RETURN));
        return;
    }
    emitAdaptValue(m, arena, seq, implRet, samRet);
    seq.add(m.create<InsnNode>(returnOpcode(samRet.getSort())));
}

// altMetafactory flags（见 JDK java.lang.invoke.LambdaMetafactory）。
constexpr int kAltFlagSerializable = 1;   // 追加实现 java/io/Serializable
constexpr int kAltFlagMarkers      = 2;   // 追加实现额外 marker 接口
constexpr int kAltFlagBridges      = 4;   // 额外发射桥接方法

// altMetafactory 尾部解析结果：额外 marker 接口（含 Serializable）与桥接方法描述符。
struct AltLambdaInfo
{
    std::vector<std::string> markerIfaces;   // 追加到合成类 interfaces 的内部名
    std::vector<std::string> bridgeDescs;    // 额外发射的 SAM 桥接方法描述符
};

// 判断 indy 是否为 LambdaMetafactory 站点（阶段2 起认 metafactory；阶段2e 起也认
// altMetafactory——后者带 serializable / marker interface / bridge 等额外标志，尾部由
// parseAltMetafactoryTail 解析）。
bool isLambdaMetafactorySite(const InvokeDynamicInsnNode* indy)
{
    if (toStd(indy->bsm.getOwner()) != "java/lang/invoke/LambdaMetafactory") return false;
    const std::string name = toStd(indy->bsm.getName());
    return name == "metafactory" || name == "altMetafactory";
}

// 解析 altMetafactory 的附加 bsmArgs（[0..2]=sam/impl/instantiated 与 metafactory 同；
// [3]=flags:int，随后按 flag 位跟 markerCount+markers、bridgeCount+bridges）。metafactory
// 站点无尾部，直接返回 true 且 out 为空。任何形态异常（未知 flag 位、缺计数、类型不符）
// 返回 false → 交 SSA fail-closed，绝不静默降级。
bool parseAltMetafactoryTail(const InvokeDynamicInsnNode* indy, AltLambdaInfo& out)
{
    if (toStd(indy->bsm.getName()) != "altMetafactory") return true;   // metafactory：无尾部
    if (indy->bsmArgs.size() < 4) return false;
    auto* flagsPtr = indy->bsmArgs[3].as<int>();
    if (!flagsPtr) return false;
    const int flags = *flagsPtr;
    // 只认已知 flag 位；出现未知位（如未来扩展）保守 fail-closed。
    if (flags & ~(kAltFlagSerializable | kAltFlagMarkers | kAltFlagBridges)) return false;

    // Serializable 是纯 marker（无方法），合成类默认序列化其 capN 字段即可 round-trip；
    // 不复刻 JVM 原生 SerializedLambda 格式（对混淆输出无意义）。
    if (flags & kAltFlagSerializable) out.markerIfaces.emplace_back("java/io/Serializable");

    size_t pos      = 4;
    auto   readCount = [&](int& cnt) -> bool {
        if (pos >= indy->bsmArgs.size()) return false;
        auto* p = indy->bsmArgs[pos].as<int>();
        if (!p || *p < 0) return false;
        cnt = *p;
        ++pos;
        return true;
    };
    if (flags & kAltFlagMarkers) {
        int cnt = 0;
        if (!readCount(cnt)) return false;
        for (int i = 0; i < cnt; ++i) {
            if (pos >= indy->bsmArgs.size()) return false;
            auto* mk = indy->bsmArgs[pos].as<Type>();   // CONSTANT_Class → Type(OBJECT)
            if (!mk || mk->getSort() != Type::OBJECT) return false;
            out.markerIfaces.push_back(toStd(mk->getInternalName()));
            ++pos;
        }
    }
    if (flags & kAltFlagBridges) {
        int cnt = 0;
        if (!readCount(cnt)) return false;
        for (int i = 0; i < cnt; ++i) {
            if (pos >= indy->bsmArgs.size()) return false;
            auto* br = indy->bsmArgs[pos].as<Type>();   // CONSTANT_MethodType → Type(METHOD)
            if (!br || br->getSort() != Type::METHOD) return false;
            out.bridgeDescs.push_back(toStd(br->getDescriptor()));
            ++pos;
        }
    }
    return true;
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

// 校验给定 SAM 描述符能否按 impl handle 形态发射（fullArgs 逐位可适配 + 返回可适配）。
// 供主 SAM 与各 bridge 描述符复用——bridge 与主 SAM 同名不同签，须各自满足适配才允许展开，
// 任一不可适配即整站点 fail-closed。
bool canEmitSam(ca::str::Utf8StringArena& arena, const std::string& samDesc,
                const std::vector<Type>& capturedTypes, const Handle& impl, bool isStatic,
                bool isCtor)
{
    const std::string implDesc  = toStd(impl.getDesc());
    std::vector<Type> samArgs   = Type::getArgumentTypes(arena.intern(samDesc.c_str()));
    std::vector<Type> implArgs  = Type::getArgumentTypes(arena.intern(implDesc.c_str()));
    std::vector<Type> fullArgs  = capturedTypes;
    fullArgs.insert(fullArgs.end(), samArgs.begin(), samArgs.end());
    const Type samRet  = Type::getReturnType(arena.intern(samDesc.c_str()));
    const Type implRet = Type::getReturnType(arena.intern(implDesc.c_str()));

    if (isStatic || isCtor) {
        // fullArgs 逐位可适配到 impl 形参（阶段2d：擦除引用 CHECKCAST / 拆装箱，见 canAdaptValue；
        // 完全同型仍是快速路径）。
        if (fullArgs.size() != implArgs.size()) return false;
        for (size_t i = 0; i < fullArgs.size(); ++i) {
            if (!canAdaptValue(fullArgs[i], implArgs[i])) return false;
        }
        if (isCtor) {
            // 构造器引用：impl 必为 <init>，SAM 返回新建对象（对象类型）。
            if (toStd(impl.getName()) != "<init>") return false;
            if (samRet.getSort() != Type::OBJECT) return false;
        }
        // 返回值可适配（含装箱/拆箱/丢弃）；不可适配交 SSA fail-closed。
        else if (!canAdaptReturn(implRet, samRet)) {
            return false;
        }
    }
    else {
        // 实例方法引用：fullArgs[0] = receiver（对象/数组，可 CHECKCAST 到 owner），
        // 其余逐位可适配到 impl 形参。
        if (fullArgs.empty()) return false;
        const int recvSort = fullArgs[0].getSort();
        if (recvSort != Type::OBJECT && recvSort != Type::ARRAY) return false;
        if (fullArgs.size() != implArgs.size() + 1) return false;
        for (size_t i = 0; i < implArgs.size(); ++i) {
            if (!canAdaptValue(fullArgs[i + 1], implArgs[i])) return false;
        }
        if (!canAdaptReturn(implRet, samRet)) return false;
    }
    return true;
}

// 合成一个实现 functional interface 的类，SAM 方法转调 impl：
//   public final class <synthName> implements <ifaceInternal> {
//     <synthName>(capturedDescs) { super(); this.capI = capI; }
//     public <samName><samDesc> { return <impl>(fullArgs...); }
//   }
//   capturedTypes 空 = 无捕获 lambda：空 <init>，SAM 只用 SAM 参数。
//   capturedTypes 非空 = 捕获 lambda：N 个 private final 字段 capI + 参数化
//     <init>(capturedDescs)V（super + 逐个 PUTFIELD），SAM 先 GETFIELD 取回捕获值再接
//     SAM 参数。
// SAM 体按 impl 的 method-handle tag 发射（fullArgs = capturedTypes ++ samArgs，javac 布局）：
//   H_INVOKESTATIC            → INVOKESTATIC impl(fullArgs)            ; xRETURN
//   H_INVOKEVIRTUAL/SPECIAL   → INVOKEVIRTUAL impl（fullArgs[0]=receiver）; xRETURN
//   H_INVOKEINTERFACE         → INVOKEINTERFACE，同上
//   H_NEWINVOKESPECIAL        → NEW owner / DUP / fullArgs / INVOKESPECIAL <init> ; ARETURN
// （H_INVOKESPECIAL 私有实例方法引用已在 expandLambdaSite 提权，故按 INVOKEVIRTUAL 发射。）
std::unique_ptr<ClassNode> buildLambdaClassNode(ca::str::Utf8StringArena& arena,
                                                const std::string& synthName,
                                                const std::string& ifaceInternal,
                                                const std::string& samName,
                                                const std::string&       samDesc,
                                                const std::vector<Type>& capturedTypes,
                                                const Handle&            impl,
                                                const AltLambdaInfo&     alt)
{
    auto cn     = std::make_unique<ClassNode>();
    cn->version = Opcodes::V1_8;
    cn->access =
        Opcodes::ACC_PUBLIC | Opcodes::ACC_FINAL | Opcodes::ACC_SUPER | Opcodes::ACC_SYNTHETIC;
    cn->name      = arena.intern(synthName.c_str());
    cn->superName = arena.intern("java/lang/Object");
    cn->interfaces.push_back(arena.intern(ifaceInternal.c_str()));
    // 阶段2e：altMetafactory 追加的 marker 接口（含 Serializable / 交集类型的额外接口）。
    // 均为无方法 marker，SAM 体不受影响，只满足 instanceof / 序列化标记语义。
    for (const auto& marker : alt.markerIfaces) cn->interfaces.push_back(arena.intern(marker.c_str()));

    std::vector<ca::str::Utf8StringRef> noExc;

    // 捕获字段 cap0..capN-1（private final）+ 构造器描述符 (capturedDescs)V。
    std::vector<std::string> capName(capturedTypes.size());
    std::vector<std::string> capDesc(capturedTypes.size());
    std::string              ctorDesc = "(";
    for (size_t i = 0; i < capturedTypes.size(); ++i) {
        capName[i] = "cap" + std::to_string(i);
        capDesc[i] = toStd(capturedTypes[i].getDescriptor());
        ctorDesc += capDesc[i];
        cn->fields.push_back(new FieldNode(Opcodes::ACC_PRIVATE | Opcodes::ACC_FINAL,
                                           arena.intern(capName[i].c_str()),
                                           arena.intern(capDesc[i].c_str()), arena.intern(""),
                                           ca::core::Any{}));
    }
    ctorDesc += ")V";

    // <init>(capturedDescs)V : super.<init>() 后逐个 aload_0 / xload param / putfield capI。
    auto* init = new MethodNode(Opcodes::ACC_PUBLIC, arena.intern("<init>"),
                                arena.intern(ctorDesc.c_str()), arena.intern(""), noExc);
    init->instructions.add(init->create<VarInsnNode>(Opcodes::ALOAD, 0));
    init->instructions.add(init->create<MethodInsnNode>(Opcodes::INVOKESPECIAL,
                                              arena.intern("java/lang/Object"),
                                              arena.intern("<init>"), arena.intern("()V"), false));
    int ctorSlot     = 1;   // slot 0 = this
    int ctorMaxStack = 1;
    for (size_t i = 0; i < capturedTypes.size(); ++i) {
        const int sort = capturedTypes[i].getSort();
        init->instructions.add(init->create<VarInsnNode>(Opcodes::ALOAD, 0));
        init->instructions.add(init->create<VarInsnNode>(loadOpcode(sort), ctorSlot));
        init->instructions.add(init->create<FieldInsnNode>(Opcodes::PUTFIELD, arena.intern(synthName.c_str()),
                                                 arena.intern(capName[i].c_str()),
                                                 arena.intern(capDesc[i].c_str())));
        ctorSlot += slotSize(sort);
        // putfield 峰值：this(1) + value(1或2)。
        ctorMaxStack = std::max(ctorMaxStack, 1 + slotSize(sort));
    }
    init->instructions.add(init->create<InsnNode>(Opcodes::RETURN));
    init->maxStack  = ctorMaxStack;
    init->maxLocals = ctorSlot;
    cn->methods.push_back(init);

    // SAM 方法体：把 fullArgs = 捕获字段（GETFIELD）+ SAM 参数（xLOAD）按序压栈，每个值
    // 压栈后立即按 impl 期望类型适配（阶段2d：CHECKCAST / 拆箱 / 装箱，见 emitAdaptValue），
    // 再按 impl 的 handle tag 发射调用。实例方法引用的 receiver 天然是 fullArgs 的首元（bound
    // 时来自捕获字段、unbound 时来自 SAM 首参），故压栈顺序与静态调用完全一致，只有调用
    // 指令不同。构造器引用需先 NEW/DUP 目标类型再压 fullArgs。
    const int         tag       = impl.getTag();
    const bool        isCtorRef = (tag == Opcodes::H_NEWINVOKESPECIAL);
    const bool        isInstRef = (tag == Opcodes::H_INVOKEVIRTUAL ||
                            tag == Opcodes::H_INVOKEINTERFACE || tag == Opcodes::H_INVOKESPECIAL);
    const std::string implOwner = toStd(impl.getOwner());
    const std::string implName  = toStd(impl.getName());
    const std::string implDesc  = toStd(impl.getDesc());

    // fullArgs 各位置的目标类型：实例引用首元是 receiver（owner 对象类型），其余对应 implArgs。
    std::vector<Type> implArgs = Type::getArgumentTypes(arena.intern(implDesc.c_str()));
    std::vector<Type> toTypes;
    if (isInstRef) toTypes.push_back(Type::getObjectType(arena.intern(implOwner.c_str())));
    toTypes.insert(toTypes.end(), implArgs.begin(), implArgs.end());

    // 发射一个 SAM 体（主 SAM 与各 bridge 复用）：按给定描述符 sDesc 载入 fullArgs、适配、
    // 按 impl handle tag 发射调用。extraAccess 供 bridge 加 ACC_BRIDGE|ACC_SYNTHETIC。
    auto buildSam = [&](const std::string& sName, const std::string& sDesc,
                        int extraAccess) -> MethodNode* {
        auto* sam = new MethodNode(Opcodes::ACC_PUBLIC | extraAccess, arena.intern(sName.c_str()),
                                   arena.intern(sDesc.c_str()), arena.intern(""), noExc);
        int    samStack = 0;
        size_t argPos   = 0;   // fullArgs 位置游标（捕获段 + SAM 参数段共用）

        // 构造器引用：先 NEW/DUP 目标类型（占 2 个栈位，位于 fullArgs 之下）。
        if (isCtorRef) {
            sam->instructions.add(sam->create<TypeInsnNode>(Opcodes::NEW, intern(arena, implOwner.c_str())));
            sam->instructions.add(sam->create<InsnNode>(Opcodes::DUP));
            samStack += 2;
        }

        // fullArgs 第一段：捕获字段 cap0..capN-1（GETFIELD + 适配）。
        for (size_t i = 0; i < capturedTypes.size(); ++i, ++argPos) {
            sam->instructions.add(sam->create<VarInsnNode>(Opcodes::ALOAD, 0));
            sam->instructions.add(sam->create<FieldInsnNode>(Opcodes::GETFIELD,
                                                    arena.intern(synthName.c_str()),
                                                    arena.intern(capName[i].c_str()),
                                                    arena.intern(capDesc[i].c_str())));
            emitAdaptValue(*sam, arena, sam->instructions, capturedTypes[i], toTypes[argPos]);
            // 适配可能改变栈宽（如 Object→long 1→2），按两侧较大者计。
            samStack += std::max(slotSize(capturedTypes[i].getSort()),
                                 slotSize(toTypes[argPos].getSort()));
        }
        // fullArgs 第二段：SAM 参数（实例方法 slot 0 = this，参数从 slot 1 起；xLOAD + 适配）。
        std::vector<Type> samArgs = Type::getArgumentTypes(arena.intern(sDesc.c_str()));
        int               slot    = 1;
        for (const auto& t : samArgs) {
            sam->instructions.add(sam->create<VarInsnNode>(loadOpcode(t.getSort()), slot));
            emitAdaptValue(*sam, arena, sam->instructions, t, toTypes[argPos]);
            slot += slotSize(t.getSort());
            samStack += std::max(slotSize(t.getSort()), slotSize(toTypes[argPos].getSort()));
            ++argPos;
        }

        const Type samRet = Type::getReturnType(arena.intern(sDesc.c_str()));
        if (isCtorRef) {
            // NEW/DUP/fullArgs/INVOKESPECIAL <init> → 栈顶留下新建对象，适配（一般免 cast）后 ARETURN。
            sam->instructions.add(sam->create<MethodInsnNode>(Opcodes::INVOKESPECIAL,
                                                     intern(arena, implOwner.c_str()),
                                                     intern(arena, "<init>"),
                                                     intern(arena, implDesc.c_str()), false));
            emitReturnAdapt(*sam, arena, sam->instructions,
                            Type::getObjectType(arena.intern(implOwner.c_str())), samRet);
        }
        else {
            int  invokeOpcode = Opcodes::INVOKESTATIC;
            bool invokeIface  = false;
            switch (tag) {
            case Opcodes::H_INVOKESTATIC: invokeOpcode = Opcodes::INVOKESTATIC; break;
            case Opcodes::H_INVOKEINTERFACE:
                invokeOpcode = Opcodes::INVOKEINTERFACE;
                invokeIface  = true;
                break;
            // H_INVOKEVIRTUAL 与已提权的 H_INVOKESPECIAL 均按虚调用发射。
            default: invokeOpcode = Opcodes::INVOKEVIRTUAL; break;
            }
            sam->instructions.add(sam->create<MethodInsnNode>(invokeOpcode, intern(arena, implOwner.c_str()),
                                                     intern(arena, implName.c_str()),
                                                     intern(arena, implDesc.c_str()), invokeIface));
            emitReturnAdapt(*sam, arena, sam->instructions,
                            Type::getReturnType(arena.intern(implDesc.c_str())), samRet);
        }
        // 栈峰值另须容纳返回值（如零参 SAM 调 ()J impl：参数段 0 栈位、返回 2 栈位）。
        const Type implRetT =
            isCtorRef ? Type::getObjectType(arena.intern(implOwner.c_str()))
                      : Type::getReturnType(arena.intern(implDesc.c_str()));
        const int retSlots =
            std::max(implRetT.getSort() == Type::VOID ? 0 : slotSize(implRetT.getSort()),
                     samRet.getSort() == Type::VOID ? 0 : slotSize(samRet.getSort()));
        sam->maxStack  = std::max({samStack, retSlots, 1});
        sam->maxLocals = slot;
        return sam;
    };

    // 主 SAM。
    cn->methods.push_back(buildSam(samName, samDesc, 0));
    // 阶段2e：altMetafactory 桥接方法（同名不同签，通常是擦除到更泛的描述符）。逐个发射
    // ACC_BRIDGE|ACC_SYNTHETIC 方法，body 复用同一 impl 适配路径。去重并跳过与主 SAM 重复的
    // 描述符，避免出现同名同签的重复方法。
    std::vector<std::string> emittedDescs{samDesc};
    for (const auto& bridgeDesc : alt.bridgeDescs) {
        if (std::find(emittedDescs.begin(), emittedDescs.end(), bridgeDesc) != emittedDescs.end())
            continue;
        emittedDescs.push_back(bridgeDesc);
        cn->methods.push_back(
            buildSam(samName, bridgeDesc, Opcodes::ACC_BRIDGE | Opcodes::ACC_SYNTHETIC));
    }

    return cn;
}

// 待注册的合成类（在 getClasses 遍历结束后统一 addClass/upsertJarResource，避免
// 遍历中改 map 使迭代器失效）。
struct PendingLambdaClass
{
    std::string                internalName;   // e.g. mj2x/lambda/Lambda$0
    std::unique_ptr<ClassNode> node;
};

// 展开单个 lambda indy 站点（无状态 + 捕获统一处理）。返回是否改写成功。
// 失败（非 INVOKESTATIC impl / desc 不符合 captured++sam 布局）保持 indy 原样、交 SSA fail-closed。
//
// indy desc 参数 = 捕获变量（无状态则为空），已在 indy 前压栈；返回类型 = functional interface。
// bsmArgs[0]=samMethodType, [1]=implHandle, [2]=instantiatedMethodType。impl 形参布局
// （javac）= capturedTypes ++ samArgs。
bool expandLambdaSite(CompilerIndex& index, MethodNode* m, InvokeDynamicInsnNode* indy,
                      int synthCounter, std::vector<PendingLambdaClass>& pending)
{
    auto& arena = index.arena();

    const std::string indyDesc      = toStd(indy->desc);
    std::vector<Type> capturedTypes = Type::getArgumentTypes(arena.intern(indyDesc.c_str()));

    // functional interface = indy 返回类型（须是对象类型）。
    const Type ifaceType = Type::getReturnType(arena.intern(indyDesc.c_str()));
    if (ifaceType.getSort() != Type::OBJECT) return false;
    const std::string ifaceInternal = toStd(ifaceType.getInternalName());

    // bsmArgs：[0]=samMethodType(Type), [1]=implMethodHandle(Handle)。
    if (indy->bsmArgs.size() < 2) return false;
    auto* samTypePtr = indy->bsmArgs[0].as<Type>();
    auto* implPtr    = indy->bsmArgs[1].as<Handle>();
    if (!samTypePtr || !implPtr) return false;
    if (samTypePtr->getSort() != Type::METHOD) return false;

    // 支持的 impl handle 形态：静态 / 实例（虚·接口·已提权私有）/ 构造器。
    const int  tag        = implPtr->getTag();
    const bool isStatic   = (tag == Opcodes::H_INVOKESTATIC);
    const bool isCtor     = (tag == Opcodes::H_NEWINVOKESPECIAL);
    const bool isInstance = (tag == Opcodes::H_INVOKEVIRTUAL ||
                             tag == Opcodes::H_INVOKEINTERFACE ||
                             tag == Opcodes::H_INVOKESPECIAL);
    if (!isStatic && !isCtor && !isInstance) return false;   // H_GETFIELD/H_PUTFIELD 等暂不支持

    const std::string samDesc  = toStd(samTypePtr->getDescriptor());
    const std::string implDesc = toStd(implPtr->getDesc());

    // 阶段2e：解析 altMetafactory 尾部（flags/markers/bridges）。metafactory 站点 alt 为空。
    // 解析失败（未知 flag / 形态异常）→ fail-closed。
    AltLambdaInfo alt;
    if (!parseAltMetafactoryTail(indy, alt)) return false;

    // 主 SAM 与各 bridge 描述符逐个校验可适配（阶段2d 适配子集），任一不可适配整站点 fail-closed。
    if (!canEmitSam(arena, samDesc, capturedTypes, *implPtr, isStatic, isCtor)) return false;
    for (const auto& bridgeDesc : alt.bridgeDescs) {
        if (bridgeDesc == samDesc) continue;   // 与主 SAM 重复的 bridge 由 buildSam 去重
        if (!canEmitSam(arena, bridgeDesc, capturedTypes, *implPtr, isStatic, isCtor)) return false;
    }

    // impl 目标若在本 index 的用户类且非 public，提权为 public：合成类位于 mj2x/lambda/
    // 包，跨类/跨包访问 private/protected/包私有目标（含私有实例方法、包私有构造器）都会
    // 触发 IllegalAccessError。库方法（通常已 public）与 JDK 类（找不到）无需处理。
    if (auto* target = findMethodInIndex(index, toStd(implPtr->getOwner()),
                                         toStd(implPtr->getName()), implDesc)) {
        if (!(target->access & Opcodes::ACC_PUBLIC)) {
            target->access &= ~(Opcodes::ACC_PRIVATE | Opcodes::ACC_PROTECTED);
            target->access |= Opcodes::ACC_PUBLIC;
        }
    }

    const std::string synthName = "mj2x/lambda/Lambda$" + std::to_string(synthCounter);

    // 构造器描述符 (capturedDescs)V。
    std::string ctorDesc = "(";
    for (const auto& t : capturedTypes) ctorDesc += toStd(t.getDescriptor());
    ctorDesc += ")V";

    // 站点改写。
    //   无捕获：new synth / dup / invokespecial synth.<init>()V。
    //   有捕获：捕获值已在栈上（栈底=cap0，栈顶=capN-1）。先逆序 xSTORE 到新局部槽腾空栈，
    //     再 new / dup / 逆序? —— 需按构造器形参顺序（cap0..capN-1）重新 xLOAD，
    //     故先全部存槽，再 new/dup，再顺序 load。
    InsnList seq(&m->arena());
    if (capturedTypes.empty()) {
        seq.add(m->create<TypeInsnNode>(Opcodes::NEW, intern(arena, synthName.c_str())));
        seq.add(m->create<InsnNode>(Opcodes::DUP));
        seq.add(m->create<MethodInsnNode>(Opcodes::INVOKESPECIAL, intern(arena, synthName.c_str()),
                                   intern(arena, "<init>"), intern(arena, "()V"), false));
        m->maxStack += 2;
    }
    else {
        // 为捕获值分配局部槽（从 m->maxLocals 起）。
        std::vector<int> capSlot(capturedTypes.size());
        int              slot = m->maxLocals;
        for (size_t i = 0; i < capturedTypes.size(); ++i) {
            capSlot[i] = slot;
            slot += slotSize(capturedTypes[i].getSort());
        }
        // 逆序 xSTORE（栈顶 capN-1 先存）。
        for (size_t r = capturedTypes.size(); r > 0; --r) {
            const size_t i = r - 1;
            seq.add(m->create<VarInsnNode>(storeOpcode(capturedTypes[i].getSort()), capSlot[i]));
        }
        // new / dup / 顺序 xLOAD cap0..capN-1 / invokespecial <init>(capturedDescs)V。
        seq.add(m->create<TypeInsnNode>(Opcodes::NEW, intern(arena, synthName.c_str())));
        seq.add(m->create<InsnNode>(Opcodes::DUP));
        int loadStack = 0;
        for (size_t i = 0; i < capturedTypes.size(); ++i) {
            seq.add(m->create<VarInsnNode>(loadOpcode(capturedTypes[i].getSort()), capSlot[i]));
            loadStack += slotSize(capturedTypes[i].getSort());
        }
        seq.add(m->create<MethodInsnNode>(Opcodes::INVOKESPECIAL, intern(arena, synthName.c_str()),
                                   intern(arena, "<init>"), intern(arena, ctorDesc.c_str()),
                                   false));
        if (slot > m->maxLocals) m->maxLocals = slot;
        // 栈峰值：new+dup(2) + 全部捕获实参。
        m->maxStack += 2 + loadStack;
    }
    // InsnList::remove 只摘链不释放；buildLambdaClassNode 还要读 indy->name，
    // 故先摘链、读完后按 B7 混合清除纪律释放：reader 建树后方法体节点由
    // nodeArena_ 持有（owns 直接跳过），仅外来堆节点需要 delete。
    m->instructions.insert(indy, seq);
    m->instructions.remove(indy);

    PendingLambdaClass pc;
    pc.internalName = synthName;
    pc.node = buildLambdaClassNode(arena, synthName, ifaceInternal, toStd(indy->name), samDesc,
                                   capturedTypes, *implPtr, alt);
    pending.push_back(std::move(pc));
    if (!m->arena().owns(indy)) delete indy;
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
            const int expandedBefore = expanded + lambdaExpanded;
            for (auto* indy : concatSites) {
                bool wc = false;
                isStringConcatSite(indy, wc);
                if (expandConcatSite(arena, m, indy, wc)) ++expanded;
            }
            for (auto* indy : lambdaSites) {
                if (expandLambdaSite(index, m, indy, synthCounter, pendingLambdaClasses)) {
                    ++lambdaExpanded;
                    ++synthCounter;
                }
            }
            // 展开改写了指令流：类必须标记 modified，否则 JAR 输出写回原始字节
            //（OutputWriter 只重写 modified 类，展开静默作废），单类输出走
            // COMPUTE_MAXS 配新指令流有 VerifyError 风险（对齐 ldc_preprocessor
            // 同约束）。native_loader 的 clinit 注入按「modified 且含 ACC_NATIVE
            // 方法」判定，不受本处影响。
            if (expanded + lambdaExpanded > expandedBefore) cls.modified = true;
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
        CA_LOGT_INFO("preprocess", "Expanded {} StringConcatFactory site(s) into StringBuilder bytecode (RegC-native).\n", expanded);
    }
    if (lambdaExpanded > 0) {
        CA_LOGT_INFO("preprocess", "Expanded {} lambda site(s) into synthesized functional-interface class(es) (RegC-native).\n", lambdaExpanded);
    }
    if (remainingIndy > 0) {
        CA_LOGT_INFO("preprocess", "{} invokedynamic site(s) remain unexpanded (primitive narrowing / box+widen / custom bootstrap); StackC lowers via runtime bridge, RegC/SSA still fail-closed until later expansion phases.\n", remainingIndy);
    }
}

}   // namespace

void InvokeDynamicPreprocessor::process(CompilerIndex& index)
{
    processImpl(index);
}

}   // namespace mj2x
