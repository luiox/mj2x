/// @file        jvm_type.hpp
/// @brief       JVM 类型投影：描述符解析门面（libjbc 迁移批 1）。
///
/// 旧栈 mj2x::Type 的消费侧对位面（批 1 盘点：getType/
/// getObjectType/getMethodType/getArgumentTypes/getReturnType 静态族 +
/// getSort/getDescriptor/getInternalName/实例 getArgumentTypes/getReturnType/
/// toString/operator==）。语义逐位对齐旧栈（视图存储、INTERNAL 掩蔽为
/// OBJECT、异常消息原文），差异仅在：描述符结构校验单源 jbc::type
/// （空描述符确定性抛 invalid_argument——J-6 越界读根治点），opcode
/// 常量镜像见 jvm_opcodes.hpp。
#pragma once

#include <libca/str/utf8_string.hpp>
#include <libca/str/utf8_string_arena.hpp>

#include <string>
#include <vector>

namespace mj2x {

/// @brief JVM 类型值（sort + 描述符视图；生命周期随入参字符串/arena）。
class Type
{
public:
    static constexpr int VOID     = 0;
    static constexpr int BOOLEAN  = 1;
    static constexpr int CHAR     = 2;
    static constexpr int BYTE     = 3;
    static constexpr int SHORT    = 4;
    static constexpr int INT      = 5;
    static constexpr int FLOAT    = 6;
    static constexpr int LONG     = 7;
    static constexpr int DOUBLE   = 8;
    static constexpr int ARRAY    = 9;
    static constexpr int OBJECT   = 10;
    static constexpr int METHOD   = 11;
    static constexpr int INTERNAL = 12;

    /// @brief 解析单个类型描述符（含 V / '(' 方法描述符形态）。
    static Type getType(const ca::str::Utf8StringRef& typeDescriptor);

    /// @brief 内部名 → 类型：'[' 前缀 = ARRAY，否则 INTERNAL（getSort 掩蔽为 OBJECT）。
    static Type getObjectType(const ca::str::Utf8StringRef& internalName);

    /// @brief 方法描述符 → METHOD 排序类型（不校验，旧栈行为对齐）。
    static Type getMethodType(const ca::str::Utf8StringRef& methodDescriptor);

    /// @brief 方法描述符 → 参数类型列表（视图指向入参串）。
    static std::vector<Type> getArgumentTypes(const ca::str::Utf8StringRef& methodDescriptor);

    /// @brief 方法描述符 → 返回类型。
    static Type getReturnType(const ca::str::Utf8StringRef& methodDescriptor);

    /// @brief 排序值（INTERNAL 掩蔽为 OBJECT，旧栈行为对齐）。
    int getSort() const;

    /// @brief 描述符串：OBJECT/INTERNAL 物化 "L..;" intern 进 arena，其余原样视图。
    ca::str::Utf8StringRef getDescriptor(ca::str::Utf8StringArena& arena) const;

    /// @brief 内部名：OBJECT/INTERNAL 原样视图，ARRAY intern，其余 logic_error。
    ca::str::Utf8StringRef getInternalName(ca::str::Utf8StringArena& arena) const;

    /// @brief METHOD 排序：参数类型列表（非 METHOD 为 logic_error）。
    std::vector<Type> getArgumentTypes() const;

    /// @brief METHOD 排序：返回类型（非 METHOD 为 logic_error）。
    Type getReturnType() const;

    /// @brief 描述符串（std::string 形态，诊断输出用）。
    std::string toString() const;

    bool operator==(const Type& other) const;
    bool operator!=(const Type& other) const;

    Type() noexcept
        : sort_(VOID)
        , value_()
    {}

private:
    Type(int sort, const ca::str::Utf8StringRef& value) noexcept
        : sort_(sort)
        , value_(value)
    {}

    /// @brief 旧栈切片语义：[begin, end) 字节域 → sort + 视图。
    static Type slice(const ca::str::Utf8StringRef& desc, size_t begin, size_t end);
    static size_t returnTypeOffset(const ca::str::Utf8StringRef& methodDescriptor);

    int                    sort_;
    ca::str::Utf8StringRef value_;
};


/// @brief JVM 方法句柄值（tag + owner/name/desc 视图 + itf 位）。
///
/// 旧栈 mj2x::Handle 的消费侧对位面：bsm/implMethod Any 载荷与
/// MIR/SSA/RegIR 携带面的自有载体。视图寿命随 intern 进 Utf8StringArena 的
/// 底串（与 mj2x::Type 同纪律）。无默认构造（载荷随构造注入，旧栈契约）。
class Handle
{
public:
    Handle(int tag, const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
           const ca::str::Utf8StringRef& desc, bool itf = false)
        : tag_(tag), owner_(owner), name_(name), desc_(desc), itf_(itf)
    {}

    int getTag() const noexcept { return tag_; }
    const ca::str::Utf8StringRef& getOwner() const noexcept { return owner_; }
    const ca::str::Utf8StringRef& getName() const noexcept { return name_; }
    const ca::str::Utf8StringRef& getDesc() const noexcept { return desc_; }
    /// @brief 接口方法句柄位（Methodref vs InterfaceMethodref）。
    bool isInterface() const noexcept { return itf_; }

    bool operator==(const Handle& other) const
    {
        return tag_ == other.tag_ && itf_ == other.itf_ && owner_ == other.owner_
            && name_ == other.name_ && desc_ == other.desc_;
    }
    bool operator!=(const Handle& other) const { return !(*this == other); }

private:
    int                    tag_;
    ca::str::Utf8StringRef owner_;
    ca::str::Utf8StringRef name_;
    ca::str::Utf8StringRef desc_;
    bool                   itf_;
};

}   // namespace mj2x
