/// @file vmc4_instance.hpp
/// @brief VmInstance：一个 VM 实例的完整描述（seed 驱动生成）。
///
/// @details
/// VmInstance 是 VMC4 的中心抽象——它描述一个 VM 实例的全部可变维度：
///   - opcode 编号映射表（VmOpcode → 实例 opcode 字节集合，含 ALT 别名）
///   - handler 顺序（dispatch 表/handler 的排列）
///   - dispatcher 算法（5 种之一）
///   - 驱动上述维度的 seed
///
/// 已落地的可变性维度：
///   - opcodeTable：每个语义 → 互不相交的实例字节集合（Phase 2 canonical +
///     Phase 3 ALT 别名），编码器按指令 nonce 确定性选字节
///   - handlerOrder：支持集 opcode 的洗牌排列（决定 dispatch 结构出现顺序）
///   - dispatcherKind：5 种 dispatcher 真实落地
///   - 粒度：per-class / per-method（backend 按身份哈希派生）
///   - super-operator：编码器把 Binary(x,+1)/(x,-1) 折叠为 SuperInc/SuperDec
///   - bogus handler：解释器源码注入不可达假 handler（只污染静态分析）
///
/// ### 可复现构建不变量
///
/// 相同 instanceSeed → 相同 VmInstance（字节集合/handler 顺序/别名全同）。
/// 所有随机维度都从 instanceSeed 确定性派生，禁止未播种的随机源。
/// backend 从 CLI 主 seed 派生 instanceSeed = mix(mainSeed, 类/方法身份哈希)。
#pragma once

#include "mj2x/backend/vmc4/vm_opcode.hpp"
#include "mj2x/codegen/runtime_protection.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mj2x {

/// @brief VMC4 配置选项（由 CLI/注解传入 Vmc4Backend，再传给 VmInstance 构造）。
struct Vmc4Options
{
    /// dispatcher 形态。默认 SwitchTable（最广平台兼容）。
    VmDispatcherKind dispatcherKind = VmDispatcherKind::SwitchTable;

    /// 主 seed。backend 用它与类/方法身份哈希派生每个实例的 instanceSeed。
    /// 默认 0 也是合法 seed（仍是确定性随机化的固定实例）。
    uint64_t seed = 0;

    /// 实例粒度：per-class 同类方法共享一个 VmInstance（推荐默认，体积可控）；
    /// per-method 每方法独立实例（最强可变性，代码膨胀最大）；
    /// per-program 整个程序共享一个 VmInstance（opcode 空间全局统一，方法间
    /// 字节流互相参照，体积最优——解释器只生成一份）。
    enum class Granularity
    {
        PerClass = 0,
        PerMethod = 1,
        PerProgram = 2,
    };
    Granularity granularity = Granularity::PerClass;

    /// ALT 别名数：每个语义除 canonical 外再分配的额外 opcode 字节数（0..3）。
    /// 别名扩大 opcode 空间并让同一语义在字节流里多次出现不同字节；
    /// 解释器为同一语义的全部字节发射共享 handler。默认 1（体积/可变性平衡）。
    int aliasCount = 1;

    /// 注入的 bogus handler 数：解释器源码中出现但 dispatch 表不引用的
    /// 假 handler（只污染静态分析，运行时永不命中）。
    int bogusHandlers = 1;

    /// 执行期 dispatch drift 变体数（0 = 关闭，1..4 = 开启）。
    ///
    /// @details
    /// 开启后编码器/解释器按「字节偏移周期」轮转语义标签：字节集合固定不变，
    /// 但「集合 → 语义」的对应关系随周期切换（语义标签轮转）。解释器用
    /// per-variant unmap 表把原始字节解回 canonical 字节再分发，指令语义
    /// 完全不变量（语义保真由 unmap 纯函数保证）。红队对"字节 X = 语义 Y"
    /// 的一次性分析结论在下一周期即失效。
    int driftVariants = 0;

    /// drift 变体切换周期（字节流偏移单位）。默认 16：每 16 字节换一个变体。
    int driftPeriod = 16;

    /// 程序 blob at-rest 加密（--vmc4-encrypt-program；默认关闭）。
    ///
    /// @details
    /// 开启后编码器输出的 opcode/字节流数据段在发射期加密（密钥从主 seed 派生），
    /// 解释器在初始化加载期解密；产物 C 源与编译后二进制中不再出现明文程序流
    /// （issue #760 记录的前提缺口：jvmtp 1.6.2 起 at-rest 加密为默认形态）。
    bool programBlobEncrypt = false;

    /// 程序 blob 分块懒解密（--vmc4-lazy-decrypt；默认关闭，要求
    /// programBlobEncrypt 开启）。
    ///
    /// @details
    /// issue #760 切片 2（对齐 jvmtp 1.6.4 vm.lazy-decrypt）：开启后 blob 按
    /// 指令对齐的 64 字节目标块独立加密（块密钥从 blob 密钥 + 块下标派生），
    /// JNI_OnLoad 不再整体解密；解释器首次取码到某块时才原地解密该块，
    /// 未执行到的分支保持密文（内存 dump 只能拿到实际执行过的块）。解密后
    /// 保持明文不回擦（权衡见 vm_interpreter_emit.cpp 分块发射注释）。
    /// 单独开启（无 programBlobEncrypt）无意义：发射侧按纯加密关闭处理，
    /// CLI 侧以 requires 约束（vmc4.encrypt_program）。
    bool programLazyDecrypt = false;

    /// 反侦测检查点注入（wrapper 入口 mj2x_rt_checkpoint_run；默认关闭）。
    /// 依赖 mj2x-rt 的 rt_protect 模块（rt.resources 内嵌）。
    codegen::RuntimeProtectionOptions runtimeProtection;
};

/// @brief 一个 VM 实例的描述对象。
///
/// @details
/// 由 Vmc4Options + instanceSeed 构造（instanceSeed 由 backend 从主 seed 与
/// 类/方法身份哈希派生）。解释器发射器（VmInterpreterEmitter）和编码器（VmEncoder）
/// 都消费 VmInstance 来决定生成形态：opcode 字节、handler 排列、dispatcher 结构。
class VmInstance
{
public:
    /// @brief 从配置 + 实例 seed 构造 VM 实例。
    ///
    /// @details
    /// 确定性：相同 options + instanceSeed 产生相同实例。instanceSeed == 0 时
    /// 回落用 options.seed（便于直接构造）；backend 总是显式传入混合后的 seed。
    VmInstance(const Vmc4Options& options, uint64_t instanceSeed = 0);

    /// dispatcher 形态（解释器发射据此选择生成路径）。
    VmDispatcherKind dispatcherKind() const { return dispatcherKind_; }

    /// 主配置 seed（日志用）。
    uint64_t seed() const { return seed_; }

    /// 实际驱动本实例的 seed（配置 seed 与身份哈希混合后）。
    uint64_t instanceSeed() const { return instanceSeed_; }

    /// ALT 别名数。
    int aliasCount() const { return aliasCount_; }

    /// 注入的 bogus handler 数。
    int bogusHandlers() const { return bogusHandlers_; }

    /// drift 变体数（0 = 关闭）。
    int driftVariants() const { return driftVariants_; }

    /// drift 变体切换周期（字节偏移单位）。
    int driftPeriod() const { return driftPeriod_; }

    /// drift 是否开启（driftVariants > 0）。
    bool driftEnabled() const { return driftVariants_ > 0; }

    /// 程序 blob at-rest 加密是否开启（--vmc4-encrypt-program；issue #760 首切片）。
    ///
    /// @details
    /// 开启时 VmInterpreterEmitter 把 opcode 字节流数据段以密文发射（密钥从
    /// instanceSeed 派生），backend 在 JNI_OnLoad 发射 mj2x_vm_blob_decrypt
    /// 原地解密调用。与 dispatch drift 正交：drift 变换作用于明文字节流的
    /// 语义解释（unmap 表），加密作用于最终字节流的 at-rest 形态——加密在
    /// drift 编码完成之后、发射之前施加，解密后字节流即 drift 编码流。
    bool blobEncrypt() const { return blobEncrypt_; }

    /// 程序 blob 分块懒解密是否开启（--vmc4-lazy-decrypt；issue #760 切片 2）。
    ///
    /// @details
    /// 开启时（恒以 blobEncrypt() 为前提）VmInterpreterEmitter 把字节流按指令
    /// 对齐块独立加密并发懒解密基础设施（cstate/cend 表 + touch 调用），
    /// backend 不再在 JNI_OnLoad 发射整体解密。与 dispatch drift 正交：加密
    /// 作用于 drift 编码完成后的最终字节流（分块边界按最终流布局计算）。
    bool blobLazy() const { return blobLazy_; }

    /// @brief 按字节流偏移计算当前 drift 变体序号。
    ///
    /// @details
    /// 编码器与解释器共用同一公式（字节偏移 / period % D），天然同步，
    /// 与执行路径（跳转/分支）无关——同一字节位置永远是同一变体。
    int variantForPc(uint64_t pc) const
    {
        if (!driftEnabled()) return 0;
        return static_cast<int>((pc / static_cast<uint64_t>(driftPeriod_)) %
                                static_cast<uint64_t>(driftVariants_));
    }

    /// @brief 在指定 drift 变体下为该语义选一个实例字节。
    ///
    /// @details
    /// 变体 d 下语义 op 使用「集合 P_d(op)」的字节（语义标签轮转：集合固定，
    /// 归属轮换）。nonce 语义与 pickOpcodeByte 相同（确定性选 canonical/别名）。
    uint8_t variantPickOpcodeByte(VmOpcode op, uint64_t nonce, int variant) const;

    /// @brief 变体 d 下原始字节 → canonical 字节（unmap 表内容）。
    ///
    /// @details
    /// raw 属于集合 h → 返回 h 的 canonical 字节；不属于任何支持语义集合 →
    /// 0xFF（发射器把它填进 unmap 表，解释器落 default 走 InvalidBytecode）。
    uint8_t unmapByte(uint8_t raw, int variant) const;

    /// @brief VmOpcode → 实例 canonical 字节。
    ///
    /// @details
    /// canonical 是该语义字节集合的第一个字节。编码器通常用 pickOpcodeByte
    /// 按指令 nonce 在集合内选择；发射器的 Halt 哨兵等固定点用 canonical。
    uint8_t opcodeFor(VmOpcode op) const
    {
        return opcodeBytes_[static_cast<uint8_t>(op)].front();
    }

    /// @brief VmOpcode → 全部实例字节（canonical 在前，后跟 ALT 别名）。
    ///
    /// @details
    /// 不同语义的字节集合互不相交；同集合内字节数 = 1 + aliasCount。
    const std::vector<uint8_t>& opcodeBytesFor(VmOpcode op) const
    {
        return opcodeBytes_[static_cast<uint8_t>(op)];
    }

    /// @brief 按指令 nonce 确定性选取该语义的一个实例字节。
    ///
    /// @details
    /// 编码器每条指令递增 nonce；同一 nonce + 同一实例 → 同一字节（可复现）。
    /// 字节在 canonical+别名集合内均匀伪随机选取。
    uint8_t pickOpcodeByte(VmOpcode op, uint64_t nonce) const;

    /// @brief 该语义的 handler 是否使用等价变形形态（Phase 3 handler 形态维度）。
    ///
    /// @details
    /// 由实例 seed 确定性决定（与 opcode 映射独立），仅对提供 bodyAlt 的语义
    /// 有意义；语义不变（e2e 验证）。
    bool usesAltHandlerForm(VmOpcode op) const;

    /// @brief 支持 opcode 的洗牌排列（handler 在 dispatch 结构里的出现顺序）。
    ///
    /// @details
    /// 计算/控制流 opcode（不含 Halt）。发射器按此顺序生成 case/表项/if 链。
    const std::vector<VmOpcode>& handlerOrder() const { return handlerOrder_; }

    /// 实例的 Halt canonical 字节（停机哨兵，随映射随机化）。
    uint8_t haltByte() const { return opcodeFor(VmOpcode::Halt); }

    /// @brief 变体 d 的 case 标签排列（段内值 0..K-1，K = 支持集大小）。
    ///
    /// @details
    /// period 语义变义（#762 升级档）：drift 的编码轮换之上，case 标签本身
    /// 按周期轮换——语义 op 在变体 d 下的 case 常量 = d*K + perm_d[canonical]。
    /// 各变体标签段互不相交（跨变体无碰撞）；case 体按语义共享（行为不变，
    /// 语义保真由 e2e 验证）。drift 关闭时为空。
    const std::vector<uint8_t>& caseLabelPerm(int variant) const
    {
        return caseLabelPerm_[static_cast<size_t>(variant)];
    }

    /// @brief 语义 op 在变体 d 下的 case 标签常量（drift 关闭时 = canonical 字节）。
    int caseLabelFor(VmOpcode op, int variant) const;

    /// @brief case 标签空间大小（= driftVariants × K；drift 关闭时为 0）。
    int caseLabelSpace() const;

    /// @brief 实例的简短标识（用于生成的函数名后缀，避免多实例命名冲突）。
    std::string instanceTag() const;

    /// @brief 实例的确定性文本描述（mjdbg 红队 expected trace surface 契约）。
    ///
    /// @details
    /// 覆盖：dispatcher 形态、instanceSeed、aliasCount/bogusHandlers、每语义的
    /// 实例字节集合、handler 顺序、alt-form 选择。同实例 → 同描述（可复现），
    /// 供样本随附 trace surface 文件与红队观测比对。
    std::string describe() const;

private:
    void buildOpcodeBytes();
    void buildHandlerOrder();
    void buildDriftVariants();
    void buildCaseLabelPerms();

    VmDispatcherKind dispatcherKind_;
    uint64_t         seed_;
    uint64_t         instanceSeed_;
    int              aliasCount_;
    int              bogusHandlers_;
    int              driftVariants_;
    int              driftPeriod_;
    /// 程序 blob at-rest 加密开关（options.programBlobEncrypt 透传，不入
    /// describe()——trace surface 红队契约字段集保持稳定）。
    bool             blobEncrypt_;
    /// 分块懒解密开关（options.programLazyDecrypt 透传，不入 describe()）。
    bool             blobLazy_;
    /// VmOpcode 值 → 该语义的实例字节集合（canonical 在前）。
    std::array<std::vector<uint8_t>, 256> opcodeBytes_;
    std::vector<VmOpcode> handlerOrder_;
    /// drift 变体排列：variantPerm_[d][c] = 变体 d 下 canonical 字节 c 对应
    /// 的语义集合索引（0..K-1，K = 支持集大小）。字节集合固定，归属轮换。
    std::array<std::vector<uint8_t>, 4> variantPerm_;
    /// period 语义变义：caseLabelPerm_[d][canonical] = 变体 d 下 canonical
    /// 字节的 case 标签段内值（0..K-1）；case 常量 = d*K + 段内值。
    std::array<std::vector<uint8_t>, 4> caseLabelPerm_;
};

/// @brief 主 seed 与身份哈希混合派生实例 seed（确定性）。
///
/// @details
/// 同主 seed + 同身份 → 同实例 seed；不同身份（类/方法）→ 不同实例 seed。
uint64_t vmc4DeriveInstanceSeed(uint64_t mainSeed, uint64_t identityHash);

/// @brief 从实例 seed 派生程序 blob 加密密钥（32 位，确定性，恒非 0）。
///
/// @details
/// 发射侧（VmInterpreterEmitter 加密）与运行侧（JNI_OnLoad 解密调用发射）
/// 共用同一派生函数，保证同实例 → 同密钥。独立种子段（与 opcode 映射/
/// handler 顺序等其他实例维度不耦合）。结果折叠到 xorshift32 的 32 位状态
/// 空间；0 是 xorshift32 死锁态（密钥流退化为全零 = 未加密），折叠结果为 0
/// 时回退固定种子。密钥不以数组形式落产物——生成的 C 只含 seed 常量，
/// keystream 由解密循环现场重算。切片 2：CRT 解密原语对密钥参数与 keystream
/// 状态做用后擦除（密钥栈擦除）；分块密钥派生见 vmc4DeriveChunkKey。
uint32_t vmc4DeriveBlobKey(uint64_t instanceSeed);

/// @brief 从 blob 密钥 + 块下标派生懒解密分块密钥（32 位，确定性，恒非 0）。
///
/// @details
/// issue #760 切片 2：blob 按指令对齐块独立加密，块 c 的密钥 =
/// vmc4DeriveChunkKey(blobKey, c)（golden-ratio 扰动 + 4 轮 xorshift32 扩散）。
/// 发射侧（VmInterpreterEmitter 分块加密）与运行侧（CRT mj2x_vm_chunk_key）
/// 同一公式双镜像，纯 u32 算术逐位一致。互不耦合的独立派生面：知道某个块的
/// 密钥不能直接推出其他块的密钥（vs 整体密钥流的分段复用）。
/// 结果恒非 0（xorshift32 死锁态回避，与 vmc4DeriveBlobKey 同一约定）。
uint32_t vmc4DeriveChunkKey(uint32_t blobKey, uint32_t chunkIndex);

}   // namespace mj2x
