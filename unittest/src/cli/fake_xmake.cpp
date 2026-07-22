#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string envValue(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

fs::path projectPath(int argc, char** argv)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "-P") return argv[index + 1];
    }
    std::error_code ec;
    return fs::current_path(ec);
}

}

int main(int argc, char** argv)
{
    if (argc < 2) return 64;
    const std::string phase = std::string(argv[1]) == "f" ? "configure" : "build";
    if (envValue("MJ2X_FAKE_XMAKE_FAIL") == phase) return phase == "configure" ? 21 : 22;
    if (phase == "configure") return 0;

    const fs::path project = projectPath(argc, argv);
    if (project.empty()) return 65;
    if (envValue("MJ2X_FAKE_XMAKE_ARTIFACT") == "none") return 0;
#if defined(_WIN32)
    const fs::path library = project / "build" / "windows" / "x64" / "release" /
                             "mj2x_native.dll";
#elif defined(__APPLE__)
    const fs::path library = project / "build" / "macosx" / "x86_64" / "release" /
                             "libmj2x_native.dylib";
#else
    const fs::path library = project / "build" / "linux" / "x86_64" / "release" /
                             "libmj2x_native.so";
#endif
    std::error_code ec;
    fs::create_directories(library.parent_path(), ec);
    if (ec) return 66;
    std::ofstream output(library, std::ios::binary | std::ios::trunc);
    output << "fake-xmake-native";
    return output.good() ? 0 : 67;
}
