# morpher-jrt

定位：Java 字节码翻译到 native 语言的运行时（Java Runtime），提供翻译后代码所需的辅助库。

## 当前状态

已有实现：
1. JNI 包装器（jni_wrapper.h、platform.h）。
2. 指令运行时（rt_insn.cpp/hpp）。
3. 内存管理（rt_memory.cpp/hpp）。
4. 字符串混淆（rt_str_obf.cpp/hpp）。

## 构建

- 构建目标：`jni_lib`（动态库 .dll/.so），依赖 JDK JNI 头文件。
- 需要设置 `JAVA_HOME` 环境变量。
- 输出到 `bin/` 目录。

```bash
xmake build jni_lib
```
