# 用户自定义 intrinsic（issue #818）

三个后端（StackC / RegC / VMC4）统一支持把任意 Java **静态调用点**降级为对
用户提供的 native symbol 的直接调用，替代通用 JNI 派发。适合把热点函数、
加解密原语、反调试检查等换成用户自己的 C 实现，且不需要改 Java 源码。

## 配置

```json
{
  "intrinsics": [
    {"owner": "F", "name": "m", "desc": "(II)I", "symbol": "my_m",
     "passContext": false, "mayThrow": false}
  ],
  "sources": ["path/to/my_m.c"]
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `owner` | 是 | 类内部名（`F` / `com/x/F`，不带 `L...;`） |
| `name` | 是 | 方法名 |
| `desc` | 是 | JVM 方法描述符（`(II)I`） |
| `symbol` | 是 | 用户 native 实现的可链接符号名 |
| `passContext` | 否 | `true` 时用户函数第一参数收 `JNIEnv* env`（三后端一致） |
| `mayThrow` | 否 | `true` 时调用点保留调用后 pending 异常检查（RegC 恒保留） |

命令行：

```bash
mj2x-cli -i App.class -o App_out.class --emit-c native.c \
  --backend stackc --intrinsic-config cfg.json --compile
```

- `--intrinsic-config <file>`：上述 JSON 文件。
- `--intrinsic-src <file>`：追加单个实现源文件（可重复，与 JSON `sources` 叠加）。
- 相对路径按**当前工作目录**解析；建议传绝对路径。

## ABI 契约

`passContext: false` 时用户函数签名与 Java 描述符一一对应：

| Java 类型 | C 类型 |
|---|---|
| `void` | `void` |
| `int`/`boolean`/`byte`/`short`/`char` | `int32_t` |
| `long` | `int64_t` |
| `float` | `float` |
| `double` | `double` |
| 引用 | `jobject`（含 `jstring` 等具体类型，按 desc 收参） |

`passContext: true` 时第一参数为 `JNIEnv* env`，其后才是 Java 参数。
用户实现内可直接用 `env` 调 JNI（抛异常等），但**不要**在 Java 参数之外
假定任何其他上下文。

示例（`F.m(II)I` → `my_m`，含 env）：

```c
#include <jni.h>
#include <stdint.h>

JNIEXPORT int32_t JNICALL my_m(JNIEnv* env, int32_t a, int32_t b) {
    if (b == 0) {
        jclass ex = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
        if (ex != NULL) (*env)->ThrowNew(env, ex, "b is zero");
        return 0;   // 返回值未定义，调用点会检查 pending exception
    }
    return a * 1000 + b;
}
```

## 语义细节

- **只匹配静态调用**（INVOKESTATIC）；instance 调用点不降级。
- **RegC**：`UserIntrinsic` 恒保留调用后 pending 异常检查与 local-ref 帧
  （不做纯计算豁免）；生成 C 头部自动发射 `extern` 声明。
- **StackC**：从 operand stack 弹参直调；`mayThrow: true` 保留 ExceptionCheck。
- **VMC4**：`VmMethodRef.userSymbol` 参与 callSite 去重；helper 解包寄存器直调。
- 同 owner/name/desc 的调用点在未匹配任何条目时行为不变（通用 JNI 派发）。

## sources 并入

- `--compile` 直接编译：实现文件自动追加进 cl 编译命令。
- `--generate-xmake`：实现文件复制到工程 `src/mj2x-rt/user_intrinsics/`，
  模板的 glob（`src/mj2x-rt/**.c`）自动并入编译。

## 测试

- `user_intrinsic_test`：三后端发射形态（9 例，源码断言）。
- `user_intrinsic_e2e_test`：三后端执行矩阵（`my_m` 接管 `F.m`，断言 `2003`）。
