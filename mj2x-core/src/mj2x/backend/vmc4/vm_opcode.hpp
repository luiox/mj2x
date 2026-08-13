/// @file vm_opcode.hpp
/// @brief VMC4 虚拟机指令集定义：opcode 枚举、operand 编码、常量池。
///
/// @details
/// VMC4 是 mj2x 的 VM 虚拟化后端，把 RegIR（L3 寄存器式 IR）编码成自定义 opcode
/// 字节流，再生成一个 C 写的解释器执行它。设计目标是 Themida 风格的多虚拟机结构
/// ——不同 VmInstance 的 opcode 映射、handler 顺序、dispatcher 形态各不相同，让红队
/// 对每个 VM 实例的分析相互独立。
///
/// 本文件定义 MVP 阶段（Phase 0）的**固定**指令集：opcode 语义直接对应 RegIR 的
/// 13 个 instruction family 和 6 种 terminator。Phase 2 会引入 per-instance 的
/// opcode 编号随机化（VmInstance.opcodeTable），届时 opcode 的**数值**会随实例变化，
/// 但**语义**不变。
///
/// ### 指令格式（MVP，统一变长）
///
/// 每条指令的字节流布局：
///   [opcode:u8] [operands...]
///
/// operand 的基本单元：
///   - u8  : 1 字节无符号（type tag、op kind、predicate）
///   - u16 : 2 字节小端无符号（寄存器索引、block 索引）
///
/// **所有寄存器操作数都是 u16 索引**，指向 VM 寄存器文件 `regs[]`。字面量在编码阶段
/// 被收集到 VmConstantPool，并在方法入口 prologue 里预加载到高地址寄存器槽，
/// 使每条指令的 operand 格式统一为"寄存器索引"——dispatcher 不需要区分 reg/literal。
///
/// ### 寄存器文件布局
///
///   [0 .. numInputs)            : 方法参数（对应 RegFunction.parameters）
///   [numInputs .. maxCompute)   : 计算临时寄存器（对应 RegIR 的虚拟寄存器，
///                                 SSA value id 恒等映射）
///   [maxCompute .. numRegs)     : 常量槽（prologue 从 VmConstantPool 加载，
///                                 隔离在计算寄存器之后，避免 SSA 密集 id 重叠）
///
/// ### MVP 支持范围
///
/// Phase 0 只支持纯计算 + 控制流子集：
///   - Copy / Unary / Binary / Compare / Convert
///   - Jump / Branch / Return
///
/// 以下 family 在 MVP **fail-closed**（遇到即整个方法不转 VM，保留字节码）：
///   LoadField / StoreField / ArrayLoad / ArrayStore / Call / Allocate /
///   Intrinsic / Barrier / Throw / Switch
///
/// 这些 family 的 opcode 已预留枚举值，handler 在 Phase 2+ 补齐。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mj2x {

/// @brief VMC4 opcode 枚举。
///
/// @details
/// 数值在 MVP 固定；Phase 2 起 VmInstance.opcodeTable 会做 per-instance 重映射，
/// 届时解释器生成的 `case` 常量不再是这些固定值，但语义对应关系不变。
enum class VmOpcode : uint8_t
{
    Halt = 0x00,   ///< 停机（解释器主循环退出条件）

    // --- 纯计算 family（MVP 支持）---
    Copy    = 0x10,   ///< regs[dest] = regs[src]            [type:u8][dest:u16][src:u16]
    Unary   = 0x11,   ///< regs[dest] = op regs[src]         [op:u8][type:u8][dest:u16][src:u16]
    Binary  = 0x12,   ///< regs[dest] = lhs op rhs           [op:u8][type:u8][dest:u16][lhs:u16][rhs:u16]
    Compare = 0x13,   ///< regs[dest] = (lhs pred rhs) ? 1:0 [pred:u8][type:u8][dest:u16][lhs:u16][rhs:u16]
    Convert = 0x14,   ///< regs[dest] = (to_type) regs[src]  [from:u8][to:u8][dest:u16][src:u16]

    // --- 控制流 terminator（MVP 支持）---
    Jump       = 0x20,   ///< pc = block_targets[target]           [target:u16]
    Branch     = 0x21,   ///< if regs[cond] pc = true else false   [cond:u16][true:u16][false:u16]
    Return     = 0x22,   ///< return regs[src] / void              [has_value:u8][src:u16]

    // --- super-operator（Phase 3：指令折叠，由编码器从 Binary 模式匹配产生）---
    SuperInc   = 0x23,   ///< regs[dest] = regs[src] + 1           [type:u8][dest:u16][src:u16]
    SuperDec   = 0x24,   ///< regs[dest] = regs[src] - 1           [type:u8][dest:u16][src:u16]

    // --- 以下 family 预留，MVP fail-closed ---
    LoadField  = 0x30,
    StoreField = 0x31,
    ArrayLoad  = 0x32,
    ArrayStore = 0x33,
    Call       = 0x34,
    Allocate   = 0x35,
    Intrinsic  = 0x36,
    Barrier    = 0x37,
    Throw      = 0x38,
    Switch     = 0x39,
};

/// @brief Unary 操作种类（对应 SSAUnaryOp）。
enum class VmUnaryOp : uint8_t
{
    Neg    = 0,   ///< 取负（INEG/LNEG/FNEG/DNEG）
    BitNot = 1,   ///< 按位取反（整数）
};

/// @brief Binary 操作种类（对应 SSABinaryOp）。
enum class VmBinaryOp : uint8_t
{
    Add  = 0,
    Sub  = 1,
    Mul  = 2,
    Div  = 3,
    Rem  = 4,
    And  = 5,
    Or   = 6,
    Xor  = 7,
    Shl  = 8,
    Shr  = 9,
    UShr = 10,
};

/// @brief Compare 谓词（对应 SSAComparePredicate）。
enum class VmComparePred : uint8_t
{
    Eq = 0,
    Ne = 1,
    Lt = 2,
    Le = 3,
    Gt = 4,
    Ge = 5,
};

/// @brief VM 值类型 tag（对应 SSAType 的 VM 视角子集）。
///
/// @details
/// I32/I64/F32/F64 是计算类型；REF（对象引用）承载 jobject，自 Phase 2.0 起用于
/// 对象参数/返回值/字段。编码在每条计算指令的 type operand 里，handler 据此解释
/// 寄存器槽的位模式。Copy 的 handler 整体拷贝 union（`(void)type`），故 REF 值在
/// 寄存器间传递无需特殊处理。
enum class VmType : uint8_t
{
    I32 = 0,
    I64 = 1,
    F32 = 2,
    F64 = 3,
    REF = 4,   ///< 对象引用（slot.ref 承载 jobject）
};

/// @brief 方法调用形态（对应 JVM invoke 指令的 4 种 dispatch）。
enum class VmInvokeKind : uint8_t
{
    Virtual    = 0,
    Special    = 1,
    Static     = 2,
    Interface  = 3,
};

/// @brief JNI 类型类别：per-callsite helper 发射时 marshalling 用。
///
/// @details
/// 把 SSAType 收敛到 JNI 边界关心的 6 类，决定 jvalue 字段 / JNI Call 函数后缀 /
/// 寄存器槽读写位。boolean/byte/char/short/int 统一归 Int32（计算位宽一致，
/// JNI 侧用 jint 传递，VMC4 slot 用 .i 承载）。
enum class VmJniKind : uint8_t
{
    Void   = 0,
    Int32  = 1,   ///< boolean/byte/char/short/int → jint
    Int64  = 2,   ///< long → jlong
    Float  = 3,   ///< float → jfloat
    Double = 4,   ///< double → jdouble
    Ref    = 5,   ///< object/array → jobject
};

/// @brief 方法引用：一个 Call 调用点的静态信息（Phase 2.0）。
///
/// @details
/// 编码期把每个 Call 指令的 SSAMethodRef 收集到 VmProgram.methodRefs，字节流只存
/// callSiteIdx（索引）。backend 为每个 VmMethodRef 发射一个 TU-local helper 函数
/// （内部按 owner/name/desc/invokeKind 调 JNI），解释器经函数指针表分派。
/// argKinds/retKind 由编码期从 SSAType 推导，helper 据此 marshalling 参数/返回值。
/// 去重键 (owner,name,desc,invokeKind,hasDest) 已含 desc，故 argKinds/retKind 一致。
struct VmMethodRef
{
    std::string   owner;       ///< 方法所属类（JVM 内部名，如 "java/lang/String"）
    std::string   name;        ///< 方法名
    std::string   desc;        ///< 方法描述符（如 "(Ljava/lang/String;)I"）
    VmInvokeKind  invokeKind = VmInvokeKind::Virtual;
    bool          hasDest = false;   ///< 是否有返回值（void=false）
    VmJniKind                retKind  = VmJniKind::Void;   ///< 返回值类型类别
    std::vector<VmJniKind>   argKinds;                    ///< 参数类型类别（顺序对齐 args）
};

/// @brief 解释器调用结果。
enum class VmExecStatus : uint8_t
{
    Ok = 0,
    PendingException = 1,   ///< 执行期间抛了 Java 异常（如除零）
    InvalidBytecode = 2,    ///< opcode/operand 非法（不应发生，防御性）
};

/// @brief 常量池条目：编码阶段收集的字面量，prologue 预加载到寄存器槽。
///
/// @details
/// 一个条目对应一个常量寄存器槽。tag 决定如何把 value 写进 mj2x_vm_slot。
/// 字符串/类字面量等非计算常量在 MVP 不出现（fail-closed 的 family 才会用到）。
/// destSlot 记录该常量该落到寄存器文件的哪个索引（编码期由 maxReg 递增分配，
/// 可能与计算寄存器穿插，故非连续），prologue 据此写入 regs[destSlot]。
struct VmConstantEntry
{
    VmType  type;
    int64_t intBits;     ///< I32/I64 的值（I32 截断到 32 位）
    double  floatValue;  ///< F32/F64 的值
    uint16_t destSlot = 0;   ///< 目标寄存器槽索引（prologue 写入位置）
};

/// @brief 一个方法的完整 VM 程序（编码产物）。
///
/// @details
/// VmEncoder 把一个 RegFunction 编码成 VmProgram。解释器入口消费 bytecodes、
/// blockTargets、constantPool、numRegs 执行；寄存器文件大小由 numRegs 决定。
struct VmProgram
{
    std::vector<uint8_t>         bytecodes;      ///< 指令字节流
    std::vector<uint32_t>        blockTargets;   ///< block 索引 → bytecodes 偏移
    std::vector<VmConstantEntry> constantPool;   ///< 常量条目（prologue 预加载）
    std::vector<VmMethodRef>     methodRefs;     ///< Call 调用点的方法引用（Phase 2.0）
    uint16_t                     numInputs = 0;  ///< 方法参数占用的寄存器数
    uint16_t                     numRegs   = 0;  ///< 寄存器文件总大小
};

/// @brief VmOpcode 的字符串名（用于生成的 C 注释与日志）。
const char* vmOpcodeName(VmOpcode op);

/// @brief dispatcher 形态枚举（Phase 1 的 5 种真实 dispatcher）。
enum class VmDispatcherKind : uint8_t
{
    SwitchTable    = 0,   ///< switch(opcode){case...}
    FuncPtrTable   = 1,   ///< handlers[opcode](ctx) 函数指针表
    ComputedGoto   = 2,   ///< goto *labels[opcode]（GCC 扩展，MSVC 不支持）
    IfNest         = 3,   ///< if/else if 线性链
    StateDispatcher = 4,  ///< state 寄存器 + 平坦化 dispatcher
};

/// @brief VmDispatcherKind 的字符串名。
const char* vmDispatcherName(VmDispatcherKind kind);

}   // namespace mj2x
