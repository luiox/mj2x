#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string envValue(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

void sleepSeconds(int seconds)
{
#ifdef _WIN32
    Sleep(static_cast<DWORD>(seconds) * 1000);
#else
    sleep(static_cast<unsigned>(seconds));
#endif
}

// MJ2X_FAKE_XMAKE_HANG: simulated hung xmake (P1-4 timeout E2E). Hangs on
// every phase; the CLI's --native-timeout must reap the whole process tree
// (xmake plus any grandchildren) instead of blocking on the pipes forever.
void hangIfRequested()
{
    const std::string hang = envValue("MJ2X_FAKE_XMAKE_HANG");
    if (hang.empty() || hang == "0") return;
    int seconds = 60;
    const std::string secs = envValue("MJ2X_FAKE_XMAKE_HANG_SECS");
    if (!secs.empty()) seconds = std::atoi(secs.c_str());
    if (seconds <= 0) seconds = 60;
    sleepSeconds(seconds);
}

fs::path projectPath(int argc, char** argv)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "-P") return argv[index + 1];
    }
    std::error_code ec;
    return fs::current_path(ec);
}

bool markGeneratedSourceReadOnly(const fs::path& project)
{
    const fs::path source = project / "src" / "generated" / "native.c";
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) return false;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return SetFileAttributesW(source.c_str(), attributes | FILE_ATTRIBUTE_READONLY) != 0;
#else
    fs::permissions(source,
                    fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove,
                    ec);
    return !ec;
#endif
}

}

int main(int argc, char** argv)
{
    if (argc < 2) return 64;
    const std::string phase = std::string(argv[1]) == "f" ? "configure" : "build";
    hangIfRequested();
    const std::string failure = envValue("MJ2X_FAKE_XMAKE_FAIL");
    // NTSTATUS crash shape (issue #1023 item 3 retry-path E2E): return
    // 0xC0000374 (heap corruption, signed -1073740940) on both phases. A
    // normal return yields the same exit code as a real crash (no WER), so
    // the test can assert first-run failure -> exactly one minimal-env retry
    // -> clear final error. ASCII comments only: this target compiles without
    // /utf-8, and a C4819 codepage swallow could eat the line below.
    if (failure == "crash") return -1073740940;
    if (failure == "configure" && phase == "configure") return 21;
    if (phase == "configure") return 0;

    const fs::path project = projectPath(argc, argv);
    if (project.empty()) return 65;
    if (failure == "build") {
        if (envValue("MJ2X_FAKE_XMAKE_READONLY_SOURCE") == "1" &&
            !markGeneratedSourceReadOnly(project)) {
            return 70;
        }
        return 22;
    }
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
    if (!output.good()) return 67;
    if (envValue("MJ2X_FAKE_XMAKE_ARTIFACT") != "multiple") return 0;

    const fs::path duplicate = library.parent_path() / "duplicate" / library.filename();
    fs::create_directories(duplicate.parent_path(), ec);
    if (ec) return 68;
    std::ofstream second(duplicate, std::ios::binary | std::ios::trunc);
    second << "fake-xmake-native-duplicate";
    return second.good() ? 0 : 69;
}
