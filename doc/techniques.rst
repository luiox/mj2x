.. _techniques:

技术文档目录（techniques）
============================

状态：定稿

``doc/techniques/``\ 子树收录 mj2x 的技术文档，分两组：

- **编译技术** —— 把 JVM 字节码编译成原生代码的通用技术与本仓实现形态
- **保护技术** —— 消除可还原锚点、对抗逆向的单项技术（族划分与组合纪律见
  :ref:`protection-overview`）

单项技术分篇回答四个问题：叫什么、对抗什么、机制是什么、代价是什么。

编译技术
----------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 分篇
     - 主题
   * - :doc:`/techniques/ssa-construction`
     - SSA 构造：CFG → SSA IR、phi placement、支配树、重命名（本仓实现为
       block-argument 形态）
   * - :doc:`/techniques/bytecode-translation`
     - 字节码到 IR：操作数栈消除、异常表、switch、indy 桥接
   * - :doc:`/techniques/codegen-strategies`
     - 原生代码生成：栈机 vs 寄存器、lowering、C codegen
   * - :doc:`/techniques/jni-bridge`
     - JNI 桥接层：引用管理、字段/方法 ID、字符串、异常
   * - :doc:`/techniques/intrinsic-mapping`
     - 编译器内联函数：JDK 方法识别、语义等价、原生替换
   * - :doc:`/techniques/runtime-protection`
     - 原生运行时保护：反调试、反 JVMTI、加密原语

保护技术
----------

按作用层分族；开关联动、组合顺序与后端覆盖面见 :ref:`protection-overview`。

A. 数据流混淆（RegIR pass）：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 分篇
     - 技术
     - 开关
   * - :doc:`/techniques/mba`
     - MBA（混合布尔算术）
     - ``--regc-obfuscate`` / ``--regc-mba-deep``
   * - :doc:`/techniques/opaque-predicate`
     - 不透明谓词
     - ``--regc-obfuscate`` / ``--regc-opaque-zero``
   * - :doc:`/techniques/constant-hiding`
     - 立即数隐藏（常量加密）
     - ``--regc-hide-constants``

B. 控制流混淆（RegIR pass）：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 分篇
     - 技术
     - 开关
   * - :doc:`/techniques/control-flow-flattening`
     - CFF 控制流平坦化
     - ``--regc-obfuscate`` / ``--regc-cff-*``
   * - :doc:`/techniques/computed-jumps`
     - computed jumps
     - ``--regc-computed-jumps``

C. 调用图混淆：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 分篇
     - 技术
     - 开关
   * - :doc:`/techniques/call-indirection`
     - 调用目标间接化
     - ``--regc-indirect-calls``

D. 结构降级与边界消除：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 分篇
     - 技术
     - 开关
   * - :doc:`/techniques/native-direct-call`
     - native→native 直接 C 调用
     - ``--regc-direct-calls``\（三档）
   * - :doc:`/techniques/pod-struct`
     - POD 类降级为 C struct
     - ``--regc-pod-struct``
   * - :doc:`/techniques/vm-virtualization`
     - VM 虚拟化（VMC4）
     - ``--backend vmc4`` + ``--vmc4-*``
   * - :doc:`/techniques/vm-super-ops`
     - VM 指令级变换（super-ops / 指令碎片化）
     - ``--vmc4-super-ops`` / ``--vmc4-fragment``
   * - :doc:`/techniques/vm-regfile`
     - 寄存器文件定长化（线性扫描 + 溢出槽）
     - ``--vmc4-regfile <n>``

E. 运行时与产物保护：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 分篇
     - 技术
     - 开关
   * - :doc:`/techniques/jni-table-protection`
     - safe-env（JNI 函数表保护）
     - ``--regc-safe-env``
   * - :doc:`/techniques/runtime-anti-debug`
     - 运行时反侦测检查点
     - ``--runtime-protection``
   * - :doc:`/techniques/string-encryption`
     - 字符串 at-rest 加密
     - ``--strings-encrypt``
   * - :doc:`/techniques/native-lib-encryption`
     - native 库 at-rest 加密
     - ``--native-lib-key``
   * - :doc:`/techniques/vm-site-table-encryption`
     - vmx64 站点表 post-link 加密
     - ``--backend vmx64`` + ``--compile``\（无独立开关）

未成篇但已存在的保护面
------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 技术
     - 开关
     - 说明
   * - RegisterNatives 动态绑定
     - 总是开
     - 函数名/可见性与 JNI 符号查找完全脱钩，绑定靠 finalize 发射的注册表
   * - 方法内联
     - ``--regc-no-inline``\ 反向控制
     - SSA 内联默认开，消除 native 方法间 JNI 往返
   * - 用户 intrinsic
     - ``--intrinsic-config``
     - 任意 Java 静态调用点降级为用户 native symbol 直调，见 :doc:`/user-intrinsic`
   * - 混淆作用域（横切）
     - ``--obf-scope-include`` / ``--obf-scope-exclude``
     - 决定上面这些 RegIR 变换作用于哪些类（类级 glob，两者都为空 = 零行为
       变化）。本身不是一项技术，见 :ref:`protection-overview`
