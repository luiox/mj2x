.. _vmx64-intrinsic-channel:

vmx64 intrinsic 双层通道
=============================

状态：设计定稿（未落地，动工时以本文为契约）

vmx64 目前没有 intrinsic 通道：``--backend vmx64``\ 只收纯算术静态方法，产物自包含、不在 VM 循环内外呼 C。本文定稿它接 ``kEntries``\ 注册表的通道设计：按「能否内联成纯 ALU 序列」分两层——Tier A 发射期内联与 Tier B 加密站点间接。前者零符号零调用，后者复用 :ref:`vm-site-table-encryption`\ 的站点表加密机制。RegC 的「直调符号」形态在 vmx64 里不能照抄，原因见本文 对抗什么一节。

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 项
     - 值
   * - 对标术语
     - 无通用业界名；VM 内 intrinsic 通道（直调面隐藏）
   * - 作用层
     - 发射器（handler 体内）＋加密站点表＋运行时 stub
   * - 开关
     - 未定，随 vmx64 intrinsic 批次落地
   * - 后端
     - vmx64（RegC/LLVM 走各自既有通道）
   * - 注册表单源
     - ``kEntries``\ （``regc_intrinsic_registry.cpp``\ ）；vmx64 派生消费同一投影，与 LLVM 的派生模式相同，零改权威表

对抗什么
-----------

vmx64 的保护前提是 opcode 匿名：handler 语义藏在 dispatch 之后，静态分析看到的是一段无名字的指令块＋一张加密站点表。intrinsic 通道若按 RegC 的直调形态接入，会从两个方向破坏这个前提：

#. **调用目标聚类**\ ：release DLL 里内部相对 call 不露符号名，但「哪些 handler 调用同一目标」是公开信息。``Math.sin``\ 的 helper 只有一个，全部 sin 站点都指向它——分析者按调用目标给 handler 分簇，每个簇就是一个 opcode 等价类，VM 化的语义隐藏被逐簇还原。
#. **符号指纹叠加**\ ：RegC 侧已有同样的面——Linux 产物未开 ``-fvisibility=hidden``\ 时 ``mj2x_math_*``\ 全进 ``.dynsym``\ ，``nm -D``\ 可读出 intrinsic 家族面（固定符号集，与被保护方法身份无关，但暴露「用了哪些」）。vmx64 若直调同一批符号，指纹面叠加进产物。

RegC 不在乎这两条，因为它的保护不在 opcode 层：方法入口本就是哈希名＋ ``RegisterNatives``\ 绑定，符号面与被保护方法身份无关。vmx64 不同——它的保护前提正是 opcode 匿名，所以 intrinsic 通道必须把「身份」也藏进站点表机制里，这正是本设计比直调复杂的全部理由。

现状约束
-----------

设计以这些现状事实为边界：

- **handler 内无 call**\ ：Phase 1 的 23 op 全部纯寄存器算术，handler 体不产生任何调用指令；``env`` / ``this``\ 帧槽（``[rbp-0x40]`` / ``[rbp-0x48]``\ ）已为后续 C helper 预留，当前零消费方。
- **帧不对齐**\ ：入口布局使 ``rsp mod 16 == 8``\ （发射器已有警告）。Win64 ABI 要求 call 点 ``rsp mod 16 == 0``\ ——Tier B 动工前必须修帧，这是既有欠账，不是本设计新增的成本。
- **注册表单源后端中立**\ ：``kEntries``\ 的对外投影（``RegCIntrinsicIdentity``\ ＋ ``builtinIntrinsicTable()``\ ）本就是后端中立设计，LLVM 已示范派生消费（``llvm_intrinsic_registry.cpp``\ ）。vmx64 派生同一投影，不读 ``llvmDirect``\ 位（那是 LLVM 侧的直调开关），按条目类别判层。
- **vmx64 无 intrinsic 接线**\ ：``setUserIntrinsics``\ 在 vmx64 侧保持默认空实现，rt 挂载面（``embedded_rt.hpp``\ ）只随发 common 子集。Tier B 若接 ``mj2x-rt``\ 符号，扩挂载即可（该文件已预留注释位）。
- **ExceptionCheck 表驱动**\ ：RegC 侧豁免与否由条目类别派生（``isPureRuntimeIntrinsic``\ ），不是逐条手写。vmx64 沿用表驱动语义：纯标量子集豁免，``precheck``\ 位透传，不另立规则。

Tier A：发射期内联层
-------------------------

**判据**\ ：条目语义可展开为无分支、无调用、无内存效果的纯 ALU 序列。发射器在 handler 体内直接铺指令，产物里零符号、零调用、零重定位。``Math.abs(I)``\ 的展开形态：

.. code-block:: text

   ; Math.abs(I)：符号掩码三连，4 条指令
   mov  edx, eax
   sar  edx, 31
   xor  eax, edx
   sub  eax, edx

覆盖面：Integer/Long 位操作条目（``rol`` / ``ror`` / ``bswap`` / ``popcnt``\ 级）、Math 整数标量（abs/min/max）、Math 浮点单指令条目（``minsd`` / ``maxsd`` / ``roundsd`` / ``sqrtsd``\ ）。RegC 侧这批是 static inline 符号靠宿主编译器 ``/O2``\ 展开；vmx64 自己是编译器，展开在发射期确定，不依赖宿主编译器内联成败——同语义、更确定。

已知语义陷阱（RegC rt 侧踩过的坑，照单全收）：

- ``min/max``\ F/D 的 NaN/±0 语义与 ``minsd/maxsd``\ 的 tie 行为相悖（SSE 相等 tie 返回第二操作数，恰好复现 ±0 缺陷）——必须保留三段分流，不能一条指令了事。
- ``popcnt`` / ``lzcnt`` / ``tzcnt``\ 依赖 BMI1/ABM，基线 x64 不含。两个选项：静态等价序列（``bsr``\ 改写、SWAR popcount，RegC inline 头即此形态）或 CPUID 运行期分叉。动工决断点：默认静态序列（确定性优先）；若选 CPUID 分叉，启动期探测模式本身是可检测指纹，须单独评估。

性能是最优档：内联展开不弱于任何调用形态，比 RegC 的直调还少 call/ret 一对。

Tier B：站点间接层
-----------------------

**判据**\ ：必须出调用的条目（``sin`` / ``pow`` / ``exp`` / ``log``\ 等 libm 族，以及将来任何需要 ``env``\ 的条目）。helper stub 落在产物代码段，但 stub 入口不直接暴露：把 stub 作为站点登记进加密站点表，调用点走与 NEXT 同构的取址序列：

.. code-block:: text

   ; handler 体内调用 helper 站点 t_h（k1 与 t_h 都是发射期常数）
   mov  rax, t_h                  ; 站点号立即数
   mov  r11, <ks64(t_h, k1)>      ; 掩码立即数（k1 方法内常数，发射期算好）
   mov  rdx, [rdi+rax*8]          ; 取加密表项——与 NEXT 同形
   xor  rdx, r11
   add  rdx, r11
   sub  rdx, r12
   call rdx                       ; → stub → stub 内部 call mj2x-rt 符号

三个设计决定：

- **掩码立即数合法**\ ：``k1``\ 是方法实例 seed 派生的方法内常数（见 :ref:`vm-site-table-encryption`\ ），每个调用点的 ``ks64(t_h, k1)``\ 发射期即可算定，以立即数进指令流——与指令流链字 ``tEnc``\ 的消费形态同族。
- **每方法重编号打散聚类**\ ：helper 站点号不取全局固定值，逐方法重新分配——同一个 ``Math.sin``\ 在方法 A 是站点 37、方法 B 是站点 91，调用点立即数互不相同，「按调用目标聚类」升级为「按逐方法密钥流聚类」，跨方法的簇相关性消失。方法内的簇依然存在（同方法内两次 sin 指向同一站点），这是保留的可接受面：单方法内的 opcode 等价类本就受 ``tEnc``\ 链约束。
- **加密器零改动**\ ：post-link 加密器按 magic＋站点数定位、逐项加密（见 :ref:`vm-site-table-encryption`\ ），helper 站点只是让站点数 ``n``\ 变大（u16 域内），数据表布局契约不变，加密对 helper 站点自动生效。

前置工程：帧对齐修复（见本文 现状约束一节）；``env`` / ``this``\ 槽作 stub 传参面。stub 入口做一次 8 字节垫平，保证其内部 call ``mj2x-rt``\ 符号时 ABI 对齐成立。

性能账：间接序列约 10-15 cycle（取址 6 条＋call/ret 一对）；libm 本体 50-500 cycle。相对损耗 <5%，且只落在本来就慢的条目上。Tier A 不付这笔钱。

范围边界
-----------

进 vmx64 的切面按 RegC 已有的纯标量面取：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 层
     - 条目面
     - 形态
   * - Tier A
     - Integer/Long 位操作条目；Math I/J 纯标量；Math F/D 单指令条目
     - handler 体内 ALU 序列
   * - Tier B
     - Math libm 族 8 条（sin/cos/tan/sqrt/pow/exp/log/log10）
     - 加密站点间接 → stub → ``mj2x-rt``\ 符号
   * - 不进
     - 逃逸门控条目（Wrapper/Random/Builder 83 条）与 ``env``\ 依赖条目（arraycopy、String 面）
     - 依赖 RegC 生成 C 的状态机与 materialize 基础设施，照搬即重建半个 RegC；``env``\ 通道建立后另议

用户 intrinsic（``--intrinsic-config``\ ，见 :doc:`/user-intrinsic`\ ）同样按 Tier B 形态接入：用户符号经站点间接调用，与内置条目同机制。

与其他技术的关系
---------------------

- :ref:`vm-site-table-encryption`\ ：站点表加密机制是 Tier B 的地基，加密器零改动复用；本文只新增「表里有 helper 站点」这一事实。
- :ref:`intrinsic-mapping`\ ：``kEntries``\ 注册表单源与识别门槛（语义等价＋逃逸边界＋runtime 符号三件套）；vmx64 派生消费同一投影。
- :ref:`native-lib-encryption`\ ：叠加顺序不变——本通道在链接产物内，整库加密在其之后。

动工验收面
---------------

实现时的契约清单，逐条要测试钉住：

- Tier B 取址序列与发射器的单源重放（NEXT 宏单源测试的同族扩展）。
- 数据表布局契约测试不变——helper 站点只改 ``n``\ 不改布局。
- 每条 Tier A 条目的语义 E2E：NaN/±0/MIN 边界逐条过（陷阱清单见 Tier A 一节）。
- 每方法重编号判定：同一条目在两个方法里的站点号与调用点立即数不同。
