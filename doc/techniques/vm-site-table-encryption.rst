.. _vm-site-table-encryption:

vmx64 站点表 post-link 加密
==============================

状态：定稿

vmx64 后端把每个方法编成「加密指令流 + 站点表」交给 ml64/link 产出 DLL。链接产物里站点表默认是**带重定位的明文**：表项字节、key schedule 密钥材料、定位 magic 全部可读，``.reloc``\ 还逐条指认表区——一张函数指针表级的路标。本项在链接之后、交付之前对 DLL 做一次字节级原地改写：表项逐项 XOR 成密文、重定位熄灭、定位痕迹清零。运行时 dispatch 按 r12 的值自动分流「明文 / 加密」两条算术路径，两种落盘形态共用同一份汇编；未加密产物本身可运行，加密是纯可选加固。

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 项
     - 值
   * - 对标术语
     - 无通用业界名；与商业壳的 VM program 加密同族（at-rest 形态）
   * - 作用层
     - post-link（链接产物字节级改写）+ 运行时 dispatch（形态分支）
   * - 开关
     - 无独立开关：``--backend vmx64`` + ``--compile``\ 必经； ``--generate-xmake``\ 只生成工程，不经此步
   * - 后端
     - vmx64（其他后端无站点表）
   * - 实现面
     - 发射器 ``vmx64_asm_emit``\ ＋加密器 ``vmx64_postlink``\ ＋NEXT 宏形态分支；三方契约单源是 ``vmx64_asm_emit.hpp``\ ，本页管「为什么这样设计」与「契约是什么」

对抗什么
------------

不加密时，链接产物 ``.data``\ 段里有三样对静态分析直接可见的东西：

#. **站点表本体**\ ：每项是 ``handler首选VA - ks64(t)``\ 的 64 位线性差。配合 ``.reloc``\ 里的 DIR64 项（逐条指向表区），分析者可以直接确认「这是一张函数指针表」，把全部 handler 地址枚举出来——VM 的 handler 边界、站点数量、控制流复杂度全部现形。
#. **表尾 k1 qword**\ ：key schedule 的密钥材料明文驻留。拿着 k1 就能离线重算全部 ``ks64(t, k1)``\ ，把每个表项还原成 handler 首选 VA，甚至顺手解开指令流的 tEnc 链字。
#. **magic 定位符**\ ：ASCII ``VMX64TBL``\ 。这是给 post-link 加密器用的字节级定位标记（release DLL 无 COFF 符号表，符号定位不可依赖），但它同时也是任何人定位这张表的路标。

三样合起来：不加密的 vmx64 产物，其 VM 化只挡住了 opcode 语义，没有挡住程序结构的枚举。加密后 ``.reloc``\ 不再指认表区、k1 与定位符归零、表项成为与 k1 绑定的密文——静态拿到的是一段无法自证是表的数据。

数据表布局
--------------

发射器为每个方法在 ``.data``\ 里铺一张表，8 字节对齐，全部 qword 小端：

.. code-block:: text

   偏移          qword       内容                                    加密后
   --------------------------------------------------------------------------
   +0x00        magic       ASCII "VMX64TBL"（kVmx64TableMagic）     清零
   +0x08        count       站点数 n（u16 域，1..0xFFFF）             清零
   +0x10        entry[0]    handler首选VA - ks64(0, k1)              XOR ks64(0, k1)
   +0x18        entry[1]    handler首选VA - ks64(1, k1)              XOR ks64(1, k1)
   ...          ...         ...                                      ...
   +0x10+8(n-1) entry[n-1]  handler首选VA - ks64(n-1, k1)            XOR ks64(n-1, k1)
   +0x10+8n     k1          key schedule 密钥（恒非零）               清零
   +0x18+8n     base        表首选 VA（自引用指针）                   保留

两个设计决定：

- **定位靠扫描不靠符号**\ ：magic + 站点数 + 表尾的固定顺序是加密器唯一的定位假设。release DLL（无 ``/DEBUG``\ ）没有 COFF 符号表，符号定位不可依赖。发射器与加密器之间没有共享类型可以把守，这份数据布局的权威文本就是本节与 ``vmx64_asm_emit.hpp``\ ，两侧由 ``DataTableCarriesEncryptorContract``\ 测试对账（见本文 测试映射）。
- **base 是自引用指针**\ ：落盘值恒等于表首选 VA（``dq 表label``\ 的产物），加密与否不改变这个值。它是运行时形态分支的支点——见 :ref:`vm-site-table-encryption-form-branch`。

ks64 密钥流
----------------

全部加密挂在一个 key schedule 上：

.. code-block:: text

   ks64(x, k1) = (rotl64(x * P1, 13) ^ k1) * P2
   P1 = 0x61C88647    P2 = 0x045D9F3B（固定公开常数）

k1 由方法实例 seed 派生（splitmix64 单步，域分隔常数即 ASCII "vmx64"），以立即数发射进入口 prologue（``mov r15, imm``\ ），同时以明文 qword 落在表尾——加密器从表尾取它，与 prologue 用同一 C++ 局部变量，无第二表达。

两个消费点（x 的取值域不同）：

#. **指令流链字**\ ：每条指令开头的 tEnc 字 = 站点号 t XOR (u16)ks64(流偏移, k1)。handler 由 rbx（当前流偏移）重算同值异或解回站点号，低 16 位参与。
#. **站点表项**\ ：全 64 位参与——链接期以 addend 形态藏在 ``dq handler符号 - ks64(t)``\ 里，post-link 加密后以 XOR 掩码形态生效。

公式单源：发射侧 C++（``vmx64Ks64``\ ）、``.asm``\ 宏文本、post-link 加密器三方共享同一公式；改任何常数、移位、顺序都要三面同步（测试映射见本文末节）。

加密三步
------------

消费点在 CLI ``--compile``\ ：xmake 产出 DLL 之后、嵌入/交付之前，对整个文件做一次「读入 → 内存改写 → 原子回写」。三步全部在内存镜像上完成，全部校验通过才落盘。

第 1 步：表项纯异或
^^^^^^^^^^^^^^^^^^^^^^

链接期，发射器给每个表项写的源级表达式是 ``dq handler符号 - ks64(t)``\ 。链接器把符号解析成首选 VA（首选基址 + 段 RVA + 符号偏移），所以**落盘字节**是：

.. code-block:: text

   entry[t] = handler首选VA - ks64(t, k1)

装载时 ``.reloc``\ 的 DIR64 项让装载器给表项加上镜像基差 delta（实际装载基址 - 首选基址），运行时内存里是 ``handler首选VA - ks64 + delta``\ ，即 ``handler运行时VA - ks64``\ ——所以未加密形态下 handler 只需 ``add``\ 回 ks64 就得到真地址。加密器把落盘值逐项改写为纯异或形态：

.. code-block:: text

   entry'[t] = entry[t] XOR ks64(t, k1)

第 2 步：重定位熄灭
^^^^^^^^^^^^^^^^^^^^^^

「熄灭」指把 ``.reloc``\ 中指向表区的 DIR64 项（type 10）的 type 字段翻成 0（ABSOLUTE）——装载器遇到 type 0 直接跳过，不再往目标 qword 加 delta。表区全部 n 个表项 + base qword 共 n+1 条 DIR64 项全部熄灭。不清不行：表项已经是密文，装载器再往上加 delta，运行时无论如何都解不回真地址。

第 3 步：定位痕迹清零
^^^^^^^^^^^^^^^^^^^^^^^^

magic / 站点数 / k1 三个 qword 归零。三者是「这里有一张 vmx64 表」的全部特征字节，清零后文件里不再有可定位的表头；k1 归零同时是「已加密」的标志，构成防重复加密的幂等护栏（见本文 确定性与幂等）。表项与 base 保留——它们本身是 dispatch 运行时要用的数据。

数字例子
^^^^^^^^^^^^^^

（所有值为讲解虚构；真实 ks64 输出无此类规律。）

.. code-block:: text

   首选基址 0x140000000，实际装载基址 0x7FF600000000 → delta = 0x7FF4C0000000
   站点 t=7 的 handler（iadd）首选 VA   H = 0x140001000
   ks64(7, k1)                          K = 0x12345678

   链接期落盘表项 = H - K               = 0x140001000 - 0x12345678 = 0x12DCBB988
   第 1 步改写    = 落盘值 XOR K        = 0x12DCBB988 ^ 0x12345678 = 0x13FFFEFF0
   第 2 步        = 该表项在 .reloc 里的 DIR64 项 type 10 → 0
   第 3 步        = magic / 站点数 / k1 归零

.. _vm-site-table-encryption-form-branch:

运行时形态分支
--------------------

NEXT 宏（每条 handler 末尾的 dispatch 序列）用同一条指令序列服务两种落盘形态，分支条件是 r12：

.. code-block:: text

   ; 入口 prologue 算定一次：
   lea  r12, [base]        ; base qword 地址
   mov  r12, [r12]         ; r12 = base 落盘值
   sub  r12, rdi           ; rdi = 表运行时地址 → r12 = base落盘值 - 表运行时地址

   ; NEXT 的取址尾：
   mov  rdx, [rdi+rax*8]   ; 表项
   test r12, r12
   jz   @F                 ; r12 = 0 → 明文形态，跳过 xor
   xor  rdx, r11           ; 加密形态：摘掉掩码（r11 = ks64(t)）
   @@:
   add  rdx, r11           ; 补回 ks64(t)
   sub  rdx, r12           ; 补回镜像基差
   jmp  rdx

两条路径的算术（续上面的数字例子，K = 0x12345678）：

.. code-block:: text

   未加密形态（r12 = 0）：
     装载器已把表项加过 delta：
       rdx = 0x12DCBB988 + 0x7FF4C0000000 = 0x7FF5EDCBB988
     跳过 xor
     add rdx, r11  = 0x7FF5EDCBB988 + 0x12345678 = 0x7FF600001000   ← handler 运行时 VA
     sub rdx, 0    = 不变

   加密形态（r12 = -delta，u64 回绕 = 0xFFFF800B40000000）：
     表项未被装载器碰过：rdx = 0x13FFFEFF0（密文）
     xor rdx, r11  = 0x13FFFEFF0 ^ 0x12345678 = 0x12DCBB988         ← 链接期落盘值
     add rdx, r11  = 0x12DCBB988 + 0x12345678 = 0x140001000         ← handler 首选 VA
     sub rdx, r12  = 0x140001000 + 0x7FF4C0000000 = 0x7FF600001000  ← 同一目标

为什么用 base qword 当支点：它是表对自己的引用，落盘值恒等于表首选 VA，加密与否不改变。装载器改写它（未加密）→ r12 = 表运行时地址 - 表运行时地址 = 0；装载器跳过它（加密）→ r12 = 首选 VA - 运行时地址 = -delta。一个 qword 同时表达「形态」与「基差」，handler 里不需要任何额外标记位。

为什么分支只压在表项 xor 上：未加密表项经重定位已含基差，再 xor 即破坏；加密表项未经重定位，必须先 xor。静态发射的汇编无法同时服务两种落盘形态，差异被压缩到这一条条件 xor 里，两态汇合同一条 add/sub/jmp 收尾。

fail-closed 拒绝清单
--------------------------

加密器的立场：任何一步校验不过，整个操作作废、原文件一字不动——宁可拒绝交付，不产出半加密产物（半加密 = 表项改了一半或重定位熄灭一半，运行时静默跳飞，比不加密更糟）。

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 检查
     - 拒绝的坏产物 / 误操作
   * - magic 扫描零命中
     - 重复加密（第 3 步已清零定位符）；或输入不是 vmx64 产物（输错文件）
   * - 站点数为 0 或超出 u16 域
     - magic 碰撞（任意数据里恰好出现 ``VMX64TBL``\ ）；或表被改写——发射器只产 1..0xFFFF 个站点
   * - 整表越过宿主段原始数据
     - magic 碰撞或截断产物；同时保证后续读入不越界
   * - k1 为 0
     - 已加密产物（k1 已清零）或损坏产物；若放行，掩码退化为固定公开常数 ``ks64(t, 0)``\ ，产出任何人都能解开的伪加密
   * - 期望的 DIR64 项缺失 / 同目标非 DIR64 项
     - 产物不是预期链接形态（重定位被剥掉、表被改）。熄灭一半留一半 = 装载器改写部分密文，dispatch 静默走错
   * - ``.reloc``\ 块畸形（块头不足 8 字节 / 越段）
     - ``.reloc``\ 损坏；遍历会走出块边界
   * - 非 AMD64 PE / 无 ``.reloc``\ 段
     - 输错文件——加密器的全部字段偏移按 AMD64 PE32+ 布局取，vmx64 产物恒为可重定位 PE

确定性与幂等
------------------

加密无随机量：同一产物 → 逐字节相同的加密结果。幂等的含义不是「二次加密无害」而是「二次加密被拒」：第 3 步清零定位符后，再次加密时 magic 扫描零命中 → fail-closed 拒绝，不会出现把密文再 XOR 一遍的双倍加密产物。

集成点
------------

单一消费点：CLI ``--compile``\ 且后端为 vmx64 时，在 ``buildXmakeProject`` 产出 DLL 之后、嵌入/交付之前调用。``--generate-xmake``\ 只生成工程不编译，不经此步——未加密产物本身可运行（装载器改写 base → r12 = 0 → 明文 dispatch），因此两种工作流的产物都自洽。

回写是原子替换：先写 ``<产物>.mj2x-atomic-<token>``\ 临时文件，全部成功后 rename 回原路径；进程中途死掉，磁盘上要么是原产物要么是完整加密产物。

后续嵌入流水线（可选）：``--native-lib-pack-hook``\ 外部加壳 → ``--native-lib-key``\ 整库加密 → 嵌入 JAR，都在本项之后执行，互不干扰（见 :ref:`native-lib-encryption`\ ）。

已知局限
------------

**delta = 0 简并**\ ：加密产物恰按首选基址装载（ASLR 关闭 / 固定基址部署）时，delta = 0，加密态的 r12 也是 0——形态分支误走 add-only 路径，把密文当明文表项加出垃圾地址，``jmp``\ 跳飞。这是静默错误，不是 fail-closed。 ASLR 常开（Windows DLL 默认动态基址）使其实际不可达；固定基址部署场景必须禁用加密或让镜像避开首选基址。

测试映射
------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 测试
     - 锁定的契约
   * - ``Vmx64PostlinkTest.EncryptsTableAndQuenchesRelocations``
     - 合成最小 PE 上三步改写逐项核对（表项异或值 / DIR64 type 翻 0 / 痕迹清零）+ 确定性重放 + 二次加密拒绝
   * - ``Vmx64PostlinkTest.FailClosedOnMalformedInputs``
     - 全部 fail-closed 面（非 PE / 非 AMD64 / 重定位缺失 / 计数越界 / k1 为 0 / ``.reloc``\ 畸形），且被拒输入原文件一字不动
   * - ``Vmx64StreamInterpTest.Ks64AsmMacroReplaysCppSide``
     - NEXT 宏文本与 C++ ``vmx64Ks64``\ 的单源契约：宏体 19 行指令序列逐行比对 + 公式数值重放逐位相等
   * - ``Vmx64StreamInterpTest.DataTableCarriesEncryptorContract``
     - 数据表布局契约：表头/表尾 qword 的存在与相对顺序、base 自引用、 prologue 的 r12 算定序列——布局漂移即加密器失明

与其他技术的关系
------------------------

- 与 :ref:`native-lib-encryption`\ 层次不同、分工互补：那是整库 blob 的 XOR 流加密（嵌入 JAR 前对整个 ``.dll``\ / ``.so``\ 文件加密、加载时整体解密），本项是 DLL 内部站点表按 qword 的地址掩码改写（产物仍是合法 PE，装载器可直接加载，运行时边执行边解）。叠加顺序：本项先做，整库加密后做。
- 与 :ref:`vm-virtualization`\ 同属 VM 族保护，但对象是 VMC4；vmx64 是独立的 MASM 后端，本项是其产物加固面。
