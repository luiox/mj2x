#include "invokedynamic_preprocessor.hpp"

#include "mj2x/core/jvm_opcodes.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/any.hpp"
#include <algorithm>
#include <string>
#include <vector>
#include "libca/log/log_macros.hpp"

#include "mj2x/core/dom_edit.hpp"
#include "mj2x/core/dom_payload.hpp"

namespace mj2x {


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
        // #921 staticArena 退役：仅瞬时比对，函数级局部 arena 零驻留。
        ca::str::Utf8StringArena scratch;
        const std::string        desc = toStd(argType.getDescriptor(scratch));
        if (desc == "Ljava/lang/String;") return "(Ljava/lang/String;)Ljava/lang/StringBuilder;";
        return "(Ljava/lang/Object;)Ljava/lang/StringBuilder;";
    }
    default: return "(Ljava/lang/Object;)Ljava/lang/StringBuilder;";
    }
}

// 在 seq 尾部追加「append 一个字符串字面量」的指令：LDC <str> + INVOKEVIRTUAL
// StringBuilder.append(String)。栈上此前须有 StringBuilder 引用，append 返回自身，
// 栈保持 StringBuilder 顶（链式）。CP 经调用方 session 语义 intern。
void emitAppendStringSeq(jbc::ir::ClassSession& session, std::vector<jbc::ir::Insn>& seq,
                         const std::string& literal)
{
    seq.push_back(jbc::ir::CodeEditor::loadConst(session.internString(literal)));
    jbc::ir::Insn append;
    append.op = jbc::core::Op::INVOKEVIRTUAL;
    append.cp = session.internMethodRef("java/lang/StringBuilder", "append",
                                        "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
    seq.push_back(std::move(append));
}

// 展开单个 makeConcat[WithConstants] indy 站点（DOM 载体）。返回是否改写成功；
// 失败（不支持的形态）保持 indy 原样、交由 SSA fail-closed。
//
// 策略：indy 执行前 N 个动态实参已在操作数栈上（栈底=第1参，栈顶=第N参）。改写为：
//   1. 逆序 xSTORE 到新分配的局部槽（栈顶=最后一个参数先存），清空这些参数；
//   2. new StringBuilder / dup / <init>；
//   3. 按 recipe 从左到右：字面量→append(String)；动态占位→xLOAD 对应槽+append(<type>)；
//      常量占位→LDC/append；
//   4. toString()，栈顶恢复为单个 String（与 indy 返回一致）。
// 单块直线代码，不引入分支/label，StackMapTable 帧不受影响；仅 maxLocals 增大。
// 发射序在预校验通过后逐条 insertBefore(at)（同锚点保序）；预校验保证发射段
// 无失败路径（DOM 逐条插入无整段回滚）。
// srcPc：被替换 indy 的原 pc。替换序列首条继承之——lower 的属性重定位 pcmap
// 以「存活指令的 srcPc」为键，锚在被删指令 pc 上的 LNT/LVT/SMT 行才会继续命中
//（行号语义不变：替换首条就是原 lambda/拼接语句的位置）。
bool expandConcatSiteDom(jbc::ir::ClassSession& session, jbc::ir::CodeUnit& unit,
                         jbc::ir::CodeEditor& editor, jbc::ir::InsnId at, const DomIndySite& site,
                         bool withConstants, ca::u32 srcPc, ca::str::Utf8StringArena& arena)
{
    const std::string indyDesc = site.desc;
    std::vector<Type> argTypes = Type::getArgumentTypes(arena.intern(indyDesc.c_str()));

    // recipe：makeConcatWithConstants 的 bsmArgs[0] 是 recipe；makeConcat 无 recipe，
    // 等价于「全部动态参数顺序拼接」。
    std::string recipe;
    size_t      nextConst = 1;   // withConstants 时常量从 bsmArgs[1] 起
    if (withConstants) {
        if (site.bsmArgs.empty() || !anyAsString(site.bsmArgs[0], recipe)) {
            return false;   // 无 recipe 常量 → 不支持形态
        }
    }
    else {
        // makeConcat：合成全 kTagArg 的 recipe。
        recipe.assign(argTypes.size(), kTagArg);
    }

    // 预校验：recipe 动态占位数与描述符一致、常量占位全部可串化。
    {
        size_t args = 0;
        size_t consts = 0;
        for (char c : recipe) {
            if (c == kTagArg) ++args;
            else if (c == kTagConst) ++consts;
        }
        if (args != argTypes.size()) return false;   // recipe 与描述符不一致
        size_t constIdx = 1;
        for (char c : recipe) {
            if (c != kTagConst) continue;
            if (constIdx >= site.bsmArgs.size()) return false;
            std::string constStr;
            if (!anyAsString(site.bsmArgs[constIdx], constStr)) return false;
            ++constIdx;
        }
    }

    // 为每个动态参数分配局部槽（从当前 maxLocals 起）。
    std::vector<int> argSlot(argTypes.size());
    int              slot = unit.maxLocals;
    for (size_t i = 0; i < argTypes.size(); ++i) {
        argSlot[i] = slot;
        slot += slotSize(argTypes[i].getSort());
    }

    // 展开序列构建后逐条 insertBefore(at)（同锚点保序，CodeDom 句柄域稳定）。
    std::vector<jbc::ir::Insn> seq;

    // 1. 逆序把栈上参数存进局部槽（栈顶是最后一个参数）。
    for (size_t r = argTypes.size(); r > 0; --r) {
        const size_t i = r - 1;
        seq.push_back(jbc::ir::CodeEditor::local(
            static_cast<jbc::core::Op>(storeOpcode(argTypes[i].getSort())),
            static_cast<ca::u32>(argSlot[i])));
    }

    // 2. new StringBuilder / dup / <init>()V。
    {
        jbc::ir::Insn nw;
        nw.op = jbc::core::Op::NEW;
        nw.cp = session.internClass("java/lang/StringBuilder");
        seq.push_back(std::move(nw));
        seq.push_back(jbc::ir::CodeEditor::simple(jbc::core::Op::DUP));
        jbc::ir::Insn init;
        init.op = jbc::core::Op::INVOKESPECIAL;
        init.cp = session.internMethodRef("java/lang/StringBuilder", "<init>", "()V");
        seq.push_back(std::move(init));
    }

    // 3. 遍历 recipe。
    size_t      nextArg = 0;
    std::string pending;   // 累积的字面量文本
    auto        flushPending = [&]() {
        if (!pending.empty()) {
            emitAppendStringSeq(session, seq, pending);
            pending.clear();
        }
    };
    for (char c : recipe) {
        if (c == kTagArg) {
            flushPending();
            const Type& t = argTypes[nextArg];
            seq.push_back(jbc::ir::CodeEditor::local(
                static_cast<jbc::core::Op>(loadOpcode(t.getSort())),
                static_cast<ca::u32>(argSlot[nextArg])));
            jbc::ir::Insn append;
            append.op = jbc::core::Op::INVOKEVIRTUAL;
            append.cp = session.internMethodRef("java/lang/StringBuilder", "append",
                                                appendDesc(t));
            seq.push_back(std::move(append));
            ++nextArg;
        }
        else if (c == kTagConst) {
            flushPending();
            std::string constStr;
            (void)anyAsString(site.bsmArgs[nextConst], constStr);
            emitAppendStringSeq(session, seq, constStr);
            ++nextConst;
        }
        else {
            pending.push_back(c);
        }
    }
    flushPending();

    // 4. toString()。
    {
        jbc::ir::Insn toString;
        toString.op = jbc::core::Op::INVOKEVIRTUAL;
        toString.cp = session.internMethodRef("java/lang/StringBuilder", "toString",
                                              "()Ljava/lang/String;");
        seq.push_back(std::move(toString));
    }

    // 逐条 insertBefore(at)（同锚点保序）。首条回填被删 indy 的 srcPc：CodeDom
    // 插入路径强制 srcPc=kNoPc（appendSlot/insertAt），而 lower 的属性重定位
    // pcmap 以存活指令 srcPc 为键——锚在被删 pc 上的 LNT/LVT/SMT 行靠首条命中
    //（行号语义不变：替换首条即原语句位置）。CodeDom::get 是正门可变访问。
    jbc::ir::InsnId firstPlaced{};
    for (auto& in : seq) {
        const auto op = in.op;
        auto placed = editor.insertBefore(at, std::move(in));
        if (placed.is_err()) {
            // 站点改写部分插入即字节码损坏——显式失败，不静默吞。
            throw std::runtime_error(std::string("invokedynamic_preprocessor: insertBefore "
                                                 "failed (op=")
                                     + std::to_string(static_cast<int>(op)) + ", "
                                     + jbc::ir::to_string(placed.unwrap_err()) + ")");
        }
        if (firstPlaced == jbc::ir::kNoInsn) {
            firstPlaced = std::move(placed).unwrap();
        }
    }
    if (srcPc != jbc::ir::kNoPc && firstPlaced != jbc::ir::kNoInsn) {
        auto mut = unit.dom.get(firstPlaced);
        if (mut.is_ok()) std::move(mut).unwrap()->srcPc = srcPc;
    }

    // maxLocals 增大以容纳新槽；maxStack 需容纳 StringBuilder+dup+参数，保守 +3。
    if (slot > unit.maxLocals) unit.maxLocals = static_cast<ca::u16>(slot);
    unit.maxStack = static_cast<ca::u16>(unit.maxStack + 3);
    return true;
}

// 判断 bsm 是否为 StringConcatFactory 站点，并区分 makeConcat / makeConcatWithConstants。
// 返回 true 时通过 outWithConstants 输出是否带常量。
bool isStringConcatSite(const Handle* bsm, bool& outWithConstants)
{
    if (bsm == nullptr) return false;
    const std::string bsmOwner = toStd(bsm->getOwner());
    const std::string bsmName  = toStd(bsm->getName());
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
    // #921 staticArena 退役：仅瞬时转串，函数级局部 arena 零驻留。
    ca::str::Utf8StringArena scratch;
    return t.getSort() == Type::ARRAY ? toStd(t.getDescriptor(scratch))
                                      : toStd(t.getInternalName(scratch));
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
    // #921 staticArena 退役：仅瞬时比对，函数级局部 arena 零驻留。
    ca::str::Utf8StringArena scratch;
    if (toStd(from.getDescriptor(scratch)) == toStd(to.getDescriptor(scratch))) return true;
    const bool fromRef = isReferenceSort(from.getSort());
    const bool toRef   = isReferenceSort(to.getSort());
    if (fromRef && toRef) return true;                              // CHECKCAST
    if (fromRef && isPrimitiveSort(to.getSort())) return true;      // CHECKCAST 包装类 + 拆箱
    if (isPrimitiveSort(from.getSort()) && toRef) {
        // 装箱目标限 Object / 对应包装类（javac 擦除只产出这两种；其余形态 fail-closed）。
        const std::string toDesc = toStd(to.getDescriptor(scratch));
        return toDesc == "Ljava/lang/Object;" ||
               toDesc == std::string("L") + wrapperInternal(from.getSort()) + ";";
    }
    if (isPrimitiveSort(from.getSort()) && isPrimitiveSort(to.getSort())) {
        // 基本类型互转：仅加宽支持（阶段2f），收窄仍 fail-closed。
        return wideningOpcode(from.getSort(), to.getSort()) != -1;
    }
    return false;
}

// 在 seq 尾部发射「栈顶值 from → to」的适配指令。调用前须先 canAdaptValue 校验。
// CP 经调用方 session 语义 intern（CHECKCAST/NEW 的 Class 条目、拆装箱的方法引用）。
void emitAdaptValueSeq(jbc::ir::ClassSession& session, std::vector<jbc::ir::Insn>& seq,
                       const Type& from, const Type& to)
{
    // #921 staticArena 退役：仅瞬时转串比对，落点=函数级局部 arena。
    ca::str::Utf8StringArena scratch;
    const std::string fromDesc = toStd(from.getDescriptor(scratch));
    const std::string toDesc   = toStd(to.getDescriptor(scratch));
    if (fromDesc == toDesc) return;
    const bool fromRef = isReferenceSort(from.getSort());
    if (fromRef && isReferenceSort(to.getSort())) {
        // 收窄到 Object 无需 cast（一切引用 <: Object）；其余发 CHECKCAST 保证验证器通过。
        if (toDesc != "Ljava/lang/Object;") {
            jbc::ir::Insn cast;
            cast.op = jbc::core::Op::CHECKCAST;
            cast.cp = session.internClass(checkcastName(to));
            seq.push_back(std::move(cast));
        }
        return;
    }
    if (fromRef) {
        // 引用 → 基本类型：CHECKCAST 包装类（已是包装类则免）+ xxxValue() 拆箱。
        const char*       w     = wrapperInternal(to.getSort());
        const std::string wDesc = std::string("L") + w + ";";
        if (fromDesc != wDesc) {
            jbc::ir::Insn cast;
            cast.op = jbc::core::Op::CHECKCAST;
            cast.cp = session.internClass(w);
            seq.push_back(std::move(cast));
        }
        jbc::ir::Insn unbox;
        unbox.op = jbc::core::Op::INVOKEVIRTUAL;
        unbox.cp = session.internMethodRef(w, unboxMethodName(to.getSort()),
                                           unboxMethodDesc(to.getSort()));
        seq.push_back(std::move(unbox));
        return;
    }
    // 基本类型 → 基本类型：加宽（canAdaptValue 已保证是受支持加宽，阶段2f）。
    if (isPrimitiveSort(to.getSort())) {
        seq.push_back(jbc::ir::CodeEditor::simple(
            static_cast<jbc::core::Op>(wideningOpcode(from.getSort(), to.getSort()))));
        return;
    }
    // 基本类型 → 引用：包装类.valueOf 装箱（canAdaptValue 已限定目标为 Object/包装类）。
    const char* w = wrapperInternal(from.getSort());
    jbc::ir::Insn box;
    box.op = jbc::core::Op::INVOKESTATIC;
    box.cp = session.internMethodRef(w, "valueOf", boxValueOfDesc(from.getSort()));
    seq.push_back(std::move(box));
}

// impl 返回值能否适配为 SAM 返回值。
bool canAdaptReturn(const Type& implRet, const Type& samRet)
{
    if (samRet.getSort() == Type::VOID) return true;    // 丢弃返回值（POP/POP2）
    if (implRet.getSort() == Type::VOID) return false;  // SAM 要值而 impl 无值
    return canAdaptValue(implRet, samRet);
}

// 发射 SAM 返回序列：适配栈顶 impl 返回值（或丢弃）后按 samRet 发 xRETURN。
void emitReturnAdaptSeq(jbc::ir::ClassSession& session, std::vector<jbc::ir::Insn>& seq,
                        const Type& implRet, const Type& samRet)
{
    if (samRet.getSort() == Type::VOID) {
        if (implRet.getSort() != Type::VOID) {
            seq.push_back(jbc::ir::CodeEditor::simple(
                slotSize(implRet.getSort()) == 2 ? jbc::core::Op::POP2 : jbc::core::Op::POP));
        }
        seq.push_back(jbc::ir::CodeEditor::simple(jbc::core::Op::RETURN));
        return;
    }
    emitAdaptValueSeq(session, seq, implRet, samRet);
    seq.push_back(
        jbc::ir::CodeEditor::simple(static_cast<jbc::core::Op>(returnOpcode(samRet.getSort()))));
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

// 判断 bsm 是否为 LambdaMetafactory 站点（阶段2 起认 metafactory；阶段2e 起也认
// altMetafactory——后者带 serializable / marker interface / bridge 等额外标志，尾部由
// parseAltMetafactoryTail 解析）。
bool isLambdaMetafactorySite(const Handle* bsm)
{
    if (bsm == nullptr) return false;
    if (toStd(bsm->getOwner()) != "java/lang/invoke/LambdaMetafactory") return false;
    const std::string name = toStd(bsm->getName());
    return name == "metafactory" || name == "altMetafactory";
}

// 解析 altMetafactory 的附加 bsmArgs（[0..2]=sam/impl/instantiated 与 metafactory 同；
// [3]=flags:int，随后按 flag 位跟 markerCount+markers、bridgeCount+bridges）。metafactory
// 站点无尾部，直接返回 true 且 out 为空。任何形态异常（未知 flag 位、缺计数、类型不符）
// 返回 false → 交 SSA fail-closed，绝不静默降级。
bool parseAltMetafactoryTail(const Handle& bsm, const std::vector<ca::core::Any>& bsmArgs,
                             AltLambdaInfo& out)
{
    if (toStd(bsm.getName()) != "altMetafactory") return true;   // metafactory：无尾部
    if (bsmArgs.size() < 4) return false;
    auto* flagsPtr = bsmArgs[3].as<int>();
    if (!flagsPtr) return false;
    const int flags = *flagsPtr;
    // 只认已知 flag 位；出现未知位（如未来扩展）保守 fail-closed。
    if (flags & ~(kAltFlagSerializable | kAltFlagMarkers | kAltFlagBridges)) return false;

    // Serializable 是纯 marker（无方法），合成类默认序列化其 capN 字段即可 round-trip；
    // 不复刻 JVM 原生 SerializedLambda 格式（对混淆输出无意义）。
    if (flags & kAltFlagSerializable) out.markerIfaces.emplace_back("java/io/Serializable");

    size_t pos      = 4;
    auto   readCount = [&](int& cnt) -> bool {
        if (pos >= bsmArgs.size()) return false;
        auto* p = bsmArgs[pos].as<int>();
        if (!p || *p < 0) return false;
        cnt = *p;
        ++pos;
        return true;
    };
    // #921 staticArena 退役：markers/bridges 两分支的内部名/描述符仅瞬时转串，
    // 共用本函数级局部 arena（寿命有界零驻留，裁决①局部 arena 口径）。
    ca::str::Utf8StringArena scratch;
    if (flags & kAltFlagMarkers) {
        int cnt = 0;
        if (!readCount(cnt)) return false;
        for (int i = 0; i < cnt; ++i) {
            if (pos >= bsmArgs.size()) return false;
            auto* mk = bsmArgs[pos].as<Type>();   // CONSTANT_Class → Type(OBJECT)
            if (!mk || mk->getSort() != Type::OBJECT) return false;
            out.markerIfaces.push_back(toStd(mk->getInternalName(scratch)));
            ++pos;
        }
    }
    if (flags & kAltFlagBridges) {
        int cnt = 0;
        if (!readCount(cnt)) return false;
        for (int i = 0; i < cnt; ++i) {
            if (pos >= bsmArgs.size()) return false;
            auto* br = bsmArgs[pos].as<Type>();   // CONSTANT_MethodType → Type(METHOD)
            if (!br || br->getSort() != Type::METHOD) return false;
            out.bridgeDescs.push_back(toStd(br->getDescriptor(scratch)));
            ++pos;
        }
    }
    return true;
}

// 在 index 的非 library 类里按 owner/name/desc 找方法（用于给私有目标方法提权）。
// DOM 读面：遍历期间会话持有调用方的 dom（cls.dom 为空），提权因此延迟到
// processImpl 遍历结束后统一执行（PendingAccessFix）。
jbc::core::MemberInfo* domMethodInIndex(CompilerIndex& index, const std::string& owner,
                                        const std::string& name, const std::string& desc)
{
    auto* cls = index.getClass(owner);
    if (!cls || !cls->dom) return nullptr;
    for (auto& m : cls->dom->methods) {
        if (cls->dom->cp.utf8Equals(m.nameCp, name) && cls->dom->cp.utf8Equals(m.descCp, desc)) {
            return &m;
        }
    }
    return nullptr;
}

/// 延迟执行的 impl 提权（会话开闭期间 cls.dom 被接管，遍历后统一落）。
struct PendingAccessFix
{
    std::string owner;
    std::string name;
    std::string desc;
};

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

// ---- indy native dispatch 静态判定（设计见 doc_ai/indy_native_dispatch_design.md §2）----

/// lambda 站点静态展开前置判定（阶段2a-2f 支持子集）。expandLambdaSite 与
/// classifyIndySite / classifyIndySiteForm 共用同一谓词，保证「判定结果 = 展开行为」
/// 零漂移。失败时经 outReason 输出稳定理由字面量（可为 null）。
bool lambdaExpansionPrecondition(ca::str::Utf8StringArena& arena, const std::string& indyDesc,
                                 const Handle& bsm, const std::vector<ca::core::Any>& bsmArgs,
                                 const char** outReason = nullptr)
{
    std::vector<Type> capturedTypes = Type::getArgumentTypes(arena.intern(indyDesc.c_str()));

    // functional interface = indy 返回类型（须是对象类型）。
    const Type ifaceType = Type::getReturnType(arena.intern(indyDesc.c_str()));
    if (ifaceType.getSort() != Type::OBJECT) {
        if (outReason) *outReason = "lambda-metafactory: indy return type is not an object (functional interface)";
        return false;
    }

    // bsmArgs：[0]=samMethodType(Type), [1]=implMethodHandle(Handle)。
    if (bsmArgs.size() < 2) {
        if (outReason) *outReason = "lambda-metafactory: bsmArgs missing sam/impl";
        return false;
    }
    auto* samTypePtr = bsmArgs[0].as<Type>();
    auto* implPtr    = bsmArgs[1].as<Handle>();
    if (!samTypePtr || !implPtr) {
        if (outReason) *outReason = "lambda-metafactory: bsmArgs[0]/[1] wrong constant type";
        return false;
    }
    if (samTypePtr->getSort() != Type::METHOD) {
        if (outReason) *outReason = "lambda-metafactory: samMethodType is not a MethodType";
        return false;
    }

    // 支持的 impl handle 形态：静态 / 实例（虚·接口·已提权私有）/ 构造器。
    const int  tag        = implPtr->getTag();
    const bool isStatic   = (tag == Opcodes::H_INVOKESTATIC);
    const bool isCtor     = (tag == Opcodes::H_NEWINVOKESPECIAL);
    const bool isInstance = (tag == Opcodes::H_INVOKEVIRTUAL ||
                             tag == Opcodes::H_INVOKEINTERFACE ||
                             tag == Opcodes::H_INVOKESPECIAL);
    if (!isStatic && !isCtor && !isInstance) {
        if (outReason) *outReason = "lambda-metafactory: unsupported impl handle tag";
        return false;
    }

    // #921 staticArena 退役：METHOD 排序 getDescriptor 返回 value_ 不 intern，落点=本函数作业级 arena。
    const std::string samDesc  = toStd(samTypePtr->getDescriptor(arena));
    const std::string implDesc = toStd(implPtr->getDesc());

    // 阶段2e：解析 altMetafactory 尾部（flags/markers/bridges）。metafactory 站点 alt 为空。
    AltLambdaInfo alt;
    if (!parseAltMetafactoryTail(bsm, bsmArgs, alt)) {
        if (outReason)
            *outReason = "lambda-metafactory: altMetafactory tail parse failed (unknown flags / shape)";
        return false;
    }

    // 主 SAM 与各 bridge 描述符逐个校验可适配（阶段2d 适配子集），任一不可适配整站点 fail-closed。
    if (!canEmitSam(arena, samDesc, capturedTypes, *implPtr, isStatic, isCtor)) {
        if (outReason)
            *outReason =
                "lambda-metafactory: unsupported impl adaptation (narrowing / box+widen / layout mismatch)";
        return false;
    }
    for (const auto& bridgeDesc : alt.bridgeDescs) {
        if (bridgeDesc == samDesc) continue;   // 与主 SAM 重复的 bridge 由 buildSam 去重
        if (!canEmitSam(arena, bridgeDesc, capturedTypes, *implPtr, isStatic, isCtor)) {
            if (outReason)
                *outReason =
                    "lambda-metafactory: unsupported bridge adaptation (narrowing / box+widen / layout mismatch)";
            return false;
        }
    }
    return true;
}

/// record（java/lang/runtime/ObjectMethods.bootstrap）站点识别（判定树 C 分支）。
bool isRecordObjectMethodsSite(const Handle* bsm)
{
    return bsm != nullptr && toStd(bsm->getOwner()) == "java/lang/runtime/ObjectMethods" &&
           toStd(bsm->getName()) == "bootstrap";
}

/// ObjectMethods.bootstrap 保留方法名：JDK 内建 equals/hashCode/toString 语义
/// （target 是多组件聚合运算，非单一访问器调用），直派不可证明，保持 helper。
bool isReservedObjectMethodsName(const std::string& name)
{
    return name == "toString" || name == "hashCode" || name == "equals";
}

/// F2 直派访问器匹配：bsmArgs[3..] 中唯一名字等于 indy 名的句柄，且 tag 受支持、
/// 调用布局与 indy 描述符逐位一致（含返回类型精确一致、实例访问器 receiver 描述符
/// 精确等于声明类）。任一不满足 → nullopt（发射端走 helper，宁窄勿宽）。
std::optional<IndyRecordMethods::Accessor> matchRecordAccessor(
    ca::str::Utf8StringArena& arena, const std::string& indyName, const std::string& indyDesc,
    const std::vector<ca::core::Any>& bsmArgs)
{
    if (isReservedObjectMethodsName(indyName)) return std::nullopt;

    std::vector<const Handle*> matches;
    for (size_t i = 3; i < bsmArgs.size(); ++i) {
        auto* h = bsmArgs[i].as<Handle>();
        if (h != nullptr && toStd(h->getName()) == indyName) matches.push_back(h);
    }
    if (matches.size() != 1) return std::nullopt;
    const Handle& handle = *matches[0];

    const int tag = handle.getTag();
    if (tag != Opcodes::H_INVOKESTATIC && tag != Opcodes::H_INVOKEVIRTUAL &&
        tag != Opcodes::H_INVOKEINTERFACE) {
        return std::nullopt;
    }

    const std::string accDesc  = toStd(handle.getDesc());
    const std::string ownerDesc = "L" + toStd(handle.getOwner()) + ";";
    std::vector<Type> accArgs    = Type::getArgumentTypes(arena.intern(accDesc.c_str()));
    std::vector<Type> indyArgs   = Type::getArgumentTypes(arena.intern(indyDesc.c_str()));
    const Type        indyRet    = Type::getReturnType(arena.intern(indyDesc.c_str()));
    const Type        accRet     = Type::getReturnType(arena.intern(accDesc.c_str()));
    // #921 staticArena 退役：METHOD 排序 getDescriptor 返回 value_ 不 intern，落点=本函数作业级 arena。
    if (toStd(indyRet.getDescriptor(arena)) != toStd(accRet.getDescriptor(arena)))
        return std::nullopt;

    const auto argDesc = [&arena](const Type& t) { return toStd(t.getDescriptor(arena)); };
    if (tag == Opcodes::H_INVOKESTATIC) {
        if (indyArgs.size() != accArgs.size()) return std::nullopt;
        for (size_t i = 0; i < accArgs.size(); ++i) {
            if (argDesc(indyArgs[i]) != argDesc(accArgs[i])) return std::nullopt;
        }
    }
    else {
        // 实例访问器：indy 首参 = receiver（描述符精确等于声明类），其余逐位一致。
        if (indyArgs.empty() || indyArgs.size() != accArgs.size() + 1) return std::nullopt;
        if (!isReferenceSort(indyArgs[0].getSort())) return std::nullopt;
        if (argDesc(indyArgs[0]) != ownerDesc) return std::nullopt;
        for (size_t i = 0; i < accArgs.size(); ++i) {
            if (argDesc(indyArgs[i + 1]) != argDesc(accArgs[i])) return std::nullopt;
        }
    }

    IndyRecordMethods::Accessor accessor;
    accessor.tag   = tag;
    accessor.owner = toStd(handle.getOwner());
    accessor.name  = toStd(handle.getName());
    accessor.desc  = accDesc;
    return accessor;
}

/// record bootstrap bsmArgs 形态校验 + F2 载荷提取：[0]=Class（recordClass）、
/// [1]=String（方法名集）、[2]=MethodType（方法描述符集）；其后是 0..n 个访问器句柄。
/// 校验语义与波次一 recordBsmArgsWellFormed 完全一致（reason 锁定不变），载荷中
/// 附带访问器匹配结果（发射期消费）。
bool recordMethodsPayload(ca::str::Utf8StringArena& arena, const std::string& indyName,
                          const std::string& indyDesc, const std::vector<ca::core::Any>& bsmArgs,
                          IndyRecordMethods& out)
{
    if (bsmArgs.size() < 3) return false;
    auto* recordCls = bsmArgs[0].as<Type>();
    if (!recordCls || recordCls->getSort() != Type::OBJECT) return false;
    std::string names;
    if (!anyAsString(bsmArgs[1], names)) return false;
    auto* methodsType = bsmArgs[2].as<Type>();
    if (!methodsType || methodsType->getSort() != Type::METHOD) return false;

    // #921 staticArena 退役：OBJECT 排序 getInternalName 返回 value_ 不 intern，
    // 落点参数按必选口径走本函数作业级 arena。
    out.recordClass = toStd(recordCls->getInternalName(arena));
    out.names       = names;
    out.methodsDesc = toStd(methodsType->getDescriptor(arena));
    out.accessor    = matchRecordAccessor(arena, indyName, indyDesc, bsmArgs);
    return true;
}

/// F1 直派形态检查：indy 调用描述符本身就是字段操作（读 = 参数空[静态]/仅 receiver
/// [实例] 且返回==字段 desc；写 = 值/captured 布局且返回 void；receiver 描述符精确
/// 等于声明类——宁窄勿宽，排除子类型 receiver 的分派差异）。满足即「站点调用 = 字段
/// 访问」，发射端可把站点直接发射为 Get/PutField。
bool lmfFieldHandleDirectShape(ca::str::Utf8StringArena& arena, const std::string& indyDesc,
                               const Handle& impl, const std::vector<Type>& indyArgs)
{
    const std::string fieldDesc = toStd(impl.getDesc());
    const std::string ownerDesc = "L" + toStd(impl.getOwner()) + ";";
    const Type        indyRet   = Type::getReturnType(arena.intern(indyDesc.c_str()));
    // #921 staticArena 退役：落点=本函数作业级 arena 参数。
    const auto        argDesc   = [&arena](const Type& t) { return toStd(t.getDescriptor(arena)); };

    switch (impl.getTag()) {
    case Opcodes::H_GETSTATIC:
        return indyArgs.empty() && toStd(indyRet.getDescriptor(arena)) == fieldDesc;
    case Opcodes::H_GETFIELD:
        return indyArgs.size() == 1 && isReferenceSort(indyArgs[0].getSort()) &&
               argDesc(indyArgs[0]) == ownerDesc && toStd(indyRet.getDescriptor(arena)) == fieldDesc;
    case Opcodes::H_PUTSTATIC:
        return indyArgs.size() == 1 && argDesc(indyArgs[0]) == fieldDesc &&
               indyRet.getSort() == Type::VOID;
    default:   // H_PUTFIELD
        return indyArgs.size() == 2 && isReferenceSort(indyArgs[0].getSort()) &&
               argDesc(indyArgs[0]) == ownerDesc && argDesc(indyArgs[1]) == fieldDesc &&
               indyRet.getSort() == Type::VOID;
    }
}

/// LMF impl 句柄为字段句柄（H_GET/H_PUT × STATIC/FIELD）时的 F1 判定（设计文档 §2 B2/B3）。
/// 返回 false = 非字段句柄（调用方继续 B1 判定）；返回 true 时 outForm/outReason 为最终结论。
/// 两个 DirectFieldAccess 子路径：
///   - 直派形态（indy 描述符即字段操作）→ 载荷 engage（P1），发射端把站点直接发射
///     为字段访问（波次二直派桩的唯一 engaged 路径）；
///   - 波次一精确一致子集（captured++sam lambda 布局，站点=创建 lambda 对象）→
///     form 仍为 DirectFieldAccess（报告侧候选标记，reason 逐字不变），载荷不填，
///     发射端走既有 helper 路径（零回归）。
bool classifyLmfFieldHandle(ca::str::Utf8StringArena& arena, const std::string& indyDesc,
                            const Type& samType, const Handle& impl,
                            const std::vector<Type>& capturedTypes, IndyDispatchForm& outForm,
                            const char*& outReason, std::optional<IndyDirectField>* outPayload)
{
    const int  tag      = impl.getTag();
    const bool isGetter = tag == Opcodes::H_GETSTATIC || tag == Opcodes::H_GETFIELD;
    const bool isSetter = tag == Opcodes::H_PUTSTATIC || tag == Opcodes::H_PUTFIELD;
    if (!isGetter && !isSetter) return false;

    // #921 staticArena 退役：METHOD 排序 getDescriptor 返回 value_ 不 intern，
    // 落点参数按必选口径走本函数作业级 arena。
    const auto        samDescRef = samType.getDescriptor(arena);
    std::vector<Type> samArgs    = Type::getArgumentTypes(arena.intern(toStd(samDescRef).c_str()));
    const Type        samRet     = Type::getReturnType(arena.intern(toStd(samDescRef).c_str()));
    const std::string fieldDesc  = toStd(impl.getDesc());
    const std::string samRetDesc = toStd(samRet.getDescriptor(arena));

    // 直派形态优先：indy 描述符即字段操作 → 判定即承诺，载荷 engage。
    if (lmfFieldHandleDirectShape(arena, indyDesc, impl, capturedTypes)) {
        outForm = IndyDispatchForm::DirectFieldAccess;
        switch (tag) {
        case Opcodes::H_GETSTATIC:
            outReason = "lambda-metafactory field getter (GETSTATIC): direct field access site (descriptor-shaped)";
            break;
        case Opcodes::H_GETFIELD:
            outReason = "lambda-metafactory field getter (GETFIELD): direct field access site (descriptor-shaped)";
            break;
        case Opcodes::H_PUTSTATIC:
            outReason = "lambda-metafactory field setter (PUTSTATIC): direct field access site (descriptor-shaped)";
            break;
        default:
            outReason = "lambda-metafactory field setter (PUTFIELD): direct field access site (descriptor-shaped)";
            break;
        }
        if (outPayload != nullptr) {
            IndyDirectField payload;
            payload.isStatic  = tag == Opcodes::H_GETSTATIC || tag == Opcodes::H_PUTSTATIC;
            payload.isWrite   = isSetter;
            payload.owner     = toStd(impl.getOwner());
            payload.name      = toStd(impl.getName());
            payload.fieldDesc = fieldDesc;
            *outPayload       = std::move(payload);
        }
        return true;
    }

    // 波次一精确一致子集（设计文档 §2 表）：无任何加宽/装箱适配——判定即承诺。
    // #921 staticArena 退役：落点=本函数作业级 arena 参数。
    const bool ok =
        isGetter
            ? (tag == Opcodes::H_GETSTATIC
                   ? samArgs.empty() && capturedTypes.empty() && samRetDesc == fieldDesc
                   : capturedTypes.size() == 1 && isReferenceSort(capturedTypes[0].getSort()) &&
                         samArgs.empty() && samRetDesc == fieldDesc)
            : (tag == Opcodes::H_PUTSTATIC
                   ? samArgs.size() == 1 && toStd(samArgs[0].getDescriptor(arena)) == fieldDesc &&
                         capturedTypes.empty() && samRet.getSort() == Type::VOID
                   : capturedTypes.size() == 1 && isReferenceSort(capturedTypes[0].getSort()) &&
                         samArgs.size() == 1 &&
                         toStd(samArgs[0].getDescriptor(arena)) == fieldDesc &&
                         samRet.getSort() == Type::VOID);

    if (ok) {
        outForm = IndyDispatchForm::DirectFieldAccess;
        switch (tag) {
        case Opcodes::H_GETSTATIC:
            outReason = "lambda-metafactory field getter (GETSTATIC): direct field access candidate";
            break;
        case Opcodes::H_GETFIELD:
            outReason = "lambda-metafactory field getter (GETFIELD): direct field access candidate";
            break;
        case Opcodes::H_PUTSTATIC:
            outReason = "lambda-metafactory field setter (PUTSTATIC): direct field access candidate";
            break;
        default:
            outReason = "lambda-metafactory field setter (PUTFIELD): direct field access candidate";
            break;
        }
    }
    else {
        outForm   = IndyDispatchForm::RuntimeHelper;
        outReason = "lambda-metafactory field handle impl outside strict direct-access subset";
    }
    return true;
}

/// 判定树主体（设计文档 §2）的站点数据级核心：A concat → B LMF → C record → D 自定义
/// bootstrap。只读站点自持数据（indy 名/描述符/bsm/bsmArgs），不改任何状态；结论由
/// 展开行为零漂移谓词支撑（B1 共用前置）。classifyIndySite（报告）与
/// classifyIndySiteForm（SSA 载荷填充）共用本核心——判定单源。
/// customBootstrapReason：D 分支理由（D1/D2 细分需要 CompilerIndex，由调用方提供）。
struct CoreDecision
{
    IndyDispatchForm form   = IndyDispatchForm::RuntimeHelper;
    const char*      reason = "";
    std::optional<IndyDirectField>   directField;
    std::optional<IndyRecordMethods> recordMethods;
};

CoreDecision classifyCore(ca::str::Utf8StringArena& arena, const std::string& indyName,
                          const std::string& indyDesc, const Handle* bsm,
                          const std::vector<ca::core::Any>& bsmArgs,
                          const char* customBootstrapReason)
{
    CoreDecision d;

    if (bsm == nullptr) {
        d.reason = "invokedynamic without bootstrap metadata";
        return d;   // fail-closed
    }

    // A：StringConcatFactory（阶段1 静态展开）。
    bool withConstants = false;
    if (isStringConcatSite(bsm, withConstants)) {
        d.form   = IndyDispatchForm::StaticExpanded;
        d.reason = withConstants
                       ? "string-concat: makeConcatWithConstants expanded to StringBuilder bytecode by phase-1"
                       : "string-concat: makeConcat expanded to StringBuilder bytecode by phase-1";
        return d;
    }

    // B：LambdaMetafactory（B1 可展开子集 / B2-B3 字段直派 / B4 回退）。
    if (isLambdaMetafactorySite(bsm)) {
        std::vector<Type> capturedTypes = Type::getArgumentTypes(arena.intern(indyDesc.c_str()));

        if (bsmArgs.size() >= 2) {
            auto* samTypePtr = bsmArgs[0].as<Type>();
            auto* implPtr    = bsmArgs[1].as<Handle>();
            if (samTypePtr && samTypePtr->getSort() == Type::METHOD && implPtr) {
                IndyDispatchForm fieldForm   = IndyDispatchForm::RuntimeHelper;
                const char*      fieldReason = nullptr;
                if (classifyLmfFieldHandle(arena, indyDesc, *samTypePtr, *implPtr, capturedTypes,
                                           fieldForm, fieldReason, &d.directField)) {
                    // 字段句柄 impl：无论是否精确子集都就此定论（展开子集不含字段句柄）。
                    d.form   = fieldForm;
                    d.reason = fieldReason;
                    return d;
                }
                d.directField.reset();   // 非字段句柄：继续 B1 判定，不携带部分载荷
            }
        }

        const char* failReason = nullptr;
        if (lambdaExpansionPrecondition(arena, indyDesc, *bsm, bsmArgs, &failReason)) {
            d.form   = IndyDispatchForm::StaticExpanded;
            d.reason = "lambda-metafactory: expandable by phase-2 (synthesized impl class)";
        }
        else {
            d.form   = IndyDispatchForm::RuntimeHelper;
            d.reason = failReason;
        }
        return d;
    }

    // C：record ObjectMethods.bootstrap（F2 静态可命名 target）。
    if (isRecordObjectMethodsSite(bsm)) {
        IndyRecordMethods payload;
        if (recordMethodsPayload(arena, indyName, indyDesc, bsmArgs, payload)) {
            d.form   = IndyDispatchForm::RecordObjectMethods;
            d.reason = "record ObjectMethods.bootstrap: statically nameable accessor targets";
            d.recordMethods = std::move(payload);
        }
        else {
            d.form   = IndyDispatchForm::RuntimeHelper;
            d.reason = "record ObjectMethods.bootstrap: malformed bsmArgs shape";
        }
        return d;
    }

    // D：自定义 bootstrap（T2；D1/D2 细分仅体现在 reason——由调用方按 index 提供）。
    d.form   = IndyDispatchForm::RuntimeHelper;
    d.reason = customBootstrapReason;
    return d;
}

/// 判定主体（载荷版）：同一核心 + 站点定位由调用方补齐。DOM/树两载体共用。
IndyDispatchDecision classifyIndySitePayloadImpl(CompilerIndex& index, const std::string& indyName,
                                             const std::string& indyDesc,
                                             const Handle&                     bsm,
                                             const std::vector<ca::core::Any>& bsmArgs)
{
    IndyDispatchDecision d;
    d.indyName = indyName;
    d.indyDesc = indyDesc;

    // D1/D2 理由细分需要编译输入类集合（仅影响 reason，不影响 form）。
    auto*  bsmOwnerCls = index.getClass(toStd(bsm.getOwner()));
    const char* customReason =
        bsmOwnerCls != nullptr && !bsmOwnerCls->isLibrary
            ? "custom bootstrap: bsm body in compilation input (static evaluation candidate, wave-2)"
            : "custom bootstrap: bsm owner external (runtime crack candidate only)";

    const CoreDecision core = classifyCore(index.arena(), indyName, indyDesc, &bsm, bsmArgs,
                                           customReason);
    d.form          = core.form;
    d.reason        = core.reason;
    d.directField   = core.directField;
    d.recordMethods = core.recordMethods;
    return d;
}

// 合成一个实现 functional interface 的类（DOM 载体，从零建 libjbc ClassFile）：
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
// 方法体经 session addMethod+CodeEditor.append+lowerMethod 发射（库编码路径），
// 成品经 writeClass→readClass 字节往返还回（真实编解码器纪律，同 dom_edit）。
std::unique_ptr<jbc::core::ClassFile> buildLambdaClassDom(ca::str::Utf8StringArena& arena,
                                                const std::string& synthName,
                                                const std::string& ifaceInternal,
                                                const std::string& samName,
                                                const std::string&       samDesc,
                                                const std::vector<Type>& capturedTypes,
                                                const Handle&            impl,
                                                const AltLambdaInfo&     alt)
{
    auto cf     = std::make_unique<jbc::core::ClassFile>();
    cf->major   = 52;   // V1_8
    cf->access  = Opcodes::ACC_PUBLIC | Opcodes::ACC_FINAL | Opcodes::ACC_SUPER
                | Opcodes::ACC_SYNTHETIC;
    {
        auto& cp       = cf->cp;
        cf->thisClass  = cp.addClass(cp.addUtf8(synthName));
        cf->superClass = cp.addClass(cp.addUtf8("java/lang/Object"));
        cf->interfaces.push_back(cp.addClass(cp.addUtf8(ifaceInternal)));
        // 阶段2e：altMetafactory 追加的 marker 接口（含 Serializable / 交集类型的额外接口）。
        // 均为无方法 marker，SAM 体不受影响，只满足 instanceof / 序列化标记语义。
        for (const auto& marker : alt.markerIfaces) {
            cf->interfaces.push_back(cp.addClass(cp.addUtf8(marker)));
        }
    }

    // 捕获字段 cap0..capN-1（private final）+ 构造器描述符 (capturedDescs)V。
    std::vector<std::string> capName(capturedTypes.size());
    std::vector<std::string> capDesc(capturedTypes.size());
    std::string              ctorDesc = "(";
    for (size_t i = 0; i < capturedTypes.size(); ++i) {
        capName[i] = "cap" + std::to_string(i);
        // #921 staticArena 退役：仅瞬时转串。
        ca::str::Utf8StringArena scratch;
        capDesc[i] = toStd(capturedTypes[i].getDescriptor(scratch));
        ctorDesc += capDesc[i];
        jbc::core::MemberInfo field;
        field.access = Opcodes::ACC_PRIVATE | Opcodes::ACC_FINAL;
        field.nameCp = cf->cp.addUtf8(capName[i]);
        field.descCp = cf->cp.addUtf8(capDesc[i]);
        cf->fields.push_back(std::move(field));
    }
    ctorDesc += ")V";

    auto opened = jbc::ir::makeSession(std::move(cf));
    if (opened.is_err()) {
        throw std::runtime_error("invokedynamic_preprocessor: lambda synth session failed for "
                                 + synthName);
    }
    auto ses = std::move(opened).unwrap();

    // 方法体发射（骨架已建、体为空 → 逐条 append 保序）。
    auto emitBody = [&](jbc::ir::CodeUnit& unit, std::vector<jbc::ir::Insn> seq) {
        jbc::ir::CodeEditor editor(unit);
        for (auto& in : seq) {
            auto added = editor.append(std::move(in));
            if (added.is_err()) {
                throw std::runtime_error("invokedynamic_preprocessor: lambda synth append failed for "
                                         + synthName);
            }
        }
    };

    // <init>(capturedDescs)V : super.<init>() 后逐个 aload_0 / xload param / putfield capI。
    int ctorSlot     = 1;   // slot 0 = this
    int ctorMaxStack = 1;
    for (size_t i = 0; i < capturedTypes.size(); ++i) {
        ctorSlot += slotSize(capturedTypes[i].getSort());
        // putfield 峰值：this(1) + value(1或2)。
        ctorMaxStack = std::max(ctorMaxStack, 1 + slotSize(capturedTypes[i].getSort()));
    }
    {
        auto added = ses->addMethod(jbc::ir::MethodSeed{"<init>", ctorDesc,
                                                        static_cast<ca::u16>(Opcodes::ACC_PUBLIC),
                                                        true, static_cast<ca::u16>(ctorMaxStack),
                                                        static_cast<ca::u16>(ctorSlot)});
        if (added.is_err()) {
            throw std::runtime_error("invokedynamic_preprocessor: lambda synth addMethod <init> failed for "
                                     + synthName);
        }
        const ca::usize mi = ses->backing().methods.size() - 1;
        auto            promoted = jbc::ir::promoteMethod(*ses, mi);
        if (promoted.is_err()) {
            throw std::runtime_error("invokedynamic_preprocessor: lambda synth promote <init> failed for "
                                     + synthName);
        }
        auto unit = std::move(promoted).unwrap();

        std::vector<jbc::ir::Insn> seq;
        seq.push_back(jbc::ir::CodeEditor::local(jbc::core::Op::ALOAD, 0));
        jbc::ir::Insn superInit;
        superInit.op = jbc::core::Op::INVOKESPECIAL;
        superInit.cp = ses->internMethodRef("java/lang/Object", "<init>", "()V");
        seq.push_back(std::move(superInit));
        int slot = 1;
        for (size_t i = 0; i < capturedTypes.size(); ++i) {
            seq.push_back(jbc::ir::CodeEditor::local(jbc::core::Op::ALOAD, 0));
            seq.push_back(jbc::ir::CodeEditor::local(
                static_cast<jbc::core::Op>(loadOpcode(capturedTypes[i].getSort())),
                static_cast<ca::u32>(slot)));
            jbc::ir::Insn put;
            put.op = jbc::core::Op::PUTFIELD;
            put.cp = ses->internFieldRef(synthName, capName[i], capDesc[i]);
            seq.push_back(std::move(put));
            slot += slotSize(capturedTypes[i].getSort());
        }
        seq.push_back(jbc::ir::CodeEditor::simple(jbc::core::Op::RETURN));
        emitBody(*unit, std::move(seq));

        auto lowered = jbc::ir::lowerMethod(*ses, mi);
        if (lowered.is_err()) {
            throw std::runtime_error("invokedynamic_preprocessor: lambda synth lower <init> failed for "
                                     + synthName);
        }
    }

    // SAM 方法体：把 fullArgs = 捕获字段（GETFIELD）+ SAM 参数（xLOAD）按序压栈，每个值
    // 压栈后立即按 impl 期望类型适配（阶段2d：CHECKCAST / 拆箱 / 装箱），
    // 再按 impl 的 handle tag 发射调用。实例方法引用的 receiver 天然是 fullArgs 的首元（bound
    // 时来自捕获字段、unbound 时来自 SAM 首参），故压栈顺序与静态调用完全一致，只有调用
    // 指令不同。构造器引用需先 NEW/DUP 目标类型再压 fullArgs。
    const int         tag       = impl.getTag();
    const bool        isCtorRef = (tag == Opcodes::H_NEWINVOKESPECIAL);
    const std::string implOwner = toStd(impl.getOwner());
    const std::string implName  = toStd(impl.getName());
    const std::string implDesc  = toStd(impl.getDesc());

    // fullArgs 各位置的目标类型：实例引用首元是 receiver（owner 对象类型），其余对应 implArgs。
    std::vector<Type> implArgs = Type::getArgumentTypes(arena.intern(implDesc.c_str()));
    std::vector<Type> toTypes;
    if (!isCtorRef && (tag == Opcodes::H_INVOKEVIRTUAL || tag == Opcodes::H_INVOKEINTERFACE
                       || tag == Opcodes::H_INVOKESPECIAL)) {
        toTypes.push_back(Type::getObjectType(arena.intern(implOwner.c_str())));
    }
    toTypes.insert(toTypes.end(), implArgs.begin(), implArgs.end());

    // 发射一个 SAM 体（主 SAM 与各 bridge 复用）：按给定描述符 sDesc 载入 fullArgs、适配、
    // 按 impl handle tag 发射调用。extraAccess 供 bridge 加 ACC_BRIDGE|ACC_SYNTHETIC。
    auto buildSam = [&](const std::string& sName, const std::string& sDesc, int extraAccess) {
        int    samStack = 0;
        size_t argPos   = 0;   // fullArgs 位置游标（捕获段 + SAM 参数段共用）

        std::vector<jbc::ir::Insn> seq;
        // 构造器引用：先 NEW/DUP 目标类型（占 2 个栈位，位于 fullArgs 之下）。
        if (isCtorRef) {
            jbc::ir::Insn nw;
            nw.op = jbc::core::Op::NEW;
            nw.cp = ses->internClass(implOwner);
            seq.push_back(std::move(nw));
            seq.push_back(jbc::ir::CodeEditor::simple(jbc::core::Op::DUP));
            samStack += 2;
        }

        // fullArgs 第一段：捕获字段 cap0..capN-1（GETFIELD + 适配）。
        for (size_t i = 0; i < capturedTypes.size(); ++i, ++argPos) {
            seq.push_back(jbc::ir::CodeEditor::local(jbc::core::Op::ALOAD, 0));
            jbc::ir::Insn get;
            get.op = jbc::core::Op::GETFIELD;
            get.cp = ses->internFieldRef(synthName, capName[i], capDesc[i]);
            seq.push_back(std::move(get));
            emitAdaptValueSeq(*ses, seq, capturedTypes[i], toTypes[argPos]);
            // 适配可能改变栈宽（如 Object→long 1→2），按两侧较大者计。
            samStack += std::max(slotSize(capturedTypes[i].getSort()),
                                 slotSize(toTypes[argPos].getSort()));
        }
        // fullArgs 第二段：SAM 参数（实例方法 slot 0 = this，参数从 slot 1 起；xLOAD + 适配）。
        std::vector<Type> samArgs = Type::getArgumentTypes(arena.intern(sDesc.c_str()));
        int               slot    = 1;
        for (const auto& t : samArgs) {
            seq.push_back(jbc::ir::CodeEditor::local(
                static_cast<jbc::core::Op>(loadOpcode(t.getSort())), static_cast<ca::u32>(slot)));
            emitAdaptValueSeq(*ses, seq, t, toTypes[argPos]);
            slot += slotSize(t.getSort());
            samStack += std::max(slotSize(t.getSort()), slotSize(toTypes[argPos].getSort()));
            ++argPos;
        }

        const Type samRet = Type::getReturnType(arena.intern(sDesc.c_str()));
        if (isCtorRef) {
            // NEW/DUP/fullArgs/INVOKESPECIAL <init> → 栈顶留下新建对象，适配（一般免 cast）后 ARETURN。
            jbc::ir::Insn initCall;
            initCall.op = jbc::core::Op::INVOKESPECIAL;
            initCall.cp = ses->internMethodRef(implOwner, "<init>", implDesc);
            seq.push_back(std::move(initCall));
            emitReturnAdaptSeq(*ses, seq, Type::getObjectType(arena.intern(implOwner.c_str())),
                               samRet);
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
            jbc::ir::Insn call;
            call.op = static_cast<jbc::core::Op>(invokeOpcode);
            call.cp = invokeIface ? ses->internInterfaceMethodRef(implOwner, implName, implDesc)
                                  : ses->internMethodRef(implOwner, implName, implDesc);
            seq.push_back(std::move(call));
            emitReturnAdaptSeq(*ses, seq, Type::getReturnType(arena.intern(implDesc.c_str())),
                               samRet);
        }
        // 栈峰值另须容纳返回值（如零参 SAM 调 ()J impl：参数段 0 栈位、返回 2 栈位）。
        const Type implRetT =
            isCtorRef ? Type::getObjectType(arena.intern(implOwner.c_str()))
                      : Type::getReturnType(arena.intern(implDesc.c_str()));
        const int retSlots =
            std::max(implRetT.getSort() == Type::VOID ? 0 : slotSize(implRetT.getSort()),
                     samRet.getSort() == Type::VOID ? 0 : slotSize(samRet.getSort()));
        const int maxStack  = std::max({samStack, retSlots, 1});
        const int maxLocals = slot;

        auto added = ses->addMethod(jbc::ir::MethodSeed{
            sName, sDesc,
            static_cast<ca::u16>(Opcodes::ACC_PUBLIC | extraAccess), true,
            static_cast<ca::u16>(maxStack), static_cast<ca::u16>(maxLocals)});
        if (added.is_err()) {
            throw std::runtime_error("invokedynamic_preprocessor: lambda synth addMethod " + sName
                                     + " failed for " + synthName);
        }
        const ca::usize mi       = ses->backing().methods.size() - 1;
        auto            promoted = jbc::ir::promoteMethod(*ses, mi);
        if (promoted.is_err()) {
            throw std::runtime_error("invokedynamic_preprocessor: lambda synth promote " + sName
                                     + " failed for " + synthName);
        }
        emitBody(*std::move(promoted).unwrap(), std::move(seq));
        auto lowered = jbc::ir::lowerMethod(*ses, mi);
        if (lowered.is_err()) {
            throw std::runtime_error("invokedynamic_preprocessor: lambda synth lower " + sName
                                     + " failed for " + synthName);
        }
    };

    // 主 SAM。
    buildSam(samName, samDesc, 0);
    // 阶段2e：altMetafactory 桥接方法（同名不同签，通常是擦除到更泛的描述符）。逐个发射
    // ACC_BRIDGE|ACC_SYNTHETIC 方法，body 复用同一 impl 适配路径。去重并跳过与主 SAM 重复的
    // 描述符，避免出现同名同签的重复方法。
    std::vector<std::string> emittedDescs{samDesc};
    for (const auto& bridgeDesc : alt.bridgeDescs) {
        if (std::find(emittedDescs.begin(), emittedDescs.end(), bridgeDesc) != emittedDescs.end())
            continue;
        emittedDescs.push_back(bridgeDesc);
        buildSam(samName, bridgeDesc, Opcodes::ACC_BRIDGE | Opcodes::ACC_SYNTHETIC);
    }

    // writeClass→readClass 字节往返还回（真实编解码器纪律，与 dom_edit 同通道；
    // ClassFile 不可移动——AttrSlot lazy span 指向 source_，移动即悬垂）。
    auto written = jbc::core::writeClass(ses->backing());
    if (written.is_err()) {
        throw std::runtime_error("invokedynamic_preprocessor: lambda synth writeClass failed for "
                                 + synthName + ": "
                                 + jbc::core::to_string(std::move(written).unwrap_err()));
    }
    auto bytes = std::move(written).unwrap();
    auto read  = jbc::core::readClass(jbc::core::ByteView{bytes.data(), bytes.size()});
    if (read.is_err()) {
        throw std::runtime_error("invokedynamic_preprocessor: lambda synth readClass failed for "
                                 + synthName);
    }
    return std::move(read).unwrap().file;
}

// 待注册的合成类（在 getClasses 遍历结束后统一 addClass/upsertJarResource，避免
// 遍历中改 map 使迭代器失效）。DOM 载体：合成类直接持 libjbc ClassFile
//（批 6d：树投影退役，合成类经 refreshMethodSnapshots 入 index）。
struct PendingLambdaClass
{
    std::string                           internalName;   // e.g. mj2x/lambda/Lambda$0
    std::unique_ptr<jbc::core::ClassFile> dom;
};

/// 合成 lambda 类宿主包判定（#1182，评审收口 P1 修：default 包返回空串）。
///
/// JVM 对「类实现接口」的访问检查只看接口类自身的 ACC 标志（嵌套仅命名，nest 只管
/// private 成员；javac 嵌套类类文件不写 ACC_PRIVATE，protected 写 ACC_PUBLIC）：
/// 合成类落在固定 mj2x/lambda 包时，实现包私有 SAM 必然在 defineClass 阶段抛
/// IllegalAccessError → FindClass 返回 NULL（异常挂起）→ 生成码 NULL 守卫把 op 落
/// NULL → lambda 包装 invoke 全 NPE。
///
/// javac 的 hidden-class 语义 = 定义在调用方可见的包里。等价规则（只动编译输入可判定
/// 的形态，其余零漂移）：
/// - SAM 在编译输入且非 public（包私有/private 嵌套）→ 落 SAM 同包。private 嵌套
///   实测同包落位即运行全绿（评审实测：类级访问不查 nest + 类文件无 ACC_PRIVATE +
///   impl 提权既有覆盖）；protected 按 javac 落盘形态即 public，谓词不触发（死分支）；
/// - SAM 不在输入（库类型）→ 维持 mj2x/lambda：跨包引用的库 SAM 只可能 public；
///   同包包私有库 SAM 属 split-package 罕态且现状即坏，不为此牺牲零漂移面。
///
/// 返回宿主包（包内带尾随 '/'；default 包=空串；"mj2x/lambda/" = 维持现状）。
/// 注意本修复是生成器侧：已发布产物遇包私有 SAM 不会自动治愈，需重转换。
std::string pickLambdaSynthHostPackage(CompilerIndex& index, const std::string& ifaceInternal)
{
    if (auto* ifaceCls = index.getClass(ifaceInternal)) {
        if (ifaceCls->dom != nullptr && (ifaceCls->dom->access & Opcodes::ACC_PUBLIC) == 0) {
            const std::size_t slash = ifaceInternal.rfind('/');
            return slash == std::string::npos ? std::string()
                                              : ifaceInternal.substr(0, slash + 1);
        }
        return "mj2x/lambda/";
    }
    // SAM 不在 index（单类输入时嵌套接口是独立 class 文件、库/JDK 接口）：
    // 若是**嵌套类型**且其外围类在 index，查外围类 InnerClasses 属性拿 SAM
    // 的真实访问位——包私有时 mj2x/lambda 落点会 IllegalAccessError（合成类
    // 跨包实现不可访问接口，java_corpus LambdasCapture 首曝：L1$Op 包私有、
    // 单类输入不在 index → synth 落 mj2x/lambda → 链接失败）。非嵌套（库/
    // JDK 顶层 SAM）或访问位未知 → 维持 mj2x/lambda 零漂移（非 public 的
    // 顶层类无法被跨包引用，引用方对其访问必已由 public 保证）。
    for (std::size_t dollar = ifaceInternal.find('$'); dollar != std::string::npos;
         dollar     = ifaceInternal.find('$', dollar + 1)) {
        auto* outerCls = index.getClass(ifaceInternal.substr(0, dollar));
        if (outerCls == nullptr || outerCls->dom == nullptr) continue;
        auto& outerDom = *outerCls->dom;
        for (auto& slot : outerDom.attrs) {
            if (slot.body == nullptr) {
                // 类级旁路槽 lazy 解码（失败即放弃嵌套判定，维持 mj2x/lambda 零漂移）。
                if (outerDom.decodeAttr(slot).is_err()) continue;
            }
            if (slot.body == nullptr ||
                slot.body->kind() != jbc::core::AttrKind::InnerClasses) {
                continue;
            }
            const auto& icAttr = *ca::core::tag_cast::cast<jbc::core::InnerClassesAttr>(slot.body.get());
            for (const auto& row : icAttr.rows) {
                if (std::string(outerDom.className(row.innerClassInfoCp)) != ifaceInternal) {
                    continue;
                }
                if ((row.accessFlags & Opcodes::ACC_PUBLIC) == 0) {
                    const std::size_t slash = ifaceInternal.rfind('/');
                    return slash == std::string::npos ? std::string()
                                                      : ifaceInternal.substr(0, slash + 1);
                }
                return "mj2x/lambda/";
            }
        }
    }
    return "mj2x/lambda/";
}

// 展开单个 lambda indy 站点（DOM 载体，无状态 + 捕获统一处理）。返回是否改写成功。
// 失败（非 INVOKESTATIC impl / desc 不符合 captured++sam 布局）保持 indy 原样、交 SSA fail-closed。
//
// indy desc 参数 = 捕获变量（无状态则为空），已在 indy 前压栈；返回类型 = functional interface。
// bsmArgs[0]=samMethodType, [1]=implHandle, [2]=instantiatedMethodType。impl 形参布局
// （javac）= capturedTypes ++ samArgs。
bool expandLambdaSiteDom(CompilerIndex& index, jbc::ir::ClassSession& session,
                         jbc::ir::CodeUnit& unit, jbc::ir::CodeEditor& editor, jbc::ir::InsnId at,
                         const DomIndySite& site, ca::u32 srcPc, int& synthCounter,
                         std::vector<PendingLambdaClass>&  pending,
                         std::vector<PendingAccessFix>&    pendingAccessFixes)
{
    // 前置判定与 classifyIndySite 共用同一谓词（判定=展开零漂移，设计文档 §2/§5）。
    // 不支持的形态（收窄 / 装箱+加宽 / 未知 flags / 字段句柄等）保持 indy 原样、交 SSA fail-closed。
    if (!lambdaExpansionPrecondition(index.arena(), site.desc, site.bsm, site.bsmArgs))
        return false;

    auto& arena = index.arena();

    const std::string indyDesc      = site.desc;
    std::vector<Type> capturedTypes = Type::getArgumentTypes(arena.intern(indyDesc.c_str()));

    // functional interface = indy 返回类型（谓词已保证是对象类型）。
    const Type ifaceType = Type::getReturnType(arena.intern(indyDesc.c_str()));
    // #921 staticArena 退役：OBJECT 排序 getInternalName 返回 value_ 不 intern，落点同上。
    const std::string ifaceInternal = toStd(ifaceType.getInternalName(arena));

    // bsmArgs：[0]=samMethodType(Type), [1]=implMethodHandle(Handle)（谓词已保证形态）。
    auto* samTypePtr = site.bsmArgs[0].as<Type>();
    auto* implPtr    = site.bsmArgs[1].as<Handle>();

    // #921 staticArena 退役：METHOD 排序 getDescriptor 返回 value_ 不 intern，落点=本函数作业级 arena。
    const std::string samDesc  = toStd(samTypePtr->getDescriptor(arena));
    const std::string implDesc = toStd(implPtr->getDesc());

    // 阶段2e：重新解析 altMetafactory 尾部（谓词已保证解析通过；防御性保留检查）。
    AltLambdaInfo alt;
    if (!parseAltMetafactoryTail(site.bsm, site.bsmArgs, alt)) return false;

    // impl 目标若在本 index 的用户类且非 public，提权为 public：合成类位于 mj2x/lambda/
    // 包，跨类/跨包访问 private/protected/包私有目标（含私有实例方法、包私有构造器）都会
    // 触发 IllegalAccessError。库方法（通常已 public）与 JDK 类（找不到）无需处理。
    // 会话开闭期间 cls.dom 被 session 接管，提权延迟到 processImpl 遍历结束统一落。
    pendingAccessFixes.push_back(
        PendingAccessFix{toStd(implPtr->getOwner()), toStd(implPtr->getName()), implDesc});

    // 宿主包判定（#1182：编译输入的非 public SAM 落 SAM 同包，其余维持 mj2x/lambda
    // 零漂移）。宿主包只读类 access/InnerClasses 静态位——树在 DOM 载体 pass 期间
    // 是稳定投影（syncDomToTree 在段尾才重建），树读零漂移。
    // 同名类防碰撞（用户类与库类一并算占用，合成类绝不顶替既有条目）：
    // 探测前跳；成功后把计数器推到选中名之后（出参语义），同 run 后续站点不会再
    // 探到本站点占用名。
    const std::string hostPkg = pickLambdaSynthHostPackage(index, ifaceInternal);
    std::string synthName;
    int         n = synthCounter;
    for (;; ++n) {
        synthName = hostPkg + "Lambda$" + std::to_string(n);
        if (index.getClass(synthName) == nullptr) break;
    }
    synthCounter = n + 1;

    // 构造器描述符 (capturedDescs)V。
    std::string ctorDesc = "(";
    for (const auto& t : capturedTypes) ctorDesc += toStd(t.getDescriptor(arena));   // #921
    ctorDesc += ")V";

    // 站点改写（DOM 发射序逐条 insertBefore(at)，同锚点保序）。
    //   无捕获：new synth / dup / invokespecial synth.<init>()V。
    //   有捕获：捕获值已在栈上（栈底=cap0，栈顶=capN-1）。先逆序 xSTORE 到新局部槽腾空栈，
    //     再 new / dup，再按构造器形参顺序（cap0..capN-1）重新 xLOAD。
    std::vector<jbc::ir::Insn> seq;
    if (capturedTypes.empty()) {
        jbc::ir::Insn nw;
        nw.op = jbc::core::Op::NEW;
        nw.cp = session.internClass(synthName);
        seq.push_back(std::move(nw));
        seq.push_back(jbc::ir::CodeEditor::simple(jbc::core::Op::DUP));
        jbc::ir::Insn init;
        init.op = jbc::core::Op::INVOKESPECIAL;
        init.cp = session.internMethodRef(synthName, "<init>", "()V");
        seq.push_back(std::move(init));
        unit.maxStack = static_cast<ca::u16>(unit.maxStack + 2);
    }
    else {
        // 为捕获值分配局部槽（从当前 maxLocals 起）。
        std::vector<int> capSlot(capturedTypes.size());
        int              slot = unit.maxLocals;
        for (size_t i = 0; i < capturedTypes.size(); ++i) {
            capSlot[i] = slot;
            slot += slotSize(capturedTypes[i].getSort());
        }
        // 逆序 xSTORE（栈顶 capN-1 先存）。
        for (size_t r = capturedTypes.size(); r > 0; --r) {
            const size_t i = r - 1;
            seq.push_back(jbc::ir::CodeEditor::local(
                static_cast<jbc::core::Op>(storeOpcode(capturedTypes[i].getSort())),
                static_cast<ca::u32>(capSlot[i])));
        }
        // new / dup / 顺序 xLOAD cap0..capN-1 / invokespecial <init>(capturedDescs)V。
        jbc::ir::Insn nw;
        nw.op = jbc::core::Op::NEW;
        nw.cp = session.internClass(synthName);
        seq.push_back(std::move(nw));
        seq.push_back(jbc::ir::CodeEditor::simple(jbc::core::Op::DUP));
        int loadStack = 0;
        for (size_t i = 0; i < capturedTypes.size(); ++i) {
            seq.push_back(jbc::ir::CodeEditor::local(
                static_cast<jbc::core::Op>(loadOpcode(capturedTypes[i].getSort())),
                static_cast<ca::u32>(capSlot[i])));
            loadStack += slotSize(capturedTypes[i].getSort());
        }
        jbc::ir::Insn init;
        init.op = jbc::core::Op::INVOKESPECIAL;
        init.cp = session.internMethodRef(synthName, "<init>", ctorDesc);
        seq.push_back(std::move(init));
        if (slot > unit.maxLocals) unit.maxLocals = static_cast<ca::u16>(slot);
        // 栈峰值：new+dup(2) + 全部捕获实参。
        unit.maxStack = static_cast<ca::u16>(unit.maxStack + 2 + loadStack);
    }
    // 逐条 insertBefore(at)（同锚点保序）+ 首条回填 srcPc，理由同 expandConcatSiteDom。
    jbc::ir::InsnId firstPlaced{};
    for (auto& in : seq) {
        const auto op = in.op;
        auto placed = editor.insertBefore(at, std::move(in));
        if (placed.is_err()) {
            // 站点改写部分插入即字节码损坏——显式失败，不静默吞。
            throw std::runtime_error(std::string("invokedynamic_preprocessor: insertBefore "
                                                 "failed (op=")
                                     + std::to_string(static_cast<int>(op)) + ", "
                                     + jbc::ir::to_string(placed.unwrap_err()) + ")");
        }
        if (firstPlaced == jbc::ir::kNoInsn) {
            firstPlaced = std::move(placed).unwrap();
        }
    }
    if (srcPc != jbc::ir::kNoPc && firstPlaced != jbc::ir::kNoInsn) {
        auto mut = unit.dom.get(firstPlaced);
        if (mut.is_ok()) std::move(mut).unwrap()->srcPc = srcPc;
    }

    PendingLambdaClass pc;
    pc.internalName = synthName;
    pc.dom = buildLambdaClassDom(arena, synthName, ifaceInternal, site.name, samDesc,
                                 capturedTypes, *implPtr, alt);
    pending.push_back(std::move(pc));
    return true;
}

void processImpl(CompilerIndex& index, IndyDispatchReport* report)
{
    auto& arena          = index.arena();
    int   expanded       = 0;
    int   lambdaExpanded = 0;
    int   remainingIndy  = 0;
    int   synthCounter   = 0;

    std::vector<PendingLambdaClass> pendingLambdaClasses;
    std::vector<PendingAccessFix>   pendingAccessFixes;

    for (auto& [name, cls] : index.getClasses()) {
        if (cls.isLibrary) continue;

        auto session = openDomEdit(cls, name);
        bool changed = false;

        // 类级 BootstrapMethods 懒解码（首个 indy 站点时）。
        const jbc::core::BootstrapMethodsAttr* bsmAttr = nullptr;

        const ca::usize methodCount = session->unit().methods.size();
        for (ca::usize mi = 0; mi < methodCount; ++mi) {
            if (!jbc::ir::hasCode(*session, mi)) continue;

            auto promoted = jbc::ir::promoteMethod(*session, mi);
            if (promoted.is_err()) {
                throw std::runtime_error("invokedynamic_preprocessor: promote failed for " + name
                                         + " method #" + std::to_string(mi));
            }
            auto unit = std::move(promoted).unwrap();

            // 先收集后改写（CodeDom 句柄域稳定，insertBefore 不失效既有句柄）。
            std::vector<jbc::ir::InsnId> siteIds;
            for (auto id : unit->dom.order()) {
                const auto* in = unit->dom.peek(id);
                if (in->op == jbc::core::Op::INVOKEDYNAMIC) siteIds.push_back(id);
            }
            if (siteIds.empty()) continue;

            if (bsmAttr == nullptr) bsmAttr = domBootstrapMethods(session->backing());
            if (bsmAttr == nullptr) {
                throw std::runtime_error("invokedynamic_preprocessor: INVOKEDYNAMIC without "
                                         "BootstrapMethods in "
                                         + name);
            }

            const std::string methodName(session->backing().nameOf(session->backing().methods[mi]));
            jbc::ir::CodeEditor editor(*unit);
            ca::i32             siteOrdinal   = 0;
            int                 methodExpanded = 0;
            for (auto id : siteIds) {
                const auto* in = unit->dom.peek(id);
                DomIndySite site = decodeDomIndySite(session->backing(), in->cp, *bsmAttr, arena);

                // indy native dispatch 静态判定：只读、只标记（波次一原型，设计文档 §5），
                // 不消费判定结果、不影响下方展开逻辑与字节码输出。
                if (report != nullptr) {
                    IndyDispatchDecision decision =
                        classifyIndySitePayloadImpl(index, site.name, site.desc, site.bsm,
                                                site.bsmArgs);
                    decision.className  = name;
                    decision.methodName = methodName;
                    decision.ordinal    = siteOrdinal;
                    report->sites.push_back(std::move(decision));
                }
                ++siteOrdinal;

                bool siteExpanded = false;
                bool wc           = false;
                if (isStringConcatSite(&site.bsm, wc)) {
                    siteExpanded =
                        expandConcatSiteDom(*session, *unit, editor, id, site, wc, in->srcPc,
                                            arena);
                    if (siteExpanded) ++expanded;
                }
                else if (isLambdaMetafactorySite(&site.bsm)) {
                    siteExpanded = expandLambdaSiteDom(index, *session, *unit, editor, id, site,
                                                       in->srcPc, synthCounter,
                                                       pendingLambdaClasses, pendingAccessFixes);
                    if (siteExpanded) ++lambdaExpanded;
                }
                if (siteExpanded) {
                    auto erased = editor.erase(id);
                    if (erased.is_err()) {
                        throw std::runtime_error("invokedynamic_preprocessor: erase indy site failed for "
                                                 + name + " method #" + std::to_string(mi));
                    }
                    ++methodExpanded;
                    changed = true;
                }
            }
            remainingIndy += static_cast<int>(siteIds.size()) - methodExpanded;

            // 展开改写了指令流才 lower（lower 写回 Code 槽；未改写保持原样）。
            if (methodExpanded > 0) {
                auto lowered = jbc::ir::lowerMethod(*session, mi);
                if (lowered.is_err()) {
                    throw std::runtime_error(
                        "invokedynamic_preprocessor: lower failed for " + name + " method #"
                        + std::to_string(mi) + ": "
                        + jbc::ir::to_string(std::move(lowered).unwrap_err()));
                }
            }
        }

        // 展开改写了指令流：类必须标记 modified，否则 JAR 输出写回原始字节
        //（OutputWriter 只重写 modified 类，展开静默作废），单类输出走
        // COMPUTE_MAXS 配新指令流有 VerifyError 风险（对齐 ldc_preprocessor
        // 同约束）。native_loader 的 clinit 注入按「modified 且含 ACC_NATIVE
        // 方法」判定，不受本处影响。
        if (changed) cls.modified = true;
        commitDomEdit(cls, name, session);
    }

    // 遍历结束后统一执行 impl 提权（会话期间 cls.dom 被接管，见 PendingAccessFix）。
    for (const auto& fix : pendingAccessFixes) {
        if (auto* target = domMethodInIndex(index, fix.owner, fix.name, fix.desc)) {
            if (!(target->access & Opcodes::ACC_PUBLIC)) {
                target->access &= ~(Opcodes::ACC_PRIVATE | Opcodes::ACC_PROTECTED);
                target->access |= Opcodes::ACC_PUBLIC;
            }
        }
    }

    // 遍历结束后统一注册合成 lambda 类（遍历中改 getClasses map 会使迭代器失效）。
    // 单类输出走 addClass（sidecar，DOM 载体——树投影由 syncDomToTree 物化），JAR 输出
    // 走 upsertJarResource。isLibrary=true 使 RegC 跳过编译（合成类保留纯字节码，由
    // OutputWriter 写出）。
    for (auto& pc : pendingLambdaClasses) {
        if (index.isJarInput()) {
            auto bytes = jbc::core::writeClass(*pc.dom);
            if (bytes.is_err()) {
                throw std::runtime_error("invokedynamic_preprocessor: synth lambda writeClass failed for "
                                         + pc.internalName);
            }
            index.upsertJarResource(pc.internalName + ".class", std::move(bytes).unwrap());
        }
        else {
            CompilerClass synthCls;
            synthCls.dom       = std::move(pc.dom);
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

IndyDispatchDecision classifyIndySitePayload(CompilerIndex& index,
                                             const std::string&             indyName,
                                             const std::string&             indyDesc,
                                             const mj2x::Handle&            bsm,
                                             const std::vector<ca::core::Any>& bsmArgs)
{
    return classifyIndySitePayloadImpl(index, indyName, indyDesc, bsm, bsmArgs);
}

ca::usize IndyDispatchReport::count(IndyDispatchForm form) const
{
    ca::usize n = 0;
    for (const auto& site : sites) {
        if (site.form == form) ++n;
    }
    return n;
}

IndyDispatchPayload classifyIndySiteForm(ca::str::Utf8StringArena& arena,
                                         const std::string&             indyName,
                                         const std::string&             indyDesc,
                                         const std::optional<mj2x::Handle>& bsm,
                                         const std::vector<ca::core::Any>&             bsmArgs)
{
    const CoreDecision core = classifyCore(
        arena, indyName, indyDesc, bsm.has_value() ? &*bsm : nullptr, bsmArgs,
        "custom bootstrap: bsm owner external (runtime crack candidate only)");
    IndyDispatchPayload payload;
    payload.form          = core.form;
    payload.directField   = core.directField;
    payload.recordMethods = core.recordMethods;
    return payload;
}

void InvokeDynamicPreprocessor::process(CompilerIndex& index)
{
    processImpl(index, nullptr);
}

void InvokeDynamicPreprocessor::process(CompilerIndex& index, IndyDispatchReport* report)
{
    processImpl(index, report);
}

}   // namespace mj2x
