# mj2x 用户指南

> mj2x 把 Java 字节码(`.class` / `.jar`)翻译为 C 源码,并将原方法标记为 `native`。
> 本文档讲怎么用。项目概览见 `../readme.md`;已实现能力清单见 `capabilities.md`。

---

## 1. 构建

```bash
xmake f --with_mj2x=y
xmake build mj2x-cli       # CLI 工具
xmake build mj2x-core      # 核心静态库(被 CLI 依赖)
xmake build mj2x_unittest  # 单元测试
xmake build mj2x_runtime_test  # R3 runtime 测试
```

需要 JDK(`JAVA_HOME` 指向 JDK 安装目录,必须包含 `jni.h`)。

---

## 2. 基本用法

### 2.1 单 class 输入

```bash
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c
```

- `Hello_native.class`:原方法被标记为 `native`
- `Hello.c`:生成的 C 源码

### 2.2 JAR 输入

```bash
mj2x-cli -i myapp.jar -o myapp_native.jar --emit-c myapp.c
```

JAR 内多个 class 会被逐个处理,非 class 资源原样保留,entry 顺序保留。

### 2.3 选择后端

```bash
# CBackend(默认,广覆盖正确性)
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c --backend cbackend

# RegC(保护导向,无栈)
mj2x-cli -i Hello.class -o Hello_native.class --emit-c Hello.c --backend regc
```

后端对比见 [capabilities.md §2 / §3](../doc_ai/capabilities.md)。

### 2.4 调试:查看中间表示

```bash
mj2x-cli -i Hello.class -o /dev/null --dump-mir
```

---

## 3. 参数参考

| 参数 | 说明 |
|---|---|
| `-i, --input <.class/.jar>` | 输入文件 |
| `-o, --output <.class/.jar>` | 输出文件(方法被标记为 native) |
| `--emit-c <file>` | 输出的 C 源码路径 |
| `--backend <cbackend\|regc>` | 选择后端(默认 `cbackend`) |
| `--dump-mir` | 打印中间表示(调试用) |
| `--help` | 打印帮助 |

> `--generate-project <dir>`(生成独立 xmake 工程)当前在分支 `feat/mj2x/xmake-project-generator`,未合入 main。

---

## 4. 编译生成的 C 为 DLL

### 4.1 用 xmake(推荐,JAR / 复杂场景)

手动建工程或用生成器(实验性,见分支)。核心是:把生成的 `.c` 连同 JNI 头编译为 shared library。

### 4.2 手动编译(Windows / cl)

```bash
javac -h . Hello.java
cl /LD /I"%JAVA_HOME%\include" /I"%JAVA_HOME%\include\win32" Hello.c /FeHello.dll
```

### 4.3 手动编译(Linux / gcc)

```bash
javac -h . Hello.java
gcc -shared -fPIC -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" Hello.c -o libnative.so
```

---

## 5. 运行

```bash
# Windows
java -Djava.library.path=. Hello

# Linux
java -Djava.library.path=. Hello
```

---

## 6. 后端选择指南

| 场景 | 推荐后端 |
|---|---|
| 需要最大正确性覆盖、不关心抗分析 | CBackend |
| 保护导向,需要无栈输出抗 c2j | RegC |
| 需要除零 trap 符合 JVM 语义 | RegC + R3 runtime |

后端的详细支持矩阵和限制:
- CBackend:`doc/c_backend_readme.md`
- RegC:`doc/regc_backend_readme.md`

---

## 7. 测试真实混淆 class

```bash
jar xf ref_samples/SnakeGame-obf-grunt.jar b.class
mj2x-cli -i b.class -o b_out.class --emit-c b_out.c
```

> 已知:`SnakeGame-obf-grunt.jar` 中 4/14 的 class 加载时抛 `readInt exceeds buffer`(libjbytecode 边界 bug,非 mj2x 问题)。

---

## 8. 常见问题

**Q: 生成的 DLL 加载报 `UnsatisfiedLinkError`?**
检查:JNI 头是否正确生成(`javac -h`)、DLL 是否在 `java.library.path`、方法签名是否匹配。

**Q: JAR 改写后签名失效?**
当前 MVP 不处理签名。`META-INF/*.SF` / `*.RSA` / `*.DSA` / `*.EC` 会触发 fail-closed。需要先去签名再处理。

**Q: 遇到 `invokedynamic` 报 `UnsupportedOperationException`?**
这是预期行为(fail-closed)。lambda / 方法句柄 / string concat 的 lowering 在 MVP 范围外。

**Q: RegC 输出里有 `cstack` / `sp`?**
不应该有。如果有,是 bug —— RegC 的结构断言禁止 stack-machine anchor。
