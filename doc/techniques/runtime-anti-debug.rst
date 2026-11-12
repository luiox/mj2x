.. _runtime-anti-debug:

运行时反侦测检查点（Runtime Protection）
==========================================

状态：已实现

受保护方法入口注入检查点，检出调试器 / JVMTI agent 等侦测信号时按预设动作响应。

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 项
     - 值
   * - 对标术语
     - 业界通称 anti-debug / anti-tamper；本仓内称 runtime protection
   * - 作用层
     - 发射端（入口注入）+ 运行时（mj2x-rt ``rt_protect``\ 模块）
   * - 开关
     - ``--runtime-protection <动作>`` / ``--runtime-protection-step`` / ``--runtime-protection-delay <ms>``
   * - 后端
     - RegC ✅ / LLVM ✅ / VMC4 ✅ / StackC ❌
   * - 默认
     - ``none``\ （不注入）

防什么
----------

前面所有技术防的都是**\ 静态分析**\ 。运行时保护防的是**\ 动态观察**\ ：调试器附加、JVMTI agent、动态 instrumentation、单步跟踪。

没有它，其余技术全部可以被跑一遍看结果的方式绕过。

机制
------

受保护方法入口注入一个检查点调用。每次检查点都执行全量信号探测（antidebug + antijvmti：Windows 的 ``IsDebuggerPresent``\ / PEB BeingDebugged / NtGlobalFlag / NtQueryInformationProcess / 硬件断点寄存器 / 命令行扫描，Linux 的 ``TracerPid``\ / 可写可执行段 / agent 库映射 / ``/proc/self/cmdline``\ ）；**\ 概率门控作用于响应而非探测**\，触发按 ``step``\ 确定性下限 + 伪随机提前推进（每 step 次调用至少触发一次），检测命中则必然触发。

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 动作
     - 行为
   * - ``none``
     - 不注入（默认）
   * - ``log``
     - 记录
   * - ``delay``
     - 睡眠 ``delay_ms``\ 毫秒，拖慢分析节奏而非拒绝，不易被定位
   * - ``fake``
     - 返回默认值，**\ 行为静默偏离**\ ，让 trace 结果不可信
   * - ``terminate``
     - 终止

``fake``\ 是对抗价值最高的一档：不崩溃、不报错，只是结果不对。分析者拿到一份看起来正常的 trace，结论却是错的，比 ``terminate``\ 更难处理，且不暴露检查点位置。

状态变量是**\ 函数级全局**\ （``mj2x_rt_cp_N``\ ），由编译管线附加的 ``rt_protect``\ 源提供；LLVM 侧编译时附带该源。检测面细节（信号清单、 fail-open 语义）见 ``mj2x-rt/src/common/mj2x_rt_protect.h``\ 的模块注释与 :ref:`runtime-protection`\ （通用技术综述）。

代价
------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 维度
     - 影响
   * - 性能
     - 每方法入口一次探测调用；``step``\ 越大响应触发越稀疏（探测本身每次都跑）
   * - 误伤
     - **合法环境可能触发**\ 。调试器、profiler、APM agent 都是侦测信号，``terminate``\ 档在用户环境里等于自毁
   * - 体感
     - ``delay``\ 档会真实拖慢生产环境

``terminate``\ 只在明确知道产物不会被合法调试时使用。

已知绕过面
------------

- **检查点是可定位的**\ ：入口的调用指令在产物里是明文，patch 掉即失效。
- **概率触发意味着可漏**\ ：``step``\ 大时短运行可能一次都不触发响应。
- **只查已知信号面**\ ：新型 instrumentation 不在检测列表里就不触发。

与其他技术的关系
------------------

- 与 **safe-env**\ 互补（一个查调试器，一个护 JNI 表）。
- 与 **VM 虚拟化的懒解密**\ 同向：都在提高动态观察的成本。
- 与所有静态技术正交：这一项是唯一的动态面。
