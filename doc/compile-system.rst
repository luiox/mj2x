.. _compile-system:

编译系统技术说明
================

状态：定稿

native 编译有三条路线（本仓采用的是 xmake 路线，zig / cmake 路线为可选形态）：

#. 原始编译器调用，自行拼接命令行参数；
#. xmake 直接编译生成的项目源码；
#. xmake 混合自定义构建系统。

.. _compile-system-pipe:

pipe 模式（不落源文件）
-----------------------

``--compile-mode pipe``\ 直接消费 backend 保留的内存 C 源码。GCC/Clang 使用 ``-x c -``\ ，Zig 使用 ``zig cc -x c -``\ 从 stdin 读取，编译器只产出 object/shared library，生成的业务 C 源码不会写入文件系统。

MSVC 的两条严格不写盘的路径均不可用：``cl /TC -``\ 会报缺少源文件， ``cl /TC \\.\pipe\<token>``\ 会被 ``c1``\ 以普通文件检查拒绝。因此 MSVC 使用 temporary-file fallback：在输出目录创建唯一临时 ``.c``\ ，通过参数数组直接启动 ``cl.exe``\ ，并在成功、编译失败和启动失败路径清理。该模式保证 CLI 返回后没有 ``.c``\ 残留，但不属于严格 no-write；有此要求时应选择 Clang。MSVC 环境未初始化时，CLI 通过固定 ``cmd.exe``\ 调用已探测的 ``vcvarsall.bat``\ ，解析环境变量后再直接启动 ``cl.exe``\ ，用户输入路径不拼接进 shell 命令。

.. _compile-system-xmake:

xmake 工程路线
--------------

编译系统演进解决两类问题：一是生成源码规模增长后的可复现构建，二是防止同行逆向分析 mj2x runtime 后构建竞品。工程化分发面越可控，runtime 暴露面越小。

xmake 一键构建属于保留工程文件的路线。``--generate-xmake <dir> --compile``\ 保留可复现工程并立即 configure/build；``--c-compiler xmake --compile``\ 使用输出目录下的唯一临时工程并在所有正常返回路径清理。构建子进程以工程目录为 cwd，并用 ``-P .`` 隔离父 xmake 上下文；release 目录必须只产生一个 ``mj2x_native``\ 共享库，随后经临时目标原子替换到最终路径。xmake 不与 pipe 模式混用。
