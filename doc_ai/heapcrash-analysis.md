# mj2x 子 xmake 堆崩溃（exit -1073740940 = 0xC0000374）复现与根因分析

> 本文是 A1 崩溃分析（mj2x 子 xmake 堆崩溃）的持久设计笔记：原临时落于
> `work/heapcrash_analysis.md`，A2 评审后按仓库规范移入 `doc_ai/`；
> §6 修复方案的 P0-A/P0-B 已在 fc0a2f1fc 落地。

- 分支：`fix/mj2x-child-env-heapcrash`（基线 dddf3bf2b）
- 日期：2026-09-30
- 结论性质：**原机 0xC0000374 堆崩溃未能在本机复现**；本机完成复现矩阵全量实测（全部 PASS）、
  定位了代码级机制候选（xmake 锁定的 tbox 版本 env 块越界写，上游已修）、并在调查过程中
  发现并修正了一个**会完全阻塞 mj2x 单测线的独立崩溃**（libca `scan_marker_files` + GBK +
  非 ASCII 目录名 → 0xC0000409），该崩溃在调查中一度被误判为"堆崩溃复现"，本文如实记录。

---

## 1. 环境记录

| 项 | 本机值 | 原机记录 | 差异 |
|---|---|---|---|
| xmake | **v3.0.9+master.e8daedeb7**（C:\Users\l1502\xmake\xmake.exe） | v3.0.9+HEAD.2b184e178 | 不同 master 构建 |
| pwsh | PowerShell 7.6.2 | — | — |
| JAVA_HOME | C:\Program Files\Eclipse Adoptium\jdk-17.0.20.101-hotspot\（JDK17） | — | 满足要求 |
| shell | Git Bash（MSYSTEM=MINGW64，subhost=msys） | 同为 Git Bash 口径 | — |
| 并发门禁 | 调查期间实际**无**构建进程在跑（tasklist 核实，多轮） | 门禁并发 | 上下文差异，见 §5.4 |
| 两版 xmake 源码差异 | `git diff 2b184e178 e8daedeb7`：23 个 commit，**全部 Lua 层**（package/target/nvcc/xcodebuild/format 等），C 核心（含 tbox 锁定版本）零差异 | | tbox 均锁定 d0138aad |

构建配置：`xmake f --with_mj2x=y --with_tests=y --with_mtest_set=y`（-m release），
`xmake build mj2x_unittest` 一次通过（119s，含 mj2x-cli 依赖）。

## 2. 结论摘要（TL;DR）

1. **复现矩阵本机全绿**：交接文档标注"原机必现"的两条 gtest 用例在本机三种场景下 18/18 全 PASS。
2. **机制候选（原机 0xC0000374）**：xmake 两个版本锁定的 tbox 子模块 `d0138aad` 在
   `mbstowcs_charset`（UTF-8→UTF-16）有**精确填充 off-by-one**（`s1[r]` 在 `r==n` 时越界写），
   上游 tbox master 已修（加 `r < n` 守卫）；它使 tbox Windows 进程派生的 **env 块构造**
   （process.c：envp 字符串列表逐项 `atow` 拼块）在**最后一个 env 条目恰好填满剩余容量**时
   把块的收尾 `\0` 写到分配外 1 个 wchar——**内容形状依赖的 2 字节堆越界**，堆损伤延迟爆出
   （0xC0000374），与"必现于固定 env、env 一变就好、同一二进制环境变了就开始崩"全部吻合。
   答责三选一：**不是 mergeProcessEnvironment 的构造方式有缺陷**（本机实测其产物块干净：
   130 项/0 大小写重复/0 空值/13.7KB），**是子 xmake（pinned tbox）对特定 env 内容形状的缺陷**；
   父 `xmake run` 的 execv 重建 env 只是触发放大器。
3. **调查中发现独立新崩溃（已修正误判）**：把 xmake 源码仓 clone 进 worktree 后，
   libca `ca::test::setup → scan_marker_files` 对全树目录名做 `.string()`（GBK ANSI 窄化），
   xmake 测试目录含 emoji（`头文件✨`）→ 抛 "No mapping for the Unicode character..." →
   `mj2x_unittest` 启动即 terminate，exit **0xC0000409**。此签名与 0xC0000374 **不同**，
   曾被我误判为"膨胀 PATH 触发堆崩溃复现"，已修正；触发物已移出扫描树，矩阵重跑全绿。
   **调度方的"极简 PATH 成功 / 本机 PATH 失败"对照实验同样需要排查此类树内杂质污染**
   （若实验机工作树里存在新增的非 ASCII 目录或半成品二进制，结论会翻转）。
4. 修复方案：mj2x 侧加固 mergeProcessEnvironment（去重/剔除空值/失败回退最小 env 重试），
   libca 侧修 scan_marker_files 宽化转换，上游侧升级含 tbox 修复的 xmake 构建。详见 §6。

## 3. 复现矩阵实测（本机，最终干净树，全部 exit 0）

| 场景 | 用例 | 次数 | 结果 |
|---|---|---|---|
| S1 `xmake run -y mj2x_unittest --gtest_filter=…` | CliTest.GenerateXmakeAndCompileBuildsNativeLibrary | 3（前一轮另 3） | 6/6 PASS |
| S1 同上 | CliTest.XmakeCompilerBuildsRegCJarAndCleansTemporaryProject | 3（前一轮另 3） | 6/6 PASS |
| S2 bash 直接跑 build/windows/x64/release/mj2x_unittest.exe | 两用例各 3 | 6 | 6/6 PASS |
| S3 `xmake l` os.iorunv 隐式继承 / `{envs={}}` 显式空 env spawn 子 xmake configure | work/scenario3.lua 两变体各 3 | 6 | 6/6 PASS |
| 补充：膨胀 PATH（2026→3257 字符，40 个重复目录）后 S1 | T1 | 3 | 3/3 PASS（干净树；见 §4 修正） |

**与原机矩阵不符**（原机 S1 必现 FAIL）。上下文差异逐项核实：
- xmake 构建不同（e8daedeb7 vs 2b184e178）——但两者 C 核心与 tbox 锁定版本完全一致，
  Lua 层差异与本崩溃无关，**不足以解释**；
- 原机崩溃复现于**门禁并发**上下文；本机调查全程核实无并发构建进程；
- env 内容不同（本机 bash 块 87 项/5595 wchar，见 §5.3 诊断）。
  → 按交接文档预案，未强行宣称复现，转入机制定位。

## 4. 重大插曲：两个崩溃签名的辨析（调查误判与修正）

调查中途（06:29 前后）出现"膨胀 PATH 下 `xmake run` 崩溃"（child exit **0xC0000409**，
连 `--gtest_list_tests`/`env -i` 也崩）。cdb 定位为**未捕获 C++ 异常**（e06d7363 second chance →
terminate → fastfail），消息：

```
No mapping for the Unicode character exists in the target multi-byte code page.
```

根因（libca master `libca/test/src/libca/test/test.cpp:38-73`）：

```cpp
void scan_marker_files(const std::filesystem::path& root) {
    ...
    for (auto& entry : iter) {
        if (entry.is_directory()) {
            auto name = entry.path().filename().string();   // :48 宽→窄（ACP=936/GBK）
```

`ca::test::setup` 从 CWD 递归扫描全树找 `.project_root_file`（黑名单仅 `.开头/build*/node_modules`），
对**每个目录名**做 `.string()` 窄化。我 06:07 为对照两版 xmake 把源码仓 clone 进了
`work/xmake_src/`，其测试目录含 emoji：`tests/projects/c/Unicode 测试/头文件✨`、`源文件🎆`
——GBK 无法映射 ✨/🎆 → 抛异常 → `main` 无捕获 → 0xC0000409。

- 时间线完全吻合：05:46–06:05 矩阵 12/12 绿（clone 前）→ 06:15 clone/checkout 出 emoji 目录 →
  06:29 起全崩。
- **影响**：我曾据此报告"膨胀 PATH 触发崩溃复现"，该结论作废；移出 clone、恢复干净树后
  膨胀 PATH 3/3 绿（§3 补充行）。
- **教训**（对门禁侧同样适用）：这种树内扫描类故障会让"环境变量对照实验"系统性误判——
  实验组/对照组若树状态不同（新增目录、半成品 exe、他人并发重建），结论不可信。
  本机期间另有第三方（非本任务会话）于 06:31 在本 worktree 触发过一次外部构建，
  产物 exe 一度启动即崩（foreign 增量对象），已通过强制重链消除并在干净基线上重测。
- 该 bug 本身值得修（见 §6 P0-libca）：任意树内非 ASCII 目录即可瘫痪整条单测线，且签名
  （0xC0000409）与堆崩溃（0xC0000374）极易混淆、污染事故归因。

## 5. 根因分析（原机 0xC0000374）

### 5.1 排除项

- 工程陈旧 `.xmake` 状态：维持交接文档排除结论（失败目录无 .xmake）。
- mergeProcessEnvironment 产物形状缺陷：本机实测（临时诊断代码，MJ2X_ENV_DEBUG=1，
  未提交，存 stash@{0}）：
  - 父进程原始块：87 项 / 0 伪变量(`=C:`已跳过) / 0 大小写重复 / 0 空值 / 最长值 2210 wchar；
  - 合并注入块：130 项 / 0 ci-重复 / 0 空值 / 最长 3746 字节 / 总 13730 字节；
  - vcvars `set` 文本解析产物即全量捕获（130 项，含 cmd 继承的父环境），与父环境完全兼容。
  → 构造方式本身在本机环境无病理形状。
- `xmake run` 上下文（S1）与直接 exe（S2）行为无差异（全绿）。

### 5.2 机制定位：pinned tbox 的 env 块构造越界写（代码级证据）

xmake 两版均锁定 tbox `d0138aad74d1dda1fffbee2f6fc0a8f869d8481c`（`git ls-tree` 核实）。
该版本 `src/tbox/libc/stdlib/mbstowcs.c`：

```c
// pinned d0138aad（bug 版）
    if (r >= 0) s1[r] = L'\0';          // r == n（精确填满）时写 s1[n]，1 wchar 越界
// tbox master（已修）
    if (r >= 0 && r < (tb_long_t)n) s1[r] = L'\0';
    // 注释：only terminate when the result fits, to avoid writing s1[n] on exact fill
```

tbox Windows 进程派生（`src/tbox/platform/windows/process.c:407-449`）在**显式 envs** 派生时
用该函数把 `"K=V"` 字符串列表逐项转宽拼成环境块：

```c
tb_size_t space = n * 3 / 2;
... else if (size + space > maxn) { maxn = size + space + TB_PATH_MAXN; realloc(maxn + 1); }
tb_size_t real = tb_atow(environment + size, p, maxn - size);   // 容量无 +1 富余
size += real; environment[size] = L'\0'; size++;
...
if (environment) environment[size++] = L'\0';                    // 收尾双 \0
```

算术推演（ASCII 项，real=n，space=1.5n）：每个条目写入后剩余容量 ≈ `0.5*n_i + 4095`
（TB_PATH_MAXN=4096）。**当最后一个条目的长度恰好等于前一模式下的剩余容量**（如前一
PATH 项 3000 字符 → 末项恰 5595 字符）时，`size` 推进到 `maxn+1`，收尾 `\0` 写到
`maxn+1` 下标——**分配外 2 字节堆越界**。这是内容形状依赖、确定性触发的 off-by-one：
- env 固定 → 必现；env 任一变化（去重、增删一项、长度变一格）→ 不触发；
- 与"同一 xmake 二进制 8 月装包成功、9 月开始崩"（环境状态改变而非二进制差异）吻合；
- 堆损伤是 2 字节，**延迟爆出**：写坏邻块元数据后，直到之后某次分配/free 才被堆管理器
  发现 → 0xC0000374，时点落在"VS2022 检测之后、写工程 .xmake 之前"——因为 configure 里
  第一个**带显式 envs** 的子派生（工具链/包上下文的子命令）恰在 VS 检测后发生，损伤在
  该点之后首次被踩实；
- 触发点在**派生 env 块的进程内**——原机场景即子 xmake 自己（其后续带 envs 的工具派生），
  与"子进程 0xC0000374"观测一致；mj2x-cli 侧 mergeProcessEnvironment 只是把这个病态形状
  原样递给了子 xmake。

xmake 自身对此类敏感性的旁证：`core/base/os.lua:1039-1042` 对 **>4096 字符**的 env 值
显式调 `os._deduplicate_pathenv` 去重后再派生——官方已知超长/重复 env 值是事故源。

### 5.3 本机诊断数据（临时诊断输出，存 work/*.log，未提交）

- `diag_cli_direct.log`：raw 87 项 / merged 130 项 13.7KB（详单 260 行 KEY 行）；
- 崩溃对照期日志：`repro_bloat1*.log`（0xC0000409，已归因 §4）、`cdb_out*.txt`、`cdb_exr.txt`
  （e06d7363 异常记录：Param[0]=0x19930520）、`cdb_sym.txt`（模块表无第三方注入 DLL）。

### 5.4 遗留不确定性（如实声明）

1. 本机未能触发真 0xC0000374，上述机制是**代码级证实存在的缺陷** + 与全部观测吻合的
   最佳解释，而非本机复现实证。确证手段：在原机 env 快照下用 debugger 断 child xmake 的
   `environment[size++]` 收尾写（或开 full pageheap 跑一次原机失败用例，若 0xC0000374 变为
   立即的 heap OOB 报告即坐实）。
2. 调度方"极简 PATH → install ok"结论方向与本机制一致（env 形状改变避开边界重合），
   但**本机干净树上膨胀 PATH 不触发**（3/3 绿），且调度方实验树状态未核验（§4 教训），
   建议在干净树上重跑该对照再采信；其更早的观测（minhook 子 xmake VS 检测两行后静默
   退出、`failed(-1)`）本身与堆崩溃签名**不符**（exit -1 是 xmake 常规错误退出码，本机
   probe 实测 `os.vrunv` 对 exit(-1)/exit(0xC0000374) 分别原样报告 failed(-1) /
   failed(-1073740940)，且 WER 无 xmake 崩溃事件），大概率是另一个失败（如 msys-subhost
   选错工具链，本机 bash 上下文即复现 `--plat=mingw` + `cannot get program for cc`）。
3. 调查期间本 worktree 曾被第三方会话并发构建（06:31 产物启动即崩，已强制重链修复）。
   本文档只对最终干净基线（§3 矩阵）的结论负责；复现实验期间务必独占构建目录。

## 6. 修复方案（不实现，仅方案 + 验收）

### P0-A mj2x：mergeProcessEnvironment 加固（防御层 1）

- 位置：`mj2x/mj2x-cli/src/native_compiler.cpp:188-200`（函数本体）；调用点
  `mj2x/mj2x-cli/src/native_project_builder.cpp:174-175`。
- 思路：
  1. 对 captured 与父环境做 **case-insensitive 键归并**（现逻辑已有）之外，增加
     **PATH/INCLUDE/LIB 等路径族值去重**（splitenv→unique→joinenv），剔除**空值条目**
     与**含 `=` 的键**（现父枚举已跳过 `=C:`，但 vcvars 文本解析产物无此防御）；
  2. 总块超阈值（如 16KB）时按「先去重后截断超长路径族」收缩——与 xmake 官方
     `os._deduplicate_pathenv`（>4096 触发）同思路；
  3. 目的不是"修复 tbox 越界"（内容巧合无法确定性规避），而是**统计性去险 + 形状卫生**。
- 说明：维持显式全量注入语义不变（#1016 语义保持），非嵌套场景零变化。

### P0-B mj2x：NTSTATUS 失败回退重试（防御层 2，确定性逃生口）

- 位置：`mj2x/mj2x-cli/src/native_project_builder.cpp:143-179`（buildXmakeProject 的
  configure/build 两段 `runXmake`）。
- 思路：`output.status.code` 为负（Windows NTSTATUS/异常退出，如 -1073740940/-1073740791）
  且非常规错误退出（排除 -1）时，**重建命令重试一次**，重试改用**最小 env**：不走
  mergeProcessEnvironment 全量注入，仅 `command.env(K,V)` 注入 captured 的
  PATH/INCLUDE/LIB/LIBPATH/TEMP/TMP 等关键覆盖集，其余整块继承（libca Command::env
  文档语义：覆盖集+继承）——env 块形状完全改变，恰好绕开内容巧合；重试仍失败再报错。
- 日志：重试发生时 CA_LOGT_WARN 输出首退码，便于线上归因。

### P0-C libca：scan_marker_files 宽化转换（本调查新发现的独立崩溃）

- 位置：libca `libca/test/src/libca/test/test.cpp:48`（`entry.path().filename().string()`）。
- 思路：目录名比较用 `u8string()`/`wstring` 或 `.generic_string()` 换 `lexical` 等价方案；
  并对 `recursive_directory_iterator` 迭代异常用 `error_code` 重载兜底（抛错→跳过该项）。
- 依据：任意树内非 ASCII 目录名（emoji/生僻字）在 ACP=936 机器上让 `mj2x_unittest` 启动
  即 0xC0000409，门禁整线瘫痪，且签名易与堆崩溃混淆（本调查已实际误判一次）。

### P1 上游：xmake/tbox

- 报告/升级：pinned tbox `d0138aad` 的 `mbstowcs_charset` 精确填充 off-by-one 已在
  tbox master 修复；推动 xmake 升级 submodule 或本地补丁构建，才是原机崩溃的**根修**。
- 门禁侧短期规避（交由调度方）：给派生 env 做 P0-A 同款去重/瘦身，或统一 PATH 预处理。

### 验收标准（修复实现后）

a) `xmake run -y mj2x_unittest --gtest_filter='CliTest.GenerateXmakeAndCompileBuildsNativeLibrary'`
   与 `'CliTest.XmakeCompilerBuildsRegCJarAndCleansTemporaryProject'` 各**连续 5 次**全绿；
b) 直接 `build/windows/x64/release/mj2x_unittest.exe` 同 filter 各 1 次绿；
c) `xmake run -y mj2x_unittest` 全量 714 用例一轮绿（约 30min）；
d) mj2x-cli smoke（`--generate-xmake … --compile` 产物 DLL 可被 java 加载）正常；
e) 语义保持：子 xmake 日志含 `checking for Microsoft Visual Studio (x64) version ... 2022`
   与 msvc overlay 生效（INCLUDE/LIB 为 vcvars 值）、`-a x64` 不回退（现基线
   diag_cli_direct.log 已留档该证据）、非嵌套（bash 直接跑 CLI）行为不变；
f)（调度方补充项）膨胀 PATH 环境下 `xmake run` 嵌套 5 连绿——**本机警示**：干净树上
   膨胀 PATH 本来就不崩（§3），该项只能防回归、不能证明修复触及原机崩溃；若需证明，
   须在原机 env 快照下构造边界重合用例（单测：合成 env 列表使末项长度 = 0.5×前项+4095，
   断言修复后子进程存活——best-effort，标记为实验性）。

## 7. 调查残留物清单（均未提交）

- `git stash@{0}`："temp-env-diag"——native_compiler.cpp 的 MJ2X_ENV_DEBUG 诊断
  （raw 块/merged 块统计），复现时可恢复使用；
- `work/`（gitignored）：全部复现日志、scenario3.lua、probe_exit/probe_vexit/
  probe_minhook*.lua、cdb 输出、env 快照；
- 两版 xmake/tbox/libca 源码 clone 已移至 `C:\sw\tmp_heapcrash_refs\`（扫描树外）；
- 注意：全局 xmake 模块中有调度方临时探针（`modules/package/tools/xmake.lua` →
  `C:/sw/tmp_probe/child_spawn_dump.txt`），本任务未触碰，由调度方清理；
- 本 worktree 曾被第三方会话并发构建（§5.4-3），建议门禁重跑前确认构建目录独占。
