#pragma once

#include "mj2x/core/compiler_index.hpp"

#include <memory>
#include <string>

namespace mj2x {

class LcaTypeResolver;

class OutputWriter
{
public:
    OutputWriter();
    ~OutputWriter();

    /// @brief Write a compiler index back to `.class` or `.jar` output.
    ///
    /// @details
    /// JAR output preserves the input entry order and non-class resources while
    /// replacing compiled class entries with rewritten class bytes.  MVP output
    /// uses stored ZIP entries, so signed JAR metadata is preserved as bytes but
    /// existing signatures are not expected to remain valid after class changes.
    /// @p lca 非空时 modified 类帧重算走谱系 LCA（帧换装，Pipeline 供给）；
    /// 缺省 nullptr = 硬编码表口径（直连调用面/单测兼容）。
    void write(const std::string& path, CompilerIndex& index, LcaTypeResolver* lca = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace mj2x
