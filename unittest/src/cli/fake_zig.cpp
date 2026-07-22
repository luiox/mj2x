#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool writeFileFromEnvironment(const char* name, const std::string& data)
{
    const char* path = std::getenv(name);
    if (path == nullptr || *path == '\0') return true;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    return output.good();
}

}

int main(int argc, char** argv)
{
    if (argc < 3 || std::string(argv[1]) != "cc") return 64;

    std::string log;
    std::string outputPath;
    bool        readsStdin = false;
    for (int index = 1; index < argc; ++index) {
        log += argv[index];
        log.push_back('\n');
        if (std::string(argv[index]) == "-o" && index + 1 < argc)
            outputPath = argv[index + 1];
        readsStdin = readsStdin || std::string(argv[index]) == "-";
    }
    if (!writeFileFromEnvironment("MJ2X_FAKE_ZIG_LOG", log)) return 65;

    std::string source;
    if (readsStdin)
        source.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    if (!writeFileFromEnvironment("MJ2X_FAKE_ZIG_SOURCE", source)) return 66;
    if (outputPath.empty() || (readsStdin && source.empty())) return 67;
    if (const char* skipOutput = std::getenv("MJ2X_FAKE_ZIG_SKIP_OUTPUT");
        skipOutput != nullptr && *skipOutput != '\0')
        return 0;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(outputPath).parent_path(), ec);
    if (ec) return 68;
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output << "fake-zig-native";
    return output.good() ? 0 : 69;
}
