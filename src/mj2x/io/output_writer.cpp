#include "output_writer.hpp"

#include <cstdio>
#include <fstream>
#include "jbytecode/jbytecode.hpp"

namespace mj2x {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

struct OutputWriter::Impl {};

OutputWriter::OutputWriter()
    : impl_(std::make_unique<Impl>())
{}

OutputWriter::~OutputWriter() = default;

void OutputWriter::write(const std::string& path, CompilerIndex& index) {
    auto& classes = index.getClasses();
    if (classes.empty()) {
        printf("[OutputWriter] No classes to write\n");
        return;
    }

    // Phase 1: single-class output
    for (auto& [name, cls] : classes) {
        ClassWriter writer(index.arena(), ClassWriter::COMPUTE_MAXS);
        cls.classNode->accept(&writer);
        auto bytes = writer.toByteArray();

        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) {
            printf("[OutputWriter] ERROR: cannot write %s\n", path.c_str());
            return;
        }
        ofs.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        ofs.close();

        printf("[OutputWriter] Written: %s (%zu bytes)\n",
               path.c_str(), bytes.size());
    }
}

} // namespace mj2x

