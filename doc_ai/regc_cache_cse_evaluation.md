# RegC JNI Cache CSE 评估

## 结论

RegC 保留并持续回归编译期已知 owner、method、field 与 class descriptor 的 TU 级
cache CSE；不为 `_mj2x_regc_find_class` 的动态 descriptor 或 runtime `class_name`
回调增加全局缓存。

动态 cache 没有可证明的正确性收益。已发射的 `NEW`、引用数组、多维数组、
`CHECKCAST`、`INSTANCEOF` 以及静态/实例 field 和 method 调用都在编译期拥有
descriptor，已由 `RegCCachePool` 生成稳定的 cache slot。剩余的通用 helper 是
standalone emitter、异常路径和未知运行时输入的 fail-safe fallback，而不是正常 RegC
backend 的热路径。

以 descriptor 字符串为 key 的 TU 静态缓存会遗漏 class-loader identity。JNI
`FindClass` 的结果取决于当前 native 调用的关联加载器；全局强引用既可能把另一个
loader 的同名类复用到当前调用，也会阻止类及其 loader 卸载。把它改为 weak global
cache 仍需要 loader identity、并发初始化、失效重试和 pending exception 处理，不能
作为不改变语义的微优化引入。`class_name` 回调同样是动态错误路径参数，缺少稳定
loader key，也没有可验证的热路径收益。

未来若要重新评估，必须同时具备：owner loader identity、`jweak` 生命周期、线程安全
初始化、pending exception 语义测试，以及命中率和 native E2E 的正收益证据。

## 可复跑基准

样本固定为
`mj2x/test_resource/protected_samples/regc/RegCIntrinsicBenchmarkSample.java`。先生成
结构指标：

```powershell
xmake run mj2x_unittest -- --gtest_filter=RegCTraceSamples.GenerateMjdbgFixtures
Get-Content test/mj2x/protected_samples/regc/RegCIntrinsicBenchmarkSample/REGC_METRICS.md
```

在 Windows、JDK 17、Clang 18.1.8、`-std=c99 -O2 -c` 下，连续三次编译生成 C 到
object 的结果为 283.8 ms、236.2 ms、259.4 ms，中位数为 259.4 ms。该数字包含 clang
进程启动，适合作为同机回归比较，不应用于跨机器绝对性能结论。

| 指标 | 当前值 |
|---|---:|
| generated C bytes | 99,478 |
| `_mj2x_regc_call_` symbol occurrences | 36 |
| TU class cache declarations | 8 |
| TU static method cache declarations | 4 |
| TU instance method cache declarations | 1 |
| TU static field cache declarations | 0 |
| TU instance field cache declarations | 0 |

`_mj2x_regc_call_` 计数包含 runtime helper 的声明和定义，用于监测生成 C 的 JNI helper
surface，不等同于动态调用次数。样本生成测试还对 intrinsic、multi-array cached path 和
无 stack anchor 的结构约束做断言。

## 验证

- `RegCBackendTest.InternsCachesAcrossGeneratedTranslationUnit`：跨方法的
  method/field/class cache declaration 去重与 cached allocation path。
- `RegCTraceSamples.GenerateMjdbgFixtures`：固定样本的 C 体积、cache declaration 和
  trace surface 指标。
- `RegCBackendE2E.IntrinsicMathAndParseIntRoundTrip`：含 class cache 与 intrinsic 的
  native DLL/JVM baseline 对照。
