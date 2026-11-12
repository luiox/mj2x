.. _ssa-construction:

SSA 构造
==========

状态：定稿（通用算法综述；本仓实现形态见本文本仓现状节）

从 JVM 字节码的控制流图构造静态单赋值（Static Single Assignment）中间表示。

.. _ssa-construction-repo:

本仓现状
----------

mj2x 的实现（``mj2x-core/src/mj2x/mir/ssa/``\ ）**\ 不走显式 phi 节点路线**\ ，是 block-argument 形态的 SSA（MLIR 风格）：

- ``SSABuilder``\ 先对 local slot 算 reaching-def 视图，跨块携带的值开**\ 块参数**\（``SSABlock``\ argument），不做支配树 / dominance-frontier 计算，也没有 DFS 重命名 pass，版本号即 ``SSAValue``\ 对象身份。
- 块参数采用简化策略：所有在汇合块有 ≥1 def 的 local slot 都开 arg（≥2 源是真合并；1 源是冗余但正确），不追求最小化。
- 下文关键边分裂 / 并行复制在 RegIR 侧对应**\ 边拷贝**\（``RegCopy``\ 记录，phi/block-argument 消除产物，挂在 CFG 边上随块发射）。
- JVM 操作数栈在构造中被消除：栈 shuffle（POP/DUP/DUP2_X*/SWAP）只存在于构建期 FrameState，发射出的 SSA 指令只引用稳定的 ``SSAValue``。

经典 phi 放置 / 重命名算法（下文）是通用基线，用于对照与阅读外部文献。

问题
------

JVM 字节码中，局部变量表是可复写的，同一 slot 在不同时间点被多次赋值：

.. code-block:: text

   iconst_1
   istore v0      ; v0 = 1
   ...
   iconst_2
   istore v0      ; v0 = 2（覆盖）

在非 SSA 形式中，``v0``\ 的两次赋值构成 def-use 链，分支汇合处需要数据流分析才能确定 ``v0``\ 的实际值。SSA 通过给每个 def 分配不同版本的变量名来消除歧义：

.. code-block:: text

   v1 = 1
   ...
   v2 = 2

SSA 性质
----------

- 每个变量只被赋值一次
- 每个 use 对应唯一的 def
- 控制流汇合处用 φ 节点合并不同路径上的值

.. code-block:: text

   entry:
       if (cond) goto L_then
       v1 = 1
       goto L_merge
   L_then:
       v2 = 2
   L_merge:
       v3 = φ(v1, v2)  // 来自两条不同路径

构造步骤
----------

1. 构建 CFG
~~~~~~~~~~~~~

从字节码构建控制流图：基本块识别、边的分类（直落/条件跳转/无条件跳转/异常边）。

.. code-block:: text

   InsnList → 基本块划分 → CFG(Block[], Edge[])
   每条边标记：fall-through / branch / exception / switch

异常边是 JVM 特有的难题，任何指令都可能抛异常。CFG 中每个 try 块内的指令都需要到 handler 的异常边。

2. 计算支配关系
~~~~~~~~~~~~~~~~~

对于 CFG 中的每个基本块：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 概念
     - 定义
   * - 支配（d dom n）
     - 块 d 支配块 n 当且仅当所有从 entry 到 n 的路径都经过 d
   * - 严格支配（d sdom n）
     - d dom n 且 d ≠ n
   * - 直接支配者（idom(n)\ ，dominator tree）
     - 离 n 最近的严格支配者
   * - 支配边界（DF(n)\ ，dominance frontier）
     - 从 n 出发可到达、但 n 不严格支配的块的集合

支配树通过 Lengauer-Tarjan 算法在 ``O(E α(V))``\ 时间内构建。

3. Phi 节点放置
~~~~~~~~~~~~~~~~~

在支配边界处插入 φ 节点。核心算法：

.. code-block:: text

   for each variable v:
       for each block b that defines v:
           for each block d ∈ DF(b):          // b 的支配边界
               if 没有 phi for v in d:
                   insert phi(v) in d
                   if d ∉ worklist:
                       add d to worklist      // phi 自身也是一个 def

由于 φ 产生新的 def，它可能在其自身的支配边界 DF(d) 中再触发 φ，所以用 worklist 迭代直到收敛。

4. 变量重命名
~~~~~~~~~~~~~~~

深度优先遍历支配树，为每个 def 分配新版本号：

.. code-block:: text

   DFS(block b):
       for each instruction in b:
           replace each use with current version of v
           for each def of v:
               create new version, push to version stack
       for each successor s of b:
           for each phi in s:
               fill phi operand from current version for this predecessor
       for each child c ∈ dominator tree:
           DFS(c)
       pop all versions assigned in b

Phi 的边缘分歧
----------------

φ 的语义是根据进入路径取对应值。在 CFG 中，φ 的操作数按前驱基本块的顺序排列：

.. code-block:: text

   L_then:     → L_merge
   L_else:     → L_merge

   L_merge:
       v3 = φ(v1, v2)

第一个操作数 ``v1``\ 来自路径 L_then → L_merge，第二个 ``v2``\ 来自 L_else → L_merge。对应关系由 CFG 前驱列表的顺序决定。

从 SSA 到后端的 Lowering
--------------------------

φ 节点不是机器可执行的。后端需要将 SSA 形式 lower 为寄存器/栈操作。

关键边分裂
~~~~~~~~~~~~

在每个 φ 的前驱边上插入边分裂基本块，将 φ 的赋值操作转换为边上的复制：

.. code-block:: text

   ; 分裂前
   L_then → L_merge:  v3 = φ(v1, v2)
   ; 分裂后，在边上插入复制
   L_then → L_split:  v3 = v1
   L_split → L_merge

并行复制
~~~~~~~~~~

多 φ 节点需要并行复制语义，所有 φ 的操作数同时读、同时写：

.. code-block:: text

   ; 顺序复制可能出错：
   r1 = r2;    // 覆盖 r1，丢失原始值
   r2 = r1;    // 此时 r1 已经是新值

   ; 应使用临时变量实现并行语义：
   tmp = r2;
   r1  = tmp;
   r2  = tmp;

更高效的方式是用寄存器分配解决：如果 φ 的输入和输出最终分配到同一寄存器，复制可省略。

与 SSA 破坏的关系
~~~~~~~~~~~~~~~~~~~

关键边分裂和并行复制统称为 SSA 破坏（SSA destruction）。好的寄存器分配可以显著减少插入的复制指令数量，当 φ 的输入输出映射到同一物理寄存器时，复制是冗余的。

与 JVM 字节码的特殊映射
-------------------------

JVM 局部变量的可复写特性意味着几乎所有 store 指令都会产生新的 SSA def。局部变量 slot 与 SSA 版本的对应关系：

.. code-block:: text

   Bytecode:        SSA IR:
   istore v0        vk = ...
   istore v0        v(k+1) = ...   新版本
   iload v0         = v(k+1)       用最新版本

JVM 操作数栈在 SSA 构造中被消除，栈上的值退化为 SSA 变量的临时生命期。每个 push 产生一个 def，每个 consume 产生一个 use。中间指令的数量决定了 SSA IR 中的临时变量数量。
