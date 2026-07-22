编译系统演进路线

目前我想到的有三步，我这个是xmake路线，也可以zig路线或者cmake路线。

1. 原始编译器调用，自己拼接命令行参
2. xmake直接编译生成的项目源码
3. xmake混合自定义构建系统

`--compile-mode pipe` 直接消费 backend 保留的内存 C 源码。GCC/Clang 使用 `-x c -`，Zig
使用 `zig cc -x c -` 从
stdin 读取，编译器只产出 object/shared library，生成的业务 C 源码不会写入文件系统。

MSVC 已验证两条严格不落地路径均不可用：`cl /TC -` 会报缺少源文件，`cl /TC
\\.\pipe\<token>` 会被 `c1` 以普通文件检查拒绝。因此 MSVC 使用 temporary-file fallback：
在输出目录创建唯一临时 `.c`，通过参数数组直接启动 `cl.exe`，并在成功、编译失败和启动失败
路径清理。该模式保证 CLI 返回后没有 `.c` 残留，但不属于严格 no-write；有此要求时应选择
Clang。MSVC 环境未初始化时，CLI 通过固定 `cmd.exe` 调用已探测的 `vcvarsall.bat`，解析环境
变量后再直接启动 `cl.exe`，用户输入路径不拼接进 shell 命令。

编译系统演进，一个是解决源码多的问题，另外一个就是解决被同行逆向的问题，用mj2x的这些user，他们逆向分析我的runtime，然后制造我的竞品，也就是同行竞争问题。

xmake 一键构建属于落地工程路线。`--generate-xmake <dir> --compile` 保留可复现工程并立即
configure/build；`--c-compiler xmake --compile` 使用输出目录下的唯一临时工程并在所有正常返回
路径清理。构建子进程以工程目录为 cwd，并用 `-P .` 隔离父 xmake 上下文；release 目录必须只
产生一个 `mj2x_native` 共享库，随后经临时目标原子替换到最终路径。xmake 不与 pipe 模式混用。
