.. _vm-virtualization:

VM 虚拟化（VMC4）
===================

状态：已实现（可用版）

把方法语义编码成自定义指令集的程序 blob，由内嵌解释器执行。原生指令序列不再对应任何公开 ISA，分析者得先逆向这个 VM 本身。

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 项
     - 值
   * - 对标术语
     - jvmtp ``vm.*``\ ；业界通称 code virtualization / VM-based obfuscation
   * - 作用层
     - 后端（``--backend vmc4``\ ）+ 编码器 + 解释器
   * - 开关
     - ``--backend vmc4`` + ``--vmc4-*``\ 一组
   * - 后端
     - VMC4（其他三后端不走这条路）
   * - 默认
     - 关（``backend``\ 默认 ``stackc``\ ）

防什么
----------

前几项技术（MBA / opaque / CFF / 常量加密）都在让原生代码难读这一层做。 VM 虚拟化换了个层次：**\ 根本不生成对应语义的原生代码**\ ，而是生成一个解释器加一份数据。分析者面对的是：

#. 先逆向 VM 的 opcode 语义（每个实例随机，前一个实例的结论不能复用）
#. 再把程序 blob 解码成有意义的指令流

跳过了这两步，看到的只是一段在 switch 里跳来跳去的解释器。

机制
------

随机化面（编译期，由 seed 驱动）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 开关
     - 作用
   * - ``--vmc4-seed``
     - 主 seed，驱动每实例 opcode 映射 / handler 顺序；同 seed + 同输入 → 同产物
   * - ``--vmc4-granularity``
     - ``class``\（默认）/ ``method``\（每方法独立 VM 实例，最强可变性）/ ``program``\（全程序共享，体积最优）
   * - ``--vmc4-dispatcher``
     - dispatcher 形态：``switch_table``\（默认）/ ``funcptr_table`` / ``computed_goto``\（需 GCC/clang）/ ``if_nest`` / ``state_dispatcher``
   * - ``--vmc4-alias``
     - 每语义的 ALT 别名字节数 0..3，同一语义有多个 opcode 编码
   * - ``--vmc4-bogus``
     - 注入不可达假 handler 0..4，只污染静态分析不参与执行
   * - ``--vmc4-shuffle-blocks``
     - 编码期块乱序（Sattolo 单环排列），块 id 空间与物理布局同时打乱，入口块恒 id 0，控制流语义经加密间接跳转表保持
   * - ``--vmc4-super-ops``
     - 指令级：相邻多条 VM 指令合成一条复合指令（``±1``\ → SuperInc/ SuperDec；同块相邻双 Binary → FusedBinary，中间值不再独立成寄存器）。详见 :ref:`vm-super-ops`
   * - ``--vmc4-fragment``
     - 指令级：每条计算指令的操作数经专用临时槽中转，一条拆成先 Copy 后原指令的多段无意义搬运。实测 +34% 解释耗时，面向高保护诉求按需开启。详见 :ref:`vm-super-ops`

前六项改的是**\ 指令的编码与布局**\ （字节到语义的映射、块顺序、加密形态），后两项改的是**\ 指令序列本身的内容与条数**\，这是两个不同的层次，正交可叠。 VM 本体只做到换指令集，指令级变换才进一步做到打散结构。

执行期面
----------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 开关
     - 作用
   * - ``--vmc4-drift``
     - 执行期 dispatch 变体数 0..4；语义标签按字节偏移周期轮转，unmap 表解码
   * - ``--vmc4-drift-period``
     - 变体切换周期（字节偏移单位）
   * - ``--vmc4-encrypt-program``
     - 程序 blob at-rest 加密，解释器加载期解密；产物无明文程序流
   * - ``--vmc4-lazy-decrypt``
     - 按指令对齐 64 字节块独立加密，块首次执行才解密、未走分支保持密文，密钥材料用后擦除
   * - ``--vmc4-block-lazy-decrypt``
     - 按 CFG 块划界的链式懒解密，块密钥 ``(seed, blockIdx, prevKey)`` 链式派生

懒解密的价值在于**\ 覆盖率对抗**\ ：静态 dump 内存只能拿到已执行过的块，未走到的分支保持密文。这也是动态 trace 类攻击的主要反制。

覆盖面
--------

可用版已全覆：对象、字段、数组、调用、异常、intrinsic，真实 jar 转化率 100%。

一个显式的分工决定
--------------------

**VMC4 不走 RegIR transform passes**\ 。MBA / opaque / CFF / 常量加密 / 调用间接化在 VMC4 上全部不适用。理由是重复投入：VM 层已有自己的混淆面（opcode 随机化 / drift / 块乱序 / 加密），再叠 RegIR 变换收益递减，且重复叠加语义风险。

代价
------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 维度
     - 影响
   * - 性能
     - **最重**\ ：解释执行比原生慢 1-2 个数量级。**\ 性能非本后端目标**
   * - 产物体积
     - blob + 解释器；``program``\ 粒度最优、``method``\ 粒度最差
   * - 可调试性
     - 完全没有，栈上只有解释器帧

已知绕过面
------------

- **解释器是通用靶子**\ ：VM 虽然每实例随机，但解释器主体结构是固定的。攻破一次解释器骨架，后续实例的 opcode 映射仍需逐个解，但成本下降。
- **trace 可重建执行过的路径**\ 。drift 与懒解密提高的是这个成本，不是让它不可能。
- **入口/出口是明文**\ ：JNI 边界处的参数与返回值仍是 Java 语义。

与其他技术的关系
------------------

- 与所有 RegIR 变换**\ 互斥**\ （见上）。
- 与 **VM 指令级变换**\ （:ref:`vm-super-ops`\ ）**\ 正交可叠**\ ：本项换指令集与布局，那一项改指令序列的内容与条数。两者都作用在同一条 VM 指令流上但动的是不同维度。
- 与 **运行时保护检查点**\ 可叠（``--runtime-protection``\ 支持 VMC4）。
- 与 **trace 输出**\ 契约：``--vmc4-trace-surface``\ 写期望 trace 输出文件，是 mjdbg 攻击测试的闭环契约（同 seed + 同输入 → 同内容）。新增 opcode（如 FusedBinary）会改变 trace 输出的 opcodeTable 行数，测试侧的支持集镜像必须同步。
