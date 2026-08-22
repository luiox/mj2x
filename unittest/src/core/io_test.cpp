#include <gmock/gmock.h>

#include <fstream>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#include "jzip/zip_io.hpp"
#include "zip/checksum.hpp"
#include "libca/str/utf8_string.hpp"
#include "test_helper.hpp"
#include "mj2x/backend/stackc/stackc_backend.hpp"
#include "mj2x/mj2x.hpp"
#include "mj2x/postprocessor/cloader_postprocessor.hpp"
#include "mj2x/postprocessor/native_loader_postprocessor.hpp"
#include "mj2x/preprocessor/mark_all_preprocessor.hpp"
#include "jbytecode/jbytecode.hpp"

using namespace mj2x;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;
using namespace ca::str;

// Helper: create a minimal class in memory
static std::unique_ptr<ClassNode> makeSimpleClass(const char* className = "TestClass")
{
    auto cn       = std::make_unique<ClassNode>();
    cn->version   = Opcodes::V1_8;
    cn->access    = Opcodes::ACC_PUBLIC;
    cn->name      = Utf8StringRef::from_cstr(className);
    cn->superName = ZUtf8StringRef::from_static("java/lang/Object");

    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto mn       = std::make_unique<MethodNode>(Opcodes::ACC_PUBLIC | Opcodes::ACC_STATIC,
                                           ZUtf8StringRef::from_static("add"),
                                           ZUtf8StringRef::from_static("(II)I"),
                                           ZUtf8StringRef::from_static(""),
                                           noExceptions);
    mn->maxStack  = 2;
    mn->maxLocals = 2;

    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 0));
    mn->instructions.add(new VarInsnNode(Opcodes::ILOAD, 1));
    mn->instructions.add(new InsnNode(Opcodes::IADD));
    mn->instructions.add(new InsnNode(Opcodes::IRETURN));

    cn->methods.push_back(mn.release());
    return cn;
}

static std::vector<ca::u8> classBytesFor(const char* className)
{
    auto                     cn = makeSimpleClass(className);
    ca::str::Utf8StringArena arena;
    ClassWriter              cw(arena, ClassWriter::COMPUTE_MAXS);
    cn->accept(&cw);
    return cw.toByteArray();
}

static std::vector<ca::u8> annotationInterfaceBytes()
{
    auto                     cn = std::make_unique<ClassNode>();
    ca::str::Utf8StringArena arena;
    cn->version = Opcodes::V1_8;
    cn->access  = Opcodes::ACC_PUBLIC | Opcodes::ACC_ABSTRACT | Opcodes::ACC_INTERFACE |
                 Opcodes::ACC_ANNOTATION;
    cn->name      = arena.intern("pkg/Anno");
    cn->superName = ZUtf8StringRef::from_static("java/lang/Object");
    cn->interfaces.push_back(ZUtf8StringRef::from_static("java/lang/annotation/Annotation"));

    auto* retention =
        cn->visitAnnotation(ZUtf8StringRef::from_static("Ljava/lang/annotation/Retention;"), true);
    retention->visitEnum(ZUtf8StringRef::from_static("value"),
                         ZUtf8StringRef::from_static("Ljava/lang/annotation/RetentionPolicy;"),
                         ZUtf8StringRef::from_static("RUNTIME"));
    retention->visitEnd();

    std::vector<ca::str::Utf8StringRef> noExceptions;
    auto                                addDefaultedMethod = [&](const char* name) {
        auto  mn = std::make_unique<MethodNode>(Opcodes::ACC_PUBLIC | Opcodes::ACC_ABSTRACT,
                                               ZUtf8StringRef::from_static(name),
                                               ZUtf8StringRef::from_static("()Ljava/lang/String;"),
                                               ZUtf8StringRef::from_static(""),
                                               noExceptions);
        auto* defaultValue = mn->visitAnnotationDefault();
        defaultValue->visit(ca::str::Utf8StringRef(), ZUtf8StringRef::from_static("yes"));
        defaultValue->visitEnd();
        cn->methods.push_back(mn.release());
    };
    addDefaultedMethod("val");
    addDefaultedMethod("val2");

    ClassWriter cw(arena, ClassWriter::COMPUTE_MAXS);
    cn->accept(&cw);
    return cw.toByteArray();
}

// Helper: write class bytes to temp file
static std::string writeTempClass(const std::string& suffix)
{
    auto path = test::getTempOutPath("mj2x_" + suffix).string();

    auto bytes = classBytesFor("TestClass");

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

static std::unique_ptr<jzip::LocalFileHeader> makeJarEntry(const std::string&         name,
                                                           const std::vector<ca::u8>& data)
{
    auto       local = std::make_unique<jzip::LocalFileHeader>();
    zip::Crc32 crc;
    crc.update(data.data(), data.size());
    local->setFileName(name);
    local->setGeneralPurposeBitFlag(0x0800);
    local->setCompressionMethod(0);
    local->setCrc32(crc.value());
    local->setCompressedSize(static_cast<ca::i64>(data.size()));
    local->setUncompressedSize(static_cast<ca::i64>(data.size()));
    local->setFileData(data);
    local->setOffset(0);
    return local;
}

struct TestJarEntry
{
    std::string         name;
    std::vector<ca::u8> data;
};

static std::string writeTempJarWithEntries(const std::string&               suffix,
                                           const std::vector<TestJarEntry>& entries)
{
    auto             path = test::getTempOutPath("mj2x_" + suffix).string();
    jzip::ZipArchive archive;
    for (const auto& entry : entries) {
        archive.addPart(makeJarEntry(entry.name, entry.data));
    }
    auto          jarBytes = jzip::ZipIO::writeDirect(archive);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(jarBytes.data()),
              static_cast<std::streamsize>(jarBytes.size()));
    return path;
}

static std::string writeTempJar(const std::string& suffix)
{
    return writeTempJarWithEntries(suffix,
                                   {
                                       {"pkg/A.class", classBytesFor("pkg/A")},
                                       {"pkg/B.class", classBytesFor("pkg/B")},
                                       {"assets/config.txt", std::vector<ca::u8>{'o', 'k'}},
                                   });
}

static std::vector<ca::u8> resourceBytes(const std::string& resource)
{
    auto bytes = test::getResource(resource);
    return std::vector<ca::u8>(bytes.begin(), bytes.end());
}

static std::vector<ca::u8> readJarEntry(const std::string& jarPath, const std::string& name)
{
    std::ifstream       in(jarPath, std::ios::binary | std::ios::ate);
    auto                size = static_cast<size_t>(in.tellg());
    std::vector<ca::u8> bytes(size);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    auto        archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes));
    const auto* local   = archive.getLocalFileByName(name);
    if (local == nullptr) return {};
    return jzip::ZipIO::decompressLocalFile(*local);
}

static std::unique_ptr<ClassNode> readClassFromBytes(std::vector<ca::u8>&      bytes,
                                                     ca::str::Utf8StringArena& arena)
{
    EXPECT_LE(bytes.size(), static_cast<size_t>(std::numeric_limits<ca::i32>::max()));
    auto        cn = std::make_unique<ClassNode>();
    ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
    reader.accept(cn.get(), 0);
    return cn;
}

static MethodNode* findMethod(ClassNode& cn, const char* name)
{
    for (auto* method : cn.methods) {
        if (method->name.to_std_string() == name) return method;
    }
    return nullptr;
}

TEST(IOTest, ClassWriteReadRoundtrip)
{
    auto                     cn = makeSimpleClass();
    ca::str::Utf8StringArena arena;
    ClassWriter              cw(arena, ClassWriter::COMPUTE_MAXS);
    cn->accept(&cw);
    auto bytes = cw.toByteArray();
    ASSERT_GT(bytes.size(), 50);

    ASSERT_LE(bytes.size(), static_cast<size_t>(std::numeric_limits<ca::i32>::max()));
    ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
    ClassNode   cn2;
    reader.accept(&cn2, 0);

    EXPECT_EQ(cn2.name, ZUtf8StringRef::from_static("TestClass"));
    EXPECT_EQ(cn2.methods.size(), 1);
    EXPECT_EQ(cn2.methods[0]->name, ZUtf8StringRef::from_static("add"));
    EXPECT_EQ(cn2.methods[0]->desc, ZUtf8StringRef::from_static("(II)I"));
}

TEST(IOTest, AnnotationDefaultsAndElementNamesRoundtripThroughTree)
{
    ca::str::Utf8StringArena arena;
    auto                     bytes = annotationInterfaceBytes();
    auto                     first = readClassFromBytes(bytes, arena);

    ClassWriter cw(arena, ClassWriter::COMPUTE_MAXS);
    first->accept(&cw);
    auto rewritten = cw.toByteArray();
    auto second    = readClassFromBytes(rewritten, arena);

    ASSERT_EQ(second->visibleAnnotations.size(), 1u);
    auto* retention = second->visibleAnnotations[0];
    EXPECT_EQ(retention->desc.to_std_string(), "Ljava/lang/annotation/Retention;");
    ASSERT_EQ(retention->elements.size(), 1u);
    EXPECT_EQ(retention->elements[0].name.to_std_string(), "value");
    EXPECT_EQ(retention->elements[0].tag, 'e');
    EXPECT_EQ(retention->elements[0].sval.to_std_string(),
              "Ljava/lang/annotation/RetentionPolicy; RUNTIME");

    for (const char* methodName : {"val", "val2"}) {
        auto* method = findMethod(*second, methodName);
        ASSERT_NE(method, nullptr) << methodName;
        ASSERT_NE(method->annotationDefault, nullptr) << methodName;
        ASSERT_EQ(method->annotationDefault->elements.size(), 1u) << methodName;
        const auto& value = method->annotationDefault->elements[0];
        EXPECT_TRUE(value.name.is_empty()) << methodName;
        EXPECT_EQ(value.tag, 's') << methodName;
        EXPECT_EQ(value.sval.to_std_string(), "yes") << methodName;
    }
}

TEST(IOTest, InputLoaderRoundtrip)
{
    auto path = writeTempClass("loader_test.class");

    InputLoader loader;
    auto        index = loader.load(path);
    ASSERT_NE(index, nullptr);

    auto* cls = index->getClass("TestClass");
    ASSERT_NE(cls, nullptr);
    EXPECT_EQ(cls->classNode->methods.size(), 1);

    std::remove(path.c_str());
}

TEST(IOTest, JarInputLoaderAndOutputWriterPreserveClassesAndResources)
{
    auto inPath  = writeTempJar("loader_test.jar");
    auto outPath = test::getTempOutPath("mj2x_loader_out.jar").string();

    InputLoader loader;
    auto        index = loader.load(inPath);
    ASSERT_NE(index, nullptr);
    EXPECT_TRUE(index->isJarInput());
    ASSERT_NE(index->getClass("pkg/A"), nullptr);
    ASSERT_NE(index->getClass("pkg/B"), nullptr);
    ASSERT_EQ(index->jarEntries().size(), 3u);

    OutputWriter writer;
    writer.write(outPath, *index);

    auto resource = readJarEntry(outPath, "assets/config.txt");
    EXPECT_EQ(resource, std::vector<ca::u8>({'o', 'k'}));

    auto classBytes = readJarEntry(outPath, "pkg/A.class");
    EXPECT_EQ(classBytes, classBytesFor("pkg/A"));
    ca::str::Utf8StringArena arena;
    auto                     cn = readClassFromBytes(classBytes, arena);
    EXPECT_EQ(cn->name.to_std_string(), "pkg/A");

    std::remove(inPath.c_str());
    std::remove(outPath.c_str());
}

TEST(IOTest, JarMvpFixturePreservesManifestResourcesAndMultiReleaseClasses)
{
    auto manifest       = resourceBytes("mvp_jar/META-INF/MANIFEST.MF");
    auto config         = resourceBytes("mvp_jar/assets/config.properties");
    auto versionedClass = classBytesFor("mvpjar/MvpJarMain");
    auto inPath         = writeTempJarWithEntries(
        "mvp_fixture.jar",
        {
            {"META-INF/MANIFEST.MF", manifest},
            {"mvpjar/MvpJarMain.class", classBytesFor("mvpjar/MvpJarMain")},
            {"mvpjar/MvpJarHelper.class", classBytesFor("mvpjar/MvpJarHelper")},
            {"assets/config.properties", config},
            {"META-INF/versions/9/mvpjar/MvpJarMain.class", versionedClass},
        });
    auto outPath = test::getTempOutPath("mj2x_mvp_fixture_out.jar").string();

    InputLoader loader;
    auto        index = loader.load(inPath);
    ASSERT_NE(index, nullptr);
    EXPECT_TRUE(index->isJarInput());
    ASSERT_NE(index->getClass("mvpjar/MvpJarMain"), nullptr);
    ASSERT_NE(index->getClass("mvpjar/MvpJarHelper"), nullptr);
    ASSERT_EQ(index->jarEntries().size(), 5u);

    OutputWriter writer;
    writer.write(outPath, *index);

    EXPECT_EQ(readJarEntry(outPath, "META-INF/MANIFEST.MF"), manifest);
    EXPECT_EQ(readJarEntry(outPath, "assets/config.properties"), config);
    EXPECT_EQ(readJarEntry(outPath, "META-INF/versions/9/mvpjar/MvpJarMain.class"), versionedClass);

    std::remove(inPath.c_str());
    std::remove(outPath.c_str());
}

TEST(IOTest, JarInputLoaderRejectsSignedJarMetadata)
{
    auto inPath =
        writeTempJarWithEntries("signed_metadata.jar",
                                {
                                    {"pkg/A.class", classBytesFor("pkg/A")},
                                    {"META-INF/MORPHER.SF", std::vector<ca::u8>{'s', 'i', 'g'}},
                                });

    InputLoader loader;
    EXPECT_THROW((void)loader.load(inPath), std::runtime_error);

    std::remove(inPath.c_str());
}

TEST(IOTest, JarInputLoaderRejectsJarWithoutRootClasses)
{
    auto inPath = writeTempJarWithEntries(
        "resource_only.jar",
        {
            {"META-INF/MANIFEST.MF", resourceBytes("mvp_jar/META-INF/MANIFEST.MF")},
            {"assets/config.properties", resourceBytes("mvp_jar/assets/config.properties")},
        });

    InputLoader loader;
    EXPECT_THROW((void)loader.load(inPath), std::runtime_error);

    std::remove(inPath.c_str());
}

TEST(IOTest, NativeLoaderPostprocessorAddsLoaderClassResource)
{
    CompilerIndex index;
    index.setJarInput(true);
    // 仅注入资源，不开 clinit 注入（对齐 StackC 场景）。
    NativeLoaderPostprocessor postprocessor("META-INF/mj2x/native/app.dll", ".dll", false);
    postprocessor.process(index);

    ASSERT_EQ(index.jarEntries().size(), 1u);
    EXPECT_EQ(index.jarEntries()[0].name, "mj2x/runtime/NativeLoader.class");
    EXPECT_FALSE(index.jarEntries()[0].data.empty());

    ca::str::Utf8StringArena arena;
    auto                     loaderClass = readClassFromBytes(index.jarEntries()[0].data, arena);
    EXPECT_EQ(loaderClass->name.to_std_string(), "mj2x/runtime/NativeLoader");
    EXPECT_NE(findMethod(*loaderClass, "load"), nullptr);
}

TEST(IOTest, NativeLoaderPostprocessorInjectsClinitLoadCallForModifiedClasses)
{
    // 验证 RegC 路径的 clinit 注入：modified 且非 library 且含 ACC_NATIVE 方法的类
    // 应在 <clinit> 头部前插 NativeLoader.load(resourcePath, tempSuffix) 调用。
    // modified 现在也被 indy/ldc 展开置位（纯字节码改写、无 native 方法），
    // 那些类不得注入。
    CompilerIndex index;
    index.setJarInput(true);

    // 构造一个 modified + native 类（模拟 RegC injectIntoClass 后的状态）。
    CompilerClass cls;
    cls.classNode  = std::make_unique<ClassNode>();
    cls.modified   = true;
    cls.isLibrary  = false;
    cls.classNode->name = index.arena().intern("test/Sample");
    {
        std::vector<ca::str::Utf8StringRef> noExcepts;
        cls.classNode->methods.push_back(new MethodNode(
            Opcodes::ACC_PUBLIC | Opcodes::ACC_NATIVE,
            index.arena().intern("nativeOp"), index.arena().intern("()V"),
            index.arena().intern(""), noExcepts));
    }
    index.addClass("test/Sample", std::move(cls));

    // 构造一个仅因 indy/ldc 展开而 modified 的纯字节码类：不得注入 clinit。
    CompilerClass expandedOnly;
    expandedOnly.classNode  = std::make_unique<ClassNode>();
    expandedOnly.modified   = true;
    expandedOnly.isLibrary  = false;
    expandedOnly.classNode->name = index.arena().intern("test/IndyOnly");
    index.addClass("test/IndyOnly", std::move(expandedOnly));

    NativeLoaderPostprocessor postprocessor("META-INF/mj2x/native/app.dll", ".dll", true);
    postprocessor.process(index);

    CompilerClass* out = index.getClass("test/Sample");
    ASSERT_NE(out, nullptr);
    MethodNode* clinit = findMethod(*out->classNode, "<clinit>");
    ASSERT_NE(clinit, nullptr);
    // clinit 应含 NativeLoader.load 调用（INVOKESTATIC mj2x/runtime/NativeLoader.load）。
    bool foundLoadCall = false;
    for (auto* insn : clinit->instructions) {
        if (insn->getOpcode() == Opcodes::INVOKESTATIC) {
            auto* mi = static_cast<MethodInsnNode*>(insn);
            if (mi->owner.to_std_string() == "mj2x/runtime/NativeLoader" &&
                mi->name.to_std_string() == "load") {
                foundLoadCall = true;
                break;
            }
        }
    }
    EXPECT_TRUE(foundLoadCall) << "modified native class clinit missing NativeLoader.load call";

    CompilerClass* indyOnly = index.getClass("test/IndyOnly");
    ASSERT_NE(indyOnly, nullptr);
    EXPECT_EQ(findMethod(*indyOnly->classNode, "<clinit>"), nullptr)
        << "indy/ldc-expanded class without native methods must not get a clinit load call";
}

TEST(IOTest, CLoaderPostprocessorAddsDispatchLoaderClassResource)
{
    CompilerIndex index;
    index.setJarInput(true);
    CLoaderPostprocessor postprocessor;

    postprocessor.process(index);

    ASSERT_EQ(index.jarEntries().size(), 1u);
    EXPECT_EQ(index.jarEntries()[0].name, "mj2x/cloader/Loader.class");

    ca::str::Utf8StringArena arena;
    auto                     loaderClass = readClassFromBytes(index.jarEntries()[0].data, arena);
    ASSERT_NE(loaderClass, nullptr);
    EXPECT_EQ(loaderClass->name.to_std_string(), "mj2x/cloader/Loader");
    auto* registerMethod = findMethod(*loaderClass, "registerNativesForClass");
    ASSERT_NE(registerMethod, nullptr);
    EXPECT_TRUE((registerMethod->access & Opcodes::ACC_NATIVE) != 0);
}

TEST(IOTest, CLoaderPostprocessorAddsSidecarClassForClassOutput)
{
    CompilerIndex        index;
    CLoaderPostprocessor postprocessor;

    postprocessor.process(index);

    auto* loaderClass = index.getClass("mj2x/cloader/Loader");
    ASSERT_NE(loaderClass, nullptr);
    ASSERT_NE(loaderClass->classNode, nullptr);
    EXPECT_EQ(loaderClass->classNode->name.to_std_string(), "mj2x/cloader/Loader");
    auto* registerMethod = findMethod(*loaderClass->classNode, "registerNativesForClass");
    ASSERT_NE(registerMethod, nullptr);
    EXPECT_TRUE((registerMethod->access & Opcodes::ACC_NATIVE) != 0);
}

// 可读源码版(mj2x/cloader/Loader.java)与生成器防漂移守卫:关键结构面
// (方法名/描述符/NativeLoader 调用形态)必须两侧一致;改任一侧需同步另一侧。
TEST(IOTest, CLoaderJavaReferenceStaysInSyncWithGenerator)
{
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / "mj2x" / "cloader" / "Loader.java";
    ASSERT_TRUE(fs::exists(source)) << source;
    std::ifstream in(source, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

    // 生成器固定发射的方法面(Java 侧签名;JNI 描述符由生成器单测覆盖)。
    EXPECT_NE(text.find("registerNativesForClass"), std::string::npos) << text;
    EXPECT_NE(text.find("invokeMethodHandle1"), std::string::npos) << text;
    EXPECT_NE(text.find("static native void registerNativesForClass(int slot, Class<?> clazz)"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("invokeMethodHandle1(MethodHandle handle, Object receiver)"),
              std::string::npos)
        << text;
    // clinit 注入的调用形态(资源路径配置时)。
    EXPECT_NE(text.find("mj2x.runtime.NativeLoader.load"), std::string::npos) << text;
}

TEST(PipelineTest, PackagedClassSidecarUsesClasspathRoot)
{
    namespace fs = std::filesystem;
    const fs::path root = test::getOutPath("mj2x/packaged_sidecar/.keep").parent_path();
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::create_directories(root / "pkg", ec);
    ASSERT_FALSE(ec) << ec.message();

    const fs::path input  = root / "pkg" / "TestClass.class";
    const fs::path output = root / "pkg" / "TestClass_protected.class";
    const fs::path cFile  = root / "native.c";
    const auto     bytes  = classBytesFor("pkg/TestClass");
    {
        std::ofstream out(input, std::ios::binary);
        ASSERT_TRUE(out);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    PipelineConfig cfg;
    cfg.inputPath  = input.string();
    cfg.outputPath = output.string();
    Pipeline pipeline(std::move(cfg));
    auto     backend = std::make_unique<StackCBackend>();
    backend->setOutputPath(cFile.string());
    pipeline.registerBackend(std::move(backend));
    pipeline.registerPreprocessor(std::make_unique<MarkAllMethodsPreprocessor>());
    pipeline.registerPostprocessor(std::make_unique<CLoaderPostprocessor>());
    ASSERT_EQ(pipeline.run(), 0);

    EXPECT_TRUE(fs::exists(root / "mj2x" / "cloader" / "Loader.class"));
    EXPECT_FALSE(fs::exists(root / "pkg" / "mj2x" / "cloader" / "Loader.class"));
    fs::remove_all(root, ec);
}

TEST(PipelineTest, BadInputPathReturnsError)
{
    PipelineConfig cfg;
    cfg.inputPath  = "nonexistent.class";
    cfg.outputPath = "out.class";
    Pipeline pipeline(std::move(cfg));
    EXPECT_NE(pipeline.run(), 0);
}

// 审查 G-1 修复回归：writeBytes 曾对打不开/写失败只记日志即返回，磁盘满或
// 路径不可写时 pipeline 返回 0（成功）而产物缺失。输出路径位于不存在的目录
// 必须以非 0 退出。
TEST(PipelineTest, BadOutputPathReturnsError)
{
    auto inPath = writeTempClass("pipeline_badout_in.class");

    PipelineConfig cfg;
    cfg.inputPath  = inPath;
    cfg.outputPath =
        test::getTempOutPath("mj2x_badout_dir/.keep").parent_path().string() +
        "/no_such_dir/pipeline_out.class";
    Pipeline pipeline(std::move(cfg));
    EXPECT_NE(pipeline.run(), 0);
}

TEST(PipelineTest, FullPipelineRoundtrip)
{
    auto inPath  = writeTempClass("pipeline_in.class");
    auto outPath = test::getTempOutPath("mj2x_pipeline_out.class").string();

    PipelineConfig cfg;
    cfg.inputPath  = inPath;
    cfg.outputPath = outPath;
    Pipeline pipeline(std::move(cfg));
    EXPECT_EQ(pipeline.run(), 0);

    std::ifstream ifs(outPath, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(ifs);
    EXPECT_GT(ifs.tellg(), 50);
    ifs.close();

    std::remove(inPath.c_str());
    std::remove(outPath.c_str());
}

TEST(PipelineTest, JarPipelineMarksClassesNativeAndPreservesResources)
{
    auto inPath  = writeTempJar("pipeline_in.jar");
    auto outPath = test::getTempOutPath("mj2x_pipeline_out.jar").string();
    auto cPath   = test::getTempOutPath("mj2x_pipeline_out.c").string();

    PipelineConfig cfg;
    cfg.inputPath  = inPath;
    cfg.outputPath = outPath;
    Pipeline pipeline(std::move(cfg));
    auto     backend = std::make_unique<StackCBackend>();
    backend->setOutputPath(cPath);
    pipeline.registerBackend(std::move(backend));
    pipeline.registerPreprocessor(std::make_unique<MarkAllMethodsPreprocessor>());
    pipeline.registerPostprocessor(std::make_unique<CLoaderPostprocessor>());
    EXPECT_EQ(pipeline.run(), 0);

    auto resource = readJarEntry(outPath, "assets/config.txt");
    EXPECT_EQ(resource, std::vector<ca::u8>({'o', 'k'}));
    auto loaderResource = readJarEntry(outPath, "mj2x/cloader/Loader.class");
    EXPECT_FALSE(loaderResource.empty());

    auto                     classBytes = readJarEntry(outPath, "pkg/A.class");
    ca::str::Utf8StringArena arena;
    auto                     cn  = readClassFromBytes(classBytes, arena);
    auto*                    add = findMethod(*cn, "add");
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE((add->access & Opcodes::ACC_NATIVE) != 0);
    auto* bridge = findMethod(*cn, "__mj2x_registerClass");
    EXPECT_EQ(bridge, nullptr);
    EXPECT_NE(findMethod(*cn, "<clinit>"), nullptr);

    std::ifstream cFile(cPath, std::ios::binary);
    ASSERT_TRUE(cFile);

    std::remove(inPath.c_str());
    std::remove(outPath.c_str());
    std::remove(cPath.c_str());
}
