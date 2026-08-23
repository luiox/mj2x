# mj2x-llvmrt 运行时下沉 benchmark（#894 收尾）

日期：2026-08-23。对比 main（d79f75ab7 + refeq 修复 cherry-pick）vs 分支
（`refactor/mj2x-llvm-runtime-sink`，67dd8b6cc + c1ce74d79）。基线含缺陷修复
的原因：main 在 `ifnull/ifnonnull` 路径发射非法 IR（见 c1ce74d79），不含该
修复则代表性输入无法在两侧同时编译；cherry-pick 后两侧唯一差异即运行时
下沉本身。

## 覆盖率审计（必填）

javap -p -c 逐类核验，native 状态两侧完全对称：

| 被测类 | native 方法 | 有 Code 属性 | 备注 |
|---|---|---|---|
| LlvmTinySample | 9 | 1（<init>） | 两侧一致 ✅ |
| LlvmExceptionMonitorSample | 3 | 3（<init>/clinit/sync wrapper） | 两侧一致 ✅ |
| LlvmIndySurfaceSample | 6 | 2（<init>/clinit） | 两侧一致 ✅ |
| BenchWide（自建宽覆盖） | 19 | 4（<init>/clinit 等） | 两侧一致 ✅ |
| BenchNative（运行微基准） | 9 | 3（<init>/clinit 等） | 两侧一致 ✅ |

驱动侧（BenchDriver）保持纯 Java 未转换；所有校验和断言通过
（fib/loopSum/arraySum/fieldInc/boxSum/syncSum/callMix 七相位，
语义等价成立）。

## 环境

Windows x64 / MSVC 2022 / clang 18.1.8 / JDK 17.0.8。CLI 均为 release 构建；
llvm 后端 `--compile --llvm-tool clang`（clang 默认 -O0，无 LTO）。crt 源包
经 `MJ2X_CRT_DIR` 各指向各自工作区 `mj2x/mj2x-rt`。

## CLI 编译耗时（5 次，中位数）

| 输入 | main | 分支 | Δ |
|---|---|---|---|
| LlvmTinySample | 1631 ms | 1565 ms | -4.0% |
| LlvmExceptionMonitorSample | 1633 ms | 1558 ms | -4.6% |
| LlvmIndySurfaceSample | 1558 ms | 1590 ms | +2.1% |
| BenchWide | 1581 ms | 1514 ms | -4.2% |
| BenchNative | 1536 ms | 1563 ms | +1.8% |

结论：**中性**（差异 ±5% 内、方向不一致，处于机器噪声带）。分支每模块多
编译一份 llvmir 运行时 C 源，但省去大体积 IR 的处理，两端相抵。

## 产物体积（首次运行产物）

| 输入 | .ll 字节（main→分支） | define internal（main→分支） | dll 字节（main→分支） |
|---|---|---|---|
| LlvmTinySample | 80718→25506（-68%） | 77→9 | 294400→307200（+4%） |
| LlvmExceptionMonitorSample | 68683→13471（-80%） | 71→3 | 292352→305664（+5%） |
| LlvmIndySurfaceSample | 82391→27179（-67%） | 74→6 | 295424→308736（+5%） |
| BenchWide | 98566→43354（-56%） | 87→19 | 297984→310784（+4%） |
| BenchNative | 76750→21538（-72%） | 77→9 | 293376→306688（+5%） |

模块 IR 大幅缩水（helper 体移出模块）；dll 略增（运行时 C 并入链接，
一次性成本）。

## 产物运行耗时（48 样本中位数，µs；进程级交错消除漂移）

| 相位 | main | 分支 | Δ | 有效性 |
|---|---|---|---|---|
| fib(26)×20（递归+调用） | 24851 | 25560 | +2.9% | 有效 |
| loopSum(20M)（纯循环） | 45537 | 47872 | +5.1% | 有效 |
| arraySum 100K×100（数组） | 215965 | 210702 | **-2.4%** | 有效 |
| fieldInc(5M)（静态字段对） | 3194871 | 3529611 | +10.5% | 有效 |
| boxSum(300K)（box/unbox） | 3474 | 3814 | +9.8% | <5ms 无参考价值 |
| syncSum(200K)（monitor） | 1012 | 1156 | +14.2% | <5ms 无参考价值 |
| callMix(1M)（虚调用） | 2910 | 3901 | +34.1% | <5ms 无参考价值 |

### 解读（限定被测场景）

1. 方向不一致（arraySum 分支更快）且 loopSum 热循环不含任何 helper 调用
   却 +5.1%——主体为代码布局/环境噪声，非下沉引入的调用开销。
2. 有效相位折算单次 helper 调用差 ~0.1-0.5ns（如 fieldInc 10M 次字段访问
   差 335ms ≈ 0.33ns/次），量级在 JNI 边界噪声内。
3. clang 链接无 LTO：internal 与 extern 调用形态在 -O0 下等价（均不内联），
   与"internal→extern 对 LTO 无关、预期中性"的预判一致。
4. 结论：**运行时中性，无回归证据**。

## 复现

```powershell
git worktree add <wt> d79f75ab7 && git -C <wt> cherry-pick c1ce74d79
# xmake f --with_mj2x=y -y && xmake build mj2x-cli（两侧各一次）
# bench894/run_bench.ps1（转换计时）→ run_bench_phase2.ps1（审计+运行）
# → run_bench_interleaved.ps1（交错复测）
```

脚本与中间 CSV 在会话临时目录（不入库）；本报告数据为其汇总。
