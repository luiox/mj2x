"""Sphinx 配置：mj2x 设计规格层（宏观 + 中观技术分篇）。

规范见 AGENTS.md「文档规范」：本目录收 RST 设计规格（含 techniques/
技术分篇子树）；markdown 属人读/AI 方向层（README/AGENTS/dev_plan/
roadmap），不进 doctree。CI 以 -W --keep-going 构建，警告即失败。
"""

project = "mj2x"
author = "mj2x maintainers"
copyright = "2026, mj2x maintainers"

# 中文正文：docutils 会把全角字形算进表格边框宽度，因此本仓禁用 grid table，
# 表格一律用 list-table（无对齐不变量）。见 AGENTS.md 写作约定。
language = "zh-CN"

root_doc = "index"

# archive/ 是历史归档（只进不出，md 原样保存），不参与构建与引用校验。
exclude_patterns = ["_build", "archive", "Thumbs.db", ".DS_Store"]

# 引用必须可解析：未定义的 :ref:/:doc: 目标在 -W 下即失败。
nitpicky = True
nitpick_ignore = [
    # 设计文档里点名的 C++ 符号不做跨域解析（微观契约归代码 /// 注释层）。
    ("py:class", "doc"),
]

html_theme = "alabaster"
html_title = "mj2x 设计规格"
html_baseurl = ""
html_search_language = "zh"  # 需 jieba；未装分词器时仅影响站内搜索，不影响门禁
html_use_opensearch = ""

# 站点内跳转锚点与 label 同名，便于代码注释里的 doc:<label> 人工回溯。
html_copy_source = True
html_show_sourcelink = True
