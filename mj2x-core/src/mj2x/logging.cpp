/// @file logging.cpp
/// @brief mj2x 全局日志初始化实现（libca log 门面 + SimpleLogBackend）。
#include "mj2x/logging.hpp"

#include "libca/log/logger.hpp"
#include "libca/log/logger_registry.hpp"
#include "libca/log/simple_log_backend.hpp"

#include <memory>

namespace mj2x {

namespace {

const char* const kTargets[] = {"default", "cli", "compile", "pipeline", "regc", "stackc",
                                "vmc4", "llvm", "preprocess", "io"};

}   // namespace

void initLogging(ca::log::Level level, bool color)
{
    ca::log::SimpleLogConfig config;
    config.stream = ca::log::SimpleLogConfig::Stream::Stderr;
    config.color  = color;
    auto backend  = std::make_shared<ca::log::SimpleLogBackend>(config);
    for (const char* target : kTargets) {
        auto logger = std::make_shared<ca::log::Logger>(backend);
        logger->set_level(level);
        ca::log::LoggerRegistry::register_logger(target, logger);
    }
}

void setLogLevel(ca::log::Level level)
{
    for (const char* target : kTargets) {
        if (ca::log::Logger* logger = ca::log::LoggerRegistry::get(target)) {
            logger->set_level(level);
        }
    }
}

}   // namespace mj2x
