# AGENTS.md

mj2x 是独立的 Java 字节码到 native 编译器仓：把 `.class`/`.jar` 翻译为 C/C++/LLVM IR 并将原方法标记 `native`，经 JNI 保持 Java 运行时互操作。自 morpher monorepo 拆出（拆仓来历与 vendor 清单见 [MIGRATION.md](MIGRATION.md)）。

**仓库私有，不开源**（所有者决定）：商业保护产品线，建仓必须 `--private`；不得公开、不得加公开镜像 remote；对外只走产品发布物。

## 命令

```sh
xmake f --with_tests=y        # 开测试（gtest 走 xrepo 自动拉取）
xmake build <target>          # 一次一个 target
xmake run <target> --gtest_brief=1
```

目标：`mj2x-cli`（产品）、`mj2x-core`（库）、`mj2x-rt`（C 运行时，随生成工程分发）、`mj2x_unittest` / `mj2x_integration_test`（需 `--with_tests=y`）、`mj2x_fake_zig` / `mj2x_fake_xmake`（测试桩 exe）、`mj2x_rt_test`（运行时自测）。

构建开关：`--with_tests=y`、`--with_mj2x_llvm=n`（LLVM IR 后端，默认开）、`--with_mj2x_backend=all|stackc|regc|vmc4|llvm`（#886 单后端编译裁剪）、`--with_asan=y`（不与 gtest target 组合）。全量回归用默认配置（backend=all）。

外部依赖全部走 luiox-repo 中心包仓直连包（libca / libjstr / libjbytecode / libjzip / libgarena / libjdk_finder + xrepo 的 gtest / nlohmann_json），不引入仓内副本、不做 phony 垫片；各包嵌套的 libca 必须用 `add_requireconfs` 固定到同一 `LUIOX_LIBCA_VERSION`。仓内自带件只有两处 vendor：`test_helper/`（测试基建）与 `mtest_set/src/`（manifest + 生成器头文件子集），来历见 MIGRATION.md。

## 约定

- **Doxygen**：`///` 行注释；`/** */` 禁止。
- **No RTTI**：`dynamic_cast`/`typeid` 禁止，用 `ca::core::tag_cast::cast<T>` 家族。
- **Types**：`i32`/`u32`/`usize`/`Result<T,E>`（libca）。
- **Strings**：`Utf8String` 家族；API 借用视图用 `std::string_view`，不新增 `std::string` 存储（字节边界除外）。
- **Includes**：模块内 `"file.hpp"`，跨模块 `"module/path/file.hpp"`（如 `"mj2x-cli/src/..."`、`"manifest/native_fixture_manifest.hpp"`），禁 `../`。
- **测试输出**：`<project_root>/test/`（test-helper API），基线在 `test_resource/`。
- **构建验证**：delete-then-rebuild，内容哈希增量会跳过回到旧状态的文件，采信测试结果前先 `rm <exe> && xmake -r <target>`；构建与验证不并成一条命令。
- **三后端纪律**：StackC/RegC/VMC4 三后端改动须 emit 形单测 + 每后端至少一条 exec 级 E2E（`StackCBackendE2E` / `RegCBackendE2E` / `Vmc4BackendE2E` 矩阵）；跨三后端改动三面都要验。

## 文档规范

三层按**权威来源**分工：扩展名即角色，不按目录分类。

| 载体 | 层 | 写什么 | 谁能引用它 |
|---|---|---|---|
| `///` Doxygen | 微观 | 单个符号的契约：参数、返回、失败模式、不变量；单模块的背景与定位 | 随代码移动 |
| `doc/*.rst` | 宏观 | 跨模块设计、数据模型、schema、格式规格、后端契约 | 代码/测试/构建脚本 |
| `*.md` | 人 + AI 方向 | README、AGENTS、dev_plan、roadmap、迁移方法论、状态表 | 只有人和 AI 读 |

理由：markdown 没有可校验的引用面，无 label、无 include、手写 `§N` 会随编辑漂移，被引用的文档删除后引用悬空且无任何工具报错；RST 自带这三样校验，规范才执行得起来。

硬规则（CI 上由 `doc/check_refs.py`、Sphinx `-W`、Doxygen `WARN_AS_ERROR` 三道检查把关）：

1. 代码/测试/构建脚本只可引用 `doc/*.rst`，路径必须存在。
2. 代码面出现 `.md` 路径即失败。两类豁免：流程文档白名单（AGENTS.md、dev_plan.md、README\*.md、MIGRATION.md、roadmap.md 等，校验存在性）；带仓名标记的跨仓出处（`libjbc`、`morpher`、`mjbase`、`mjt`、`luiox`、`私有仓`），后者视为 prose 不校验存在性。
3. 引用定位用 label，形式 `doc:<label>`；被引用的位置必须有 label（不必给每节都放），命名 `<doc-slug>[-<section-slug>]`。文档内部自指可写本文 §N；跨文档的 `§N` 一律换成 `:ref:`，编号会随插入章节漂移。
4. 跨仓出处带仓名、不用路径形式（`libjbc design.md §35.4` 可，`../x.md` 不可）；在本仓没有副本的规格默认不引，要引就写一句 `<仓名> <文档名>`，不解释它为什么不在这里。
5. 单模块的背景/定位注记写进该模块头文件的 Doxygen 组注释，不建独立文件，它跟代码的耦合决定它随代码一起搬。判据：这段文字是否需要引用别处的代码或文档才说得清。
6. 当前文件只存现在成立的契约。评审轮次编号、卡号、上游验收表编号、免责句一律不写，那些是决策过程记录，归 git log 与 PR 描述。判据：删掉这句话，读者对现在怎么做的理解没有变化，就没必要写。
7. 正文一段一行，禁手工软换行；断行只属于代码块、表格行、指令选项（check_refs.py 第 4 项把关）。提交信息与 PR 正文同规。
8. 仓库文本用平实语言：禁破折号（——）与「」『』引号，改用逗号、冒号或拆句；禁生造词与内部黑话，判据是仓外读者猜不出意思就要改写，例如挂门改为添加检查、落地面改为支持的表面；禁批次与轮次编号进仓库文件，过程记录归 git log 与 PR 描述。规则覆盖 markdown、rst、代码注释与工具输出文案。

过渡豁免已全部移除；现存永久豁免只有三类：流程文档白名单（校验存在性）、带仓名标记的跨仓出处、`test/`、`build/` 生成物报告路径与无目录的裸 `.md` 文件名字面量（生成的报告/模板文件名，非文档引用）。

RST 写作约定（针对中文正文的实测问题）：

- 表格一律 `.. list-table::`，**禁用 grid table**：docutils 按显示宽度校验 grid 边框，全角字形造成 "Malformed table data"，`-W` 下直接失败。标题下划线同理按东亚显示宽度计长，CJK 标题的下划线要比字符数更长。
- 行内代码紧邻中文时必须写 `` ``x``\ 中文 ``（转义空格不能省）：docutils 判 end-string 要求闭定界符后接空白或 ASCII 标点，紧随汉字会报 start-string without end-string。``**强调**`` 同理；子目录文档进 toctree 用相对路径（`techniques/xxx`）。
- 标题层级 `=` / `-` / `^`；代码块标 `.. code-block:: <lang>`（无对应 lexer 的伪代码用 `text`，中文注释的伪 C 也标 `text`，Pygments 的 C lexer 不接受全角标点）；与代码同步的事实（参数默认值、锁定依赖版本）集中一处定义，不跨文档复述。
- 首段写状态行：`状态：定稿/草案/部分实现（已实现部分见本文 §X）`。

Doxygen 写作约定（`///` 走 markdown 解析，以下均为实测结论）：

- 尖括号占位符与 JVM 特殊名（`<init>`、`<clinit>`、`<cstdio>`、`--flag <n>`、`mj2x.param.<id>.desc`）必须包进行内代码，否则被当 HTML 标签报 "Unsupported xml/html tag"。
- 全角标点紧邻、且内容以左括号开头的行内代码段会产出不配对的 `</tt>` 标签（1.18 实测，如中文正文以冒号结尾后紧跟 `((sig)expr)(args)` 形态的代码段）：在起始反引号前留一个 ASCII 空格即可。
- `@`/`\` 字面量（模板占位符 `@NAME@`、键格式 `owner\nname\ndesc`、`#undef`）包进行内代码，否则被当命令报 "Found unknown command"；`@example` 是示例文件引入命令，写示例用文字加行内代码，如：示例：`...`。
- 行首 `-` / `+` 是列表标记：`@brief` 换行续行不能以 `+` 开头（会把后文判成非法列表项）。
- `@ref 目标` 会吞掉紧随的中文标点，目标名后要留空格并用两参形式 `@ref 目标 "显示文字"`；指向类型名的出处直接写行内代码，不用 `@ref`。
- 模块级背景/定位用 `/// @defgroup <slug> <标题>` 放在 umbrella header 顶部，文件名即锚点；组与文件注释各说一半，不复述。
- 检查严格度配置在仓根 `Doxyfile`：`WARN_AS_ERROR=YES` 拦写错的文档，`WARN_IF_UNDOCUMENTED=NO` 不拦没写的，`WARN_IF_INCOMPLETE_DOC=NO` 暂不拦漏写 `@param`（收紧计划见该文件注记）。

命令（从仓库根执行，三道检查）：

```sh
python doc/check_refs.py .                               # 代码引用面校验
sphinx-build -b html -E -W --keep-going doc doc/_build   # -E：增量会吞掉旧警告
doxygen Doxyfile                                         # 微观层（只吃 *.hpp /// 注释）
```

## 加依赖前的判断

新增依赖必须说明它服务于哪个在用的消费方；优先用 luiox-repo 已有包，包不存在的库先在上游发版再消费，不为省事往仓里拷源码副本。vendor 目录（test_helper、mtest_set/src）只做等价同步，不做功能性新增。

## E2E 依赖

后端 E2E 需要：JDK（`JAVA_HOME` 或 PATH，javac/java）、MSVC（vcvars64 + cl）、clang（LLVM/RegC intrinsic 直编，PATH 探测）。缺哪样，对应用例 skip（不算失败，但报数时要如实列出）。

## 提交

- 不提交测试产物与临时文件（`test/`、`build/`、`.xmake/`）；提交前 `git status`。
- 提交信息用约定式前缀并带模块作用域：`feat(core): …`、`fix(cli): …`、`chore(build): …`、`docs: …`。

## 发版

版本号继承 morpher 产品线 v2.x，首个版本 `2.3`，单源是 `xmake.lua` 的 `set_version`，发版 tag 与其同值（`v2.3` …）。

1. 确认 main 分支 CI 通过（linux + windows 两门），改 `set_version("X.Y.Z")` 提交合入；
2. `git tag vX.Y.Z && git push origin vX.Y.Z`；
3. tag push 触发 release workflow：先验该 tag 指向提交的 CI 全绿，再用 git-cliff（`cliff.toml`）生成 changelog（从拆仓基线 `a785a86` 起算，与上个 `v*` tag 之间），`gh release create` 发布 Release。

changelog 只收约定式提交（拆仓后）；monorepo 遗留 `[mj2x]` 格式不入册。本地预览 changelog：`git-cliff a785a86..HEAD`（需 git-cliff）。
