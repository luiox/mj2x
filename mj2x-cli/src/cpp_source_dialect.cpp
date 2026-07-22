#include "cpp_source_dialect.hpp"

#include <regex>

namespace mj2x::cli {

std::string emitCpp17Dialect(std::string_view cSource)
{
    std::string result(cSource);

    /// JNIEXPORT 仅控制导出可见性；C++ 下还必须显式固定 JVM 查找的 C linkage。
    const std::regex exportedJniFunction(R"(JNIEXPORT\s+)");
    result = std::regex_replace(result, exportedJniFunction, "extern \"C\" JNIEXPORT ");

    /// C JNI: (*env)->FindClass(env, "pkg/Type")
    /// C++ JNI: env->FindClass("pkg/Type")
    const std::regex callWithArguments(
        R"(\(\*([A-Za-z_][A-Za-z0-9_]*)\)->([A-Za-z_][A-Za-z0-9_]*)\(\1,\s*)");
    result = std::regex_replace(result, callWithArguments, "$1->$2(");

    /// C JNI: (*env)->ExceptionCheck(env)
    /// C++ JNI: env->ExceptionCheck()
    const std::regex callWithoutArguments(
        R"(\(\*([A-Za-z_][A-Za-z0-9_]*)\)->([A-Za-z_][A-Za-z0-9_]*)\(\1\))");
    return std::regex_replace(result, callWithoutArguments, "$1->$2()");
}

}   // namespace mj2x::cli
