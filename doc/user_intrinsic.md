# 用户自定义 intrinsic（issue #818）

三个后端（StackC / RegC / VMC4）统一支持把任意 Java **静态调用点**降级为对
用户提供的 native symbol 的直接调用，替代通用 JNI 派发。适合把热点函数、
加解密原语、反调试检查等换成用户自己的 C 实现，且不需要改 Java 源码。

**instance 扩面（passReceiver）**：声明 `passReceiver: true` 的条目还可以匹配
instance 调用点（INVOKEVIRTUAL / INVOKESPECIAL / INVOKEINTERFACE），把 receiver
作为参数传给用户 C 函数。

## 配置

```json
{
  "intrinsics": [
    {"owner": "F", "name": "m", "desc": "(II)I", "symbol": "my_m",
     "passContext": false, "mayThrow": false},
    {"owner": "F", "name": "im", "desc": "(II)I", "symbol": "my_im",
     "passReceiver": true}
  ],
  "sources": ["path/to/my_m.c"]
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `owner` | 是 | 类内部名（`F` / `com/x/F`，不带 `L...;`） |
| `name` | 是 | 方法名 |
| `desc` | 是 | JVM 方法描述符（`(II)I`，**不含 receiver**） |
| `symbol` | 是 | 用户 native 实现的可链接符号名 |
| `passContext` | 否 | `true` 时用户函数第一参数收 `JNIEnv* env`（三后端一致） |
| `passReceiver` | 否 | `true` 时匹配 instance 调用点，receiver 作为参数传入（见 ABI） |
| `mayThrow` | 否 | `true` 时调用点保留调用后 pending 异常检查（RegC 恒保留） |

命令行：

```bash
mj2x-cli -i App.class -o App_out.class --emit-c native.c \
  --backend stackc --intrinsic-config cfg.json --compile
```

- `--intrinsic-config <file>`：上述 JSON 文件。
- `--intrinsic-src <file>`：追加单个实现源文件（可重复，与 JSON `sources` 叠加）。
- 相对路径按**当前工作目录**解析；建议传绝对路径。

## 匹配契约（fail-closed）

| 调用点 | 条目 passReceiver=false | 条目 passReceiver=true |
|---|---|---|
| INVOKESTATIC | 匹配（原有行为） | **配置校验错误**：三后端均 fail-closed（包含该调用点的方法保留原字节码/编码失败，并输出诊断），静态方法没有 receiver |
| INVOKEVIRTUAL / SPECIAL / INTERFACE | **不匹配**（回通用 JNI 派发，G-1 行为保持） | 匹配 |

- 配置校验错误**不会**静默回退通用派发——错位配置直接暴露（RegC emitter
  Unsupported → 方法 skip；StackC `compileMethod` 预检 fail-closed；VMC4
  encode 失败）。
- instance 调用点匹配时校验 RegIR/SSA 参数个数 = desc 参数数 + 1（receiver）；
  错位判 malformed，整方法 fail-closed。

## ABI 契约

用户函数参数顺序（三后端一致）：

```
[JNIEnv* env,]        <- 仅 passContext=true
[jobject receiver,]   <- 仅 passReceiver=true（instance 调用点）
<desc 参数...>
```

`passReceiver: false` 时用户函数签名与 Java 描述符一一对应：

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

### receiver 形态（passReceiver=true）

receiver 是调用点现成的局部引用，**不复制、不增减引用计数**，以各后端 REF
值表示的原生形态直传——三者在 ABI 位型上同型（jni.h 中 `jobject` 即 `void*`）：

| 后端 | C 表达式 | 类型 |
|---|---|---|
| StackC | `stack[sp - <argCount>].l`（参数区之下第一槽） | `jobject` |
| RegC | receiver 虚拟寄存器（RegIR `args[0]`） | `mj2x_ref`（= `void*`） |
| VMC4 | `regs[recv].ref` | `void*`（= `jobject`） |

用户实现按 `jobject`（或 `mj2x_ref`）声明 receiver 参数即可；如需长期持有必须
`(*env)->NewGlobalRef`。

**JVM 语义保持**：三后端在调用用户 symbol 前都保留 null receiver NPE trap
（receiver 为 NULL 时抛 `NullPointerException`，与通用 JNI 派发一致），NULL
不会静默递给用户实现。

### 示例

`F.m(II)I` 静态 → `my_m`（含 env）：

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

`F.im(II)I` instance + passContext → `my_im`（env + receiver + 参数）：

```c
#include <jni.h>
#include <stdint.h>

int32_t my_im(JNIEnv* env, jobject self, int32_t a, int32_t b) {
    // self 即 Java 侧 this；如需访问字段/调方法走常规 JNI。
    return a * 1000 + b + 1;
}
```

## 语义细节

- **static intrinsic**（passReceiver=false）只匹配 INVOKESTATIC；instance
  intrinsic（passReceiver=true）只匹配 INVOKEVIRTUAL/INVOKESPECIAL/
  INVOKEINTERFACE。同一 owner/name/desc 在 class 里非 static 即 instance，
  配置按实际形态声明。
- **RegC**：`UserIntrinsic` 恒保留调用后 pending 异常检查与 local-ref 帧
  （不做纯计算豁免）；生成 C 头部自动发射 `extern` 声明（passReceiver 条目
  含 receiver 参数：`extern <ret> <symbol>([JNIEnv* env, ]mj2x_ref receiver, ...)`）。
- **StackC**：从 operand stack 弹参直调；receiver 取参数区之下第一槽 `.l`；
  `mayThrow: true` 保留 ExceptionCheck。
- **VMC4**：`VmMethodRef.userSymbol` 参与 callSite 去重；helper 解包寄存器
  直调，instance 站点 receiver 经 `regs[recv].ref` 传入。
- **LLVM 后端不消费用户 intrinsic**（static 与 passReceiver face 均不生效，
  `--backend llvm` 时相关配置被忽略，调用点走通用 JNI 派发）。
- 同 owner/name/desc 的调用点在未匹配任何条目时行为不变（通用 JNI 派发）。

## sources 并入

- `--compile` 直接编译：实现文件自动追加进 cl 编译命令。
- `--generate-xmake`：实现文件复制到工程 `src/mj2x-rt/user_intrinsics/`，
  模板的 glob（`src/mj2x-rt/**.c`）自动并入编译。

## 测试

- `user_intrinsic_test`：三后端发射形态（源码断言）。覆盖：static 降级、
  passContext 参数序、instance+passReceiver 降级（receiver 形态/NPE trap/
  ExceptionCheck）、static+passReceiver 配置校验 fail-closed（三后端各一负例）、
  instance 未声明 passReceiver 不匹配（G-1 回归锁）、RegC extern 声明参数序、
  VMC4 解释器 helper receiver 槽直传。
- `user_intrinsic_e2e_test`：三后端执行矩阵（`my_m` 接管 `F.m`，断言 `2003`）。
  instance/passReceiver 的执行级 E2E 依赖 CLI 侧 `--intrinsic-config` 解析
  `passReceiver` 字段，属 CLI 辖区，另行接入。
