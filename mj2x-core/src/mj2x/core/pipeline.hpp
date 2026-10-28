/// @file pipeline.hpp
/// @brief 编译流水线：InputLoader -> Preprocessor -> CodeConverter -> Backend -> Postprocessor -> OutputWriter。
///
/// @details
/// Pipeline 是 mj2x 的编排核心，固定 6 阶段顺序：
///  1. loadInput：读 .class/.jar 到 CompilerIndex；
///  2. runPreprocessors：bytecode shaping（LDC/invokedynamic/multianewarray/标记选择）；
///  3. compileMethods：每个 markedForCompile 方法经 CodeConverter 转 L1 MIR，交各 Backend；
///  4. (Backend.injectIntoClass 紧跟每个 compileMethod)；
///  5. runPostprocessors：clinit 注入、loader 资源、cloader 等（在所有编译后）；
///  6. writeOutput：写 .class/.jar。
///
/// registerBackend/Preprocessor/Postprocessor 在 run() 前注册，顺序即执行顺序。
#pragma once

#include "compiler_index.hpp"
#include "mj2x/backend/backend.hpp"
#include "mj2x/frontend/code_converter.hpp"
#include "mj2x/preprocessor/preprocessor.hpp"
#include "mj2x/postprocessor/postprocessor.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mj2x {

/// Pipeline 运行配置：输入/输出路径与可选 IR/MIR dump。
struct PipelineConfig
{
    std::string inputPath;
    std::string outputPath;
    bool        dumpIR  = false;
    bool        dumpMIR = false;
};

/// 编排 6 阶段流水线的驱动器。注册顺序 = 执行顺序。
class Pipeline
{
public:
    explicit Pipeline(PipelineConfig config);
    ~Pipeline();

    void registerBackend(std::unique_ptr<Backend> backend);
    void registerPreprocessor(std::unique_ptr<Preprocessor> preproc);
    void registerPostprocessor(std::unique_ptr<Postprocessor> postproc);

    /// 运行完整流水线，返回 0 表示成功。
    int run();

    /// @brief run() 后可查：全部后端 finalize 期降级告警总数（T1 聚合面）。
    /// @details 编程消费入口（corpus 门禁等）；0 = 无降级。
    std::size_t finalizeWarningCount() const { return finalizeWarningCount_; }

private:
    void loadInput();
    void runPreprocessors();
    void compileMethods();
    void runPostprocessors();
    void writeOutput();

    PipelineConfig                              config_;
    std::unique_ptr<CompilerIndex>              index_;
    std::vector<std::unique_ptr<Backend>>       backends_;
    std::vector<std::unique_ptr<Preprocessor>>  preprocs_;
    std::vector<std::unique_ptr<Postprocessor>> postprocs_;
    std::size_t                                 finalizeWarningCount_ = 0;
};

}   // namespace mj2x
