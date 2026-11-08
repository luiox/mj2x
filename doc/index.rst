mj2x 设计规格
=============

跨模块设计、数据模型与格式契约的权威文本。微观契约（参数、返回、失败模式）
在代码 ``///``\ 注释里随代码移动，本目录只管「为什么这样设计」与「契约是什么」。
分层规则见 ``AGENTS.md``\ 的「文档规范」一节；README、dev_plan、roadmap 等
计划/方向文档属 markdown 层，不进本 doctree。

代码注释以 ``doc:<label>``\ 形式指向本目录的具体小节；``check_refs.py``\ 校验这些
指向不会腐烂。下列每篇文档顶部的根 label 与文件 stem 同名。

.. toctree::
   :maxdepth: 2
   :caption: 设计与契约

   architecture
   user-guide
   user-intrinsic
   config-file
   compile-system
   stackc-backend
   regc-backend
   regc-test-strategy
