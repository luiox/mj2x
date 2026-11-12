.. _bytecode-translation:

字节码到 IR 翻译
==================

状态：定稿（通用技术综述；本仓实现形态见本文本仓现状节）

将 JVM 字节码指令序列翻译为编译器中间表示（IR）。

.. _bytecode-translation-repo:

本仓现状
----------

mj2x 的翻译链是两级：``frontend/code_converter.cpp``\ 先把字节码切成 L1 MIR 基本块（构建期维护 FrameState 模拟操作数栈，栈值不进产物）；随后 ``SSABuilder``\ 消栈构造 SSA（形态见 :ref:`ssa-construction`\ 本仓现状节）。

- **invokedynamic**\ 在 pipeline 级预处理静态展开（StringConcatFactory / LambdaMetafactory）；残留自定义 bootstrap 站点走**\ 运行时 bootstrap 解析**\ （模块级 cache + bsm 调用，见 :doc:`/user-guide` §5.4）。
- **异常表**\ 按 protected 区域进 MIR，异常边由异常区域记录承载（handler 入口接收异常值）。
- **fail-closed 而非回退解释**\ ：无法安全翻译的构造保持原字节码、不生成 native 代码并给出明确原因，本仓没有回退到 JVM 解释执行的桩路线。

问题
------

JVM 字节码是基于栈的指令集，IR 通常是基于虚拟寄存器或 SSA 形式。翻译需要：

#. 消除操作数栈，将栈上值映射为 IR 变量
#. 处理控制流指令转换为 IR 的分支/跳转
#. 特殊构造（异常表、switch、invokedynamic）的桥接

操作数栈消除
--------------

字节码中，大多数指令从栈读取操作数并将结果压回栈。翻译为 IR 时需要追踪栈的抽象状态。

抽象栈
~~~~~~~~

每条指令执行前，栈上值的类型是可确定的（通过帧分析）。翻译器维护一个抽象操作数栈，每个元素是 IR 变量引用：

.. code-block:: text

   指令序列：                 抽象栈状态（初始：[]）
       iconst_1               → [t1]
       iconst_2               → [t1, t2]
       iadd                   → [t3(t1 + t2)]
       istore v0              → []（t3 → store）

Push/Pull 模式
~~~~~~~~~~~~~~~~

每个 push 指令（``iconst_*``\ 、``ldc``\ 、``iload``\ 、``invokestatic``\ 的返回值等）在 IR 中产生一个 def：

.. code-block:: c

   Value* translatePush(Insn* insn, IRBuilder* ir) {
       switch (insn->opcode) {
       case ICONST_0: return ir->constInt(0);
       case ICONST_1: return ir->constInt(1);
       case BIPUSH:   return ir->constInt(insn->operand);
       case ILOAD:    return ir->loadLocal(insn->slot);
       case IADD:     return ir->add(pop(), pop());
       case INVOKESTATIC:
           return ir->call(insn->method, popArgs(insn->argc));
       // ...
       }
   }

每个 consume 指令从栈弹出操作数。对于 ``IADD``\ ：弹出两个值 → 创建加法 IR 节点 → 压回结果。

局部变量追踪
~~~~~~~~~~~~~~

``istore``\ 将栈顶值写入局部变量 slot，``iload``\ 读取。在翻译的抽象状态中维护一个 slot 到 IR Value 的映射：

.. code-block:: c

   struct TranslationState {
       std::vector<Value*> stack;       // 抽象栈
       std::vector<Value*> locals;      // slot 映射
       std::vector<Value*> args;        // 参数值
   };

控制流翻译
------------

条件跳转
~~~~~~~~~~

.. code-block:: text

   iload x
   iload y
   if_icmplt L_target

翻译为：

.. code-block:: text

   br(L_target, cmp_lt(x, y))      ; 条件跳转到 L_target
   br(L_fallthrough)               ; 否则直落

Switch
~~~~~~~~

``tableswitch``\ 和 ``lookupswitch``\ 翻译为 IR 中的 switch 节点或级联条件链：

.. code-block:: text

   tableswitch 0 3
       case 0: L0
       case 1: L1
       case 2: L2
       default: Ld

   → switch(value) { 0: goto L0; 1: goto L1; 2: goto L2; default: goto Ld; }

当 case 稀疏时，``lookupswitch``\ 更适合翻译为二分查找链。

异常处理
----------

JVM 异常处理是翻译中最复杂的部分。异常表条目 {start, end, handler, type} 定义了 protected 区域。

异常边
~~~~~~~~

CFG 中，异常表 protected 区域内的每条指令都可能通过异常边到达 handler。翻译时 try 块中的每条可能抛异常的调用/指令后都需要隐含的异常分支到 handler。

异常状态修复
~~~~~~~~~~~~~~

异常发生时，JVM 的操作数栈被清空，局部变量保持原值，异常引用被压入 handler 的栈。翻译为 IR 时，handler 入口处需接收异常值作为隐式参数：

.. code-block:: text

   handler_entry(e = exception):
       // e 即捕获的异常引用

Finally
~~~~~~~~~

``javac``\ 生成的 finally 块本质上是 try/catch 的复制。翻译时需要识别 finally 模式（复制两份的 try/catch 结构）并做特殊处理，或保持为两个副本。

Invokedynamic 桥接
--------------------

``invokedynamic``\ 的链接发生在运行时，编译器在翻译时不知道实际目标。处理方式：

保持 indy 调用
~~~~~~~~~~~~~~~~

在 IR 中保留 invokedynamic 调用节点，运行时通过 JNI 或辅助函数完成链接：

.. code-block:: text

   IR:
       call_indy(bsm, args)  → 运行时解析

Bootstrap 提前评估
~~~~~~~~~~~~~~~~~~~~

对于 bootstrap 参数全是编译期常量的 ``ConstantCallSite``\ ，编译时可以本地评估 BSM 并确定目标。如果目标方法是 JDK 方法，可以用 intrinsic 替换。

数组与对象操作
----------------

数组
~~~~~~

``newarray`` / ``anewarray`` / ``multianewarray``\ → IR 中的数组分配节点。 ``aaload`` / ``aastore``\ → 数组访问节点。

字段
~~~~~~

``getfield`` / ``putfield``\ → 对象字段访问节点。需要解析字段偏移（JVM 布局）或生成 JNI 调用。

类型检查
~~~~~~~~~~

``checkcast`` / ``instanceof``\ → 类型检查节点。翻译为运行时类型查询（比较类元数据或调用 JNI 函数）。

翻译的保守策略
----------------

对于无法完美映射到 IR 的字节码构造（如 ``jsr/ret``\ 等遗留指令），翻译器应：

#. 尝试用通用 IR 节点表达
#. 不可表达时回退到 JNI 桥接路径
#. 遇到无法安全回退的构造 → 标记失败，不生成代码（fail-closed）
