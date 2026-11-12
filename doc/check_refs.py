#!/usr/bin/env python3
"""代码引用面门禁：规范见 AGENTS.md「文档规范」（模板出自 mjbase 仓）。

只认四类可校验事实，其余一律失败：

1. ``doc:<label>`` 必须能在 doc/**.rst 里找到 ``.. _<label>:`` 声明；
   同理 ``:ref:``/``:doc:`` 目标在 RST 侧自检（label 唯一 + doc 文件在册）。
2. 代码面出现的 ``doc/...rst`` 路径必须存在。
3. 代码面不得引用设计内容形态的 ``.md``。豁免只有：流程文档白名单（AGENTS/
   dev_plan/README/MIGRATION/roadmap）；带仓名标记的跨仓出处（libjbc /
   morpher / mjbase / mjt / luiox / 私有仓）；test//build/ 下的生成物报告
   路径；以及无目录的裸 ``.md`` 文件名——且该文件名**不命中仓内真实存在的
   任何 .md 文件**（生成的报告/模板文件名字面量，非文档引用；裸名一旦与
   仓内 tracked .md 同名即视为文档引用，转 doc/*.rst 或加白名单）。
4. rst 正文一段一行（AGENTS.md 硬规则 7）：两个连续正文行即手工断行，失败。
   代码块/字面块/指令/表格区的内容豁免——它们的续行是语法的一部分。

doc/archive/ 是历史归档（只进不出）：不进 toctree、不做任何 .md/.rst 检查；
长期归宿（删除或压缩）待后续批次定。

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


# 生成物/结果/夹具根：其下的 .md 是产物或报告落盘（如 test_resource 下
# protected_samples 的 TRACE_SURFACE.md 基线、benchmark/results 报告），
# 代码裸名提及它们仍属「文件名字面量」豁免面；设计文档不在这些根里。
ARTIFACT_MD_ROOTS = ("test_resource", "test", "build", "benchmark")


def tracked_md_names(root: Path) -> set[str]:
    """仓内真实存在的 .md 文件名（排除 .git、doc/ 与生成物根；流程白名单单走
    前置分支）。

    裸 .md 名豁免的收紧依据：豁免本意是「生成的报告/模板文件名字面量」——
    那类文件只落在生成物根或根本不入仓；裸名一旦与生成物根之外的 tracked
    .md 同名即是文档引用，不再豁免。
    """
    names: set[str] = set()
    for p in root.rglob("*.md"):
        parts = p.relative_to(root).parts
        if ".git" in parts:
            continue
        if parts[0] == DOC_DIRNAME or parts[0] in ARTIFACT_MD_ROOTS:
            continue
        if p.name in PROCESS_DOCS:
            continue
        names.add(p.name)
    return names


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


WRAP_UNDERLINE = re.compile(r"^\s*([=\-^~\"'`*+_#])\1{2,}\s*$")
WRAP_FIELD = re.compile(r"^\s*:[\w\-]+(\s[\w\-]+)*:\s")
WRAP_MARKER = re.compile(r"^\s*(?:\* -|- |\+ |\* |#\. |\d+\. |\d+\) )")
WRAP_DIRECTIVE = re.compile(r"^(\s*)\.\. [\w-]+::")


def _skip_indented(lines, i, n, base):
    """跳过缩进大于 base 的连续区（空行穿透，遇缩进回落或 EOF 停）。"""
    while i < n:
        if not lines[i].strip():
            j = i
            while j < n and not lines[j].strip():
                j += 1
            if j >= n or (len(lines[j]) - len(lines[j].lstrip())) <= base:
                return i
            i = j
            continue
        if (len(lines[i]) - len(lines[i].lstrip())) <= base:
            return i
        i += 1
    return i


def manual_wrap_problems(doc_dir: Path):
    """正文一段一行检查（AGENTS.md 硬规则 7）：两个连续正文行 = 手工断行。

    结构行（空行/标题下划线/指令/label/字段选项/列表标记）不参与；指令与
    ``::`` 字面块的缩进内容整体豁免——它们的续行是语法的一部分。
    """
    problems = []
    for p in rst_files(doc_dir):
        lines = p.read_text(encoding="utf-8").splitlines()
        i, n = 0, len(lines)
        while i < n:
            line = lines[i]
            if not line.strip():
                i += 1
                continue
            dm = WRAP_DIRECTIVE.match(line)
            if (dm or WRAP_FIELD.match(line) or WRAP_UNDERLINE.match(line)
                    or WRAP_MARKER.match(line) or line.lstrip().startswith(".. _")):
                i = _skip_indented(lines, i + 1, n, len(dm.group(1))) if dm else i + 1
                continue
            base = len(line) - len(line.lstrip())
            ends_literal = line.rstrip().endswith("::")
            i += 1
            if ends_literal:
                j = i
                while j < n and not lines[j].strip():
                    j += 1
                if j < n and (len(lines[j]) - len(lines[j].lstrip())) > base:
                    i = _skip_indented(lines, i, n, base)
                    continue
            while i < n and lines[i].strip():
                l2 = lines[i]
                if (WRAP_DIRECTIVE.match(l2) or WRAP_FIELD.match(l2)
                        or WRAP_UNDERLINE.match(l2) or WRAP_MARKER.match(l2)):
                    break
                problems.append(f"{p.name}:{i + 1}: 正文手工断行——一段一行（AGENTS.md 硬规则 7）")
                i += 1
    return problems


def check(root: Path):
    problems = []
    doc_dir = root / DOC_DIRNAME
    if not doc_dir.is_dir():
        return [f"{DOC_DIRNAME}/ 不存在：目录已改名或迁移未落地"]
    labels, rsts, dup = collect_labels(doc_dir)
    problems += dup
    in_tree = toctree_entries(doc_dir / "index.rst")
    repo_md_names = tracked_md_names(root)

    # RST 侧：文档内链必须指向在册文件/已声明 label，匿名 md/rst 链接一律禁止。
    for p in rst_files(doc_dir):
        rel_no_ext = p.relative_to(doc_dir).with_suffix("").as_posix()
        if p.name != "index.rst" and p.stem not in in_tree and rel_no_ext not in in_tree:
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

    problems += manual_wrap_problems(doc_dir)

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
                if "/" not in name:
                    # 无目录的裸 .md 名默认 = 文件名字面量（生成的报告
                    # TRACE_SURFACE.md、工程模板 README.md.in 等），不是文档
                    # 引用；但裸名与仓内 tracked .md 同名时即文档引用——收紧
                    # （文档引用必须写 doc/*.rst 路径或 doc:<label>）。
                    if name in repo_md_names:
                        problems.append(
                            f"{rel}:{lineno}: 裸 .md 名与仓内文件同名 {name}"
                            f"——文档引用转 {DOC_DIRNAME}/*.rst，或删指"
                        )
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
