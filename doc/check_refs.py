#!/usr/bin/env python3
"""代码引用面门禁：规范见 AGENTS.md「文档规范」（模板出自 mjbase 仓）。

只认三类可校验事实，其余一律失败：

1. ``doc:<label>`` 必须能在 doc/**.rst 里找到 ``.. _<label>:`` 声明；
   同理 ``:ref:``/``:doc:`` 目标在 RST 侧自检（label 唯一 + doc 文件在册）。
2. 代码面出现的 ``doc/...rst`` 路径必须存在。
3. 代码面不得引用设计内容形态的 ``.md``。豁免只有：流程文档白名单（AGENTS/
   dev_plan/README/MIGRATION/roadmap）；带仓名标记的跨仓出处（libjbc /
   morpher / mjbase / mjt / luiox / 私有仓）；以及下方 TRANSITIONAL 过渡
   白名单（第一批 rst 改造遗留，TODO 第二批收编时移除）。

doc/archive/ 是历史归档（只进不出）：不进 toctree、不做任何 .md/.rst 检查；
长期归宿（删除或压缩）待第二批定。

用法：python doc/check_refs.py [仓库根]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# 代码面：源码、测试、构建脚本。人读文档（README/dev_plan/roadmap）不在此列，
# 它们引用 .md 是允许的。
CODE_ROOTS = (
    "mj2x-cli/src",
    "mj2x-core/src",
    "mj2x-rt/src",
    "mj2x-annotations",
    "unittest",
    "mtest_set",
    "test_helper",
    "cloader",
    "xproj",
    "benchmark",
    "src",
    "xmake.lua",
    "xmake",
)
CODE_SUFFIXES = {".hpp", ".cpp", ".h", ".c", ".lua", ".sh", ".py", ".ps1"}

# 流程文档：仓库级、稳定、允许被代码注释点名，但只校验存在性。
PROCESS_DOCS = {
    "AGENTS.md",
    "dev_plan.md",
    "README.md",
    "MIGRATION.md",
    "roadmap.md",
    "CHANGELOG.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
}
# 跨仓出处的 prose 标记：命中则该行的 .md 提及不参与存在性校验。
UPSTREAM_MARKERS = ("libjbc", "morpher", "mjbase", "mjt", "luiox", "私有仓", "私仓")

DOC_DIRNAME = "doc"
ARCHIVE_DIRNAME = "doc/archive"

# ===== 过渡白名单（TODO 第二批收编时移除）=====
# 仍指向已转 rst 的旧 md 路径（doc/user_intrinsic.md、config_file.md）的代码
# 文件——代码面在第一批文档改造中禁改，第二批统一改线到 doc/<slug>.rst。
TRANSITIONAL_MD_REFS = {"mj2x-cli/src/main.cpp", "mj2x-cli/src/native_compiler.cpp"}
# md 提及的路径前缀白名单：doc/techniques/ 属中观层 spec（第二批转 rst，其间
# rst/doc 内保留 md 相对链接）；doc_ai/ 是 monorepo 时代的一次性分析目录
# （现 gitignore），代码面 30 余处历史引用随第二批代码注释清理收编。
TRANSITIONAL_MD_PREFIXES = ("doc/techniques/", "techniques/", "doc_ai/", "mj2x/doc_ai/")
# 生成物输出路径（gitignore 的 test/、build/ 下）：报告文件名，不是文档引用。
ARTIFACT_MD_PREFIXES = ("test/", "build/")

LABEL_DEF = re.compile(r"^\.\. _([\w.-]+):\s*$")
LABEL_REF = re.compile(r"doc:([a-z][a-z0-9]*(?:-[a-z0-9]+)*)")
RST_PATH = re.compile(r"(?:mj2x/)?doc/[\w./-]+\.rst")
MD_MENTION = re.compile(r"[\w./-]*[\w-]\.md\b")
REF_ROLE = re.compile(r":ref:`([^>`]+)`")
DOC_ROLE = re.compile(r":doc:`([^>`]+)`")
ANON_LINK = re.compile(r"`[^`]*\.(?:md|rst) <([^`>]+\.(?:md|rst))>`_{1,2}")


def in_archive(p: Path) -> bool:
    return ARCHIVE_DIRNAME in p.as_posix()


def code_files(root: Path):
    for spec in CODE_ROOTS:
        base = root / spec
        if base.is_file():
            yield base
        elif base.is_dir():
            for p in base.rglob("*"):
                if p.is_file() and (p.suffix in CODE_SUFFIXES or p.name in {"Dockerfile"}):
                    yield p


def iter_lines(path: Path):
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="replace")
    return enumerate(text.splitlines(), 1)


def rst_files(doc_dir: Path):
    for p in sorted(doc_dir.rglob("*.rst")):
        if in_archive(p):
            continue  # 历史归档：只进不出，不进 toctree 也不做检查
        yield p


def collect_labels(doc_dir: Path):
    labels, files, dup = {}, set(), []
    for p in rst_files(doc_dir):
        files.add(p.name)
        for lineno, line in iter_lines(p):
            m = LABEL_DEF.match(line)
            if m:
                name = m.group(1)
                if name in labels:
                    dup.append(f"{p.name}:{lineno}: label 重复定义 {name}（首见 {labels[name]}）")
                labels[name] = f"{p.name}:{lineno}"
    return labels, files, dup


def toctree_entries(index: Path):
    """index.rst 里 toctree directive 的条目（去扩展名）。"""
    if not index.exists():
        return set()
    inside, entries = False, set()
    for line in index.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith(".. toctree::"):
            inside = True
            continue
        if inside:
            if stripped == "":
                continue
            if stripped.startswith(":"):
                continue
            if not line.startswith("   "):
                inside = False
                continue
            entries.add(stripped.split("#")[0].strip())
    return entries


def transitional_md_ok(mention: str, rst_side: bool, techniques: set[str]) -> bool:
    """过渡白名单命中判定。

    rst 侧不豁免 doc_ai（文档不得指向一次性目录）；techniques/ 现存 md 的提及
    （含省略目录的引用形态，如 ``vm_super_ops.md``_ 链接）两侧都豁免——该子树
    第二批转 rst 后本函数连同 techniques 豁免一并删除。
    """
    if rst_side and mention.startswith(("doc_ai/", "mj2x/doc_ai/")):
        return False
    if mention.startswith(TRANSITIONAL_MD_PREFIXES):
        return True
    return Path(mention).name.lstrip("_") in techniques


def check(root: Path):
    problems = []
    doc_dir = root / DOC_DIRNAME
    if not doc_dir.is_dir():
        return [f"{DOC_DIRNAME}/ 不存在：目录已改名或迁移未落地"]
    labels, rsts, dup = collect_labels(doc_dir)
    problems += dup
    in_tree = toctree_entries(doc_dir / "index.rst")
    techniques_dir = doc_dir / "techniques"
    techniques = {p.name for p in techniques_dir.glob("*.md")} if techniques_dir.is_dir() else set()

    # RST 侧：文档内链必须指向在册文件/已声明 label，匿名 md/rst 链接一律禁止。
    for p in rst_files(doc_dir):
        if p.name != "index.rst" and p.stem not in in_tree:
            problems.append(f"{p.name}: 未挂进 index.rst 的 toctree（Sphinx -W 会报 orphan）")
    for p in rst_files(doc_dir):
        for lineno, line in iter_lines(p):
            for m in ANON_LINK.finditer(line):
                problems.append(f"{p.name}:{lineno}: 匿名文件链接 {m.group(1)}，改用 :ref:/:doc:")
            for m in MD_MENTION.finditer(line):
                if Path(m.group(0)).name in PROCESS_DOCS:
                    continue
                if any(marker in line for marker in UPSTREAM_MARKERS):
                    continue
                if transitional_md_ok(m.group(0), rst_side=True, techniques=techniques):
                    continue
                problems.append(
                    f"{p.name}:{lineno}: 文档点名 markdown 规格 {m.group(0)}——"
                    "本仓设计内容用 :ref:，跨仓出处加仓名标记"
                )
            for m in REF_ROLE.finditer(line):
                target = m.group(1).split("<")[-1].strip().rstrip(">, ")
                if target and not target.startswith("<") and target not in labels:
                    problems.append(f"{p.name}:{lineno}: :ref:`{target}` 无对应 label 声明")
            for m in DOC_ROLE.finditer(line):
                target = m.group(1).split("<")[-1].strip().rstrip(">, ")
                if target and Path(target).name not in rsts and Path(target).stem not in {f[:-4] for f in rsts}:
                    problems.append(f"{p.name}:{lineno}: :doc:`{target}` 指向不在册文件")

    # 代码面：只允许指向 RST 规格层的引用。
    for f in code_files(root):
        rel = f.relative_to(root).as_posix()
        for lineno, line in iter_lines(f):
            for m in LABEL_REF.finditer(line):
                if m.group(1) not in labels:
                    problems.append(f"{rel}:{lineno}: doc:{m.group(1)} 无对应 label 声明")
            for m in RST_PATH.finditer(line):
                rel_path = m.group(0)
                if not (root / rel_path.replace("mj2x/", "", 1)).exists():
                    problems.append(f"{rel}:{lineno}: 文档路径不存在 {rel_path}")
            for m in MD_MENTION.finditer(line):
                name = m.group(0)
                if name.startswith(ARTIFACT_MD_PREFIXES):
                    continue  # 生成物输出路径（test/、build/ 下的报告），非文档引用
                if Path(name).name in PROCESS_DOCS:
                    continue
                if any(marker in line for marker in UPSTREAM_MARKERS):
                    continue
                if rel in TRANSITIONAL_MD_REFS:
                    # TODO 第二批：main.cpp/native_compiler.cpp 的旧 md 引用改线 rst 后移除。
                    continue
                if transitional_md_ok(name, rst_side=False, techniques=techniques):
                    # TODO 第二批：techniques 转 rst、doc_ai 历史引用清理后移除。
                    continue
                if "/" not in name:
                    # TODO 第二批（过渡）：无目录的裸 .md 名——monorepo 时代一次性
                    # 文档的 § 引用残留（regc_codegen.md §1 形态）与测试产物文件名
                    # （TRACE_SURFACE.md 等）。新引用必须写 doc/*.rst 路径或 doc:<label>。
                    continue
                problems.append(
                    f"{rel}:{lineno}: 代码面引用 markdown 规格 {name}——设计内容转 {DOC_DIRNAME}/*.rst，或删指"
                )

    return problems


def main(argv):
    root = Path(argv[1]).resolve() if len(argv) > 1 else Path(__file__).resolve().parents[1]
    problems = check(root)
    if problems:
        print(f"文档引用门禁失败：{len(problems)} 处")
        for p in problems:
            print("  " + p)
        return 1
    print("文档引用门禁通过")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
