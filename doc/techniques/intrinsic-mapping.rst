.. _intrinsic-mapping:

编译器内联函数（Intrinsic）
=============================

状态：定稿（通用品类综述；**\ 本仓实际识别面以「本仓现状」节为准**\ ，下文表格是业界常用 intrinsic 的品类罗列，不代表本仓已支持）

在编译时将特定的 JDK 方法调用替换为原生代码实现。

.. _intrinsic-mapping-repo:

本仓现状
----------

mj2x 的 intrinsic 识别面是**\ 保护导向的窄场景**\ ，不是完整 JDK 对象模型（登记表在 ``mj2x-core/src/mj2x/backend/regc/regc_intrinsic_registry.cpp``\ 的 ``kEntries``\ ，条目数以该表为准）：

- ``java.lang.Math``\ 纯函数面：``abs`` / ``min`` / ``max`` / ``sin`` / ``cos`` / ``tan`` / ``sqrt`` / ``pow`` / ``floor`` / ``ceil`` / ``round`` / ``random``\ 等直调 mj2x-rt runtime 符号
- ``Integer.parseInt(String)``\ （含字符串字面量参数）
- ``Integer`` / ``Long``\ 位操作族（``bitCount`` / ``rotateLeft`` / ``numberOfLeadingZeros`` / ``reverse``\ 等）与无符号族（``compareUnsigned`` / ``divideUnsigned`` / ``remainderUnsigned`` / ``parseUnsignedInt`` / ``toUnsignedString``\ ）及 ``max`` / ``min``
- ``Float`` / ``Double``\ bits 视图：``floatToIntBits`` / ``floatToRawIntBits`` / ``intBitsToFloat``\ 及 double 同构——非 Raw 版把任意 NaN 位型规范化，Raw 版逐位保留
- ``Character``\ 静态代理族：``toCodePoint`` / ``highSurrogate`` / ``lowSurrogate`` / ``isSurrogatePair`` / ``charCount``\ 等
- ``System.arraycopy``\ 安全子集（primitive 同型数组直拷，其余回退 JNI）
- 非逃逸包装类窄链路：``Integer/Long/.../Character.valueOf``\ 后接对应 ``xxxValue``
- 非逃逸 ``java.util.Random``\ 状态机切片（``new Random(seed)``\ + nextXxx）
- ``StringBuilder`` / ``StringBuffer``\ 状态机切片与 ``String``\ 类方法面（RegC/LLVM）

可抛异常的条目走 ``RegCLocalHelper``\ 形态（经 context 挂 pending 异常，调用后 ``ExceptionCheck``\ 保留）：无符号除法的除零抛 ``ArithmeticException``\ ，``parseUnsignedInt``\ 族抛 ``NumberFormatException``\ ；纯计算条目走 ``RuntimeFunction``\ 形态（发射端豁免调用后异常检查）。

逃逸即 fail-closed：包装类或 ``Random``\ 对象一旦作为返回值、字段/数组元素、未知 Java 调用参数或跨复杂 CFG 边界逃逸，不生成语义不可靠的 jobject。完整判据见 :doc:`/user-guide` §7.1；任意 Java 静态调用点降级为用户 native symbol 直调的自定义面见 :doc:`/user-intrinsic`\ （``--intrinsic-config``\ ）。

下文表格是业界常用品类的综述，不是本仓逐条映射的承诺；识别面随 ``kEntries``\ 迭代、上表未穷举的以登记表为准。识别一个新 intrinsic 的门槛是语义等价判据 + 逃逸边界 + runtime 符号三件套齐备，不做「表格里列了就该有」的推定。

问题
------

JVM 字节码中调用的 JDK 方法（如 ``Math.abs``\ ）在原生编译场景下如果走 JNI 调用，会有不小的性能损失：

.. code-block:: c

   // 通过 JNI 调用 Math.abs
   jclass mathClass = (*env)->FindClass(env, "java/lang/Math");
   jmethodID absId = (*env)->GetStaticMethodID(env, mathClass, "abs", "(I)I");
   jint result = (*env)->CallStaticIntMethod(env, mathClass, absId, value);

   // 如果替换为原生实现：
   int result = value < 0 ? -value : value;

intrinsic 解决这个问题：在编译时识别特定 JDK 方法调用，用原生代码直接替换。

识别方法
----------

方法签名匹配
~~~~~~~~~~~~~~

通过类名 + 方法名 + 描述符三要素识别：

.. code-block:: c

   struct Intrinsic {
       const char* className;
       const char* methodName;
       const char* descriptor;
       IntrinsicGen generator;  // 生成原生代码的函数指针
   };

   Intrinsic intrinsics[] = {
       { "java/lang/Math", "abs",       "(I)I", genMathAbs },
       { "java/lang/Math", "min",       "(II)I", genMathMin },
       { "java/lang/Math", "max",       "(II)I", genMathMax },
       { "java/lang/Math", "sqrt",      "(D)D", genMathSqrt },
       // ...
   };

匹配优先级
~~~~~~~~~~~~

.. code-block:: text

   1. 完全签名匹配（最精确，首选）
   2. 宽泛匹配（如 java/lang/Boolean.valueOf 匹配所有 valueOf 方法）
   3. 通配符匹配（java/lang/Math.* 全部匹配）

交集匹配时，具体签名优先于通配符。

常用 Intrinsic 品类（业界综述）
---------------------------------

数学运算：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - JDK 方法
     - 原生化
   * - ``Math.abs(int/long/float/double)``
     - 位运算或条件选择
   * - ``Math.min/max``
     - 条件移动或比较选择
   * - ``Math.sqrt``
     - ``sqrt()``\ 编译器内联函数或 SSE 指令
   * - ``Math.floor/ceil/rint``
     - 取整运算
   * - ``Math.sin/cos/tan``
     - 查表或近似多项式
   * - ``Math.exp/log/pow``
     - 数学库函数

位运算：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - JDK 方法
     - 原生化
   * - ``Integer.bitCount``
     - ``__builtin_popcount``\ 或手写 SWAR
   * - ``Integer.numberOfLeadingZeros``
     - ``__builtin_clz``
   * - ``Integer.numberOfTrailingZeros``
     - ``__builtin_ctz``
   * - ``Integer.reverse``
     - 位反转实现
   * - ``Long.rotateLeft/rotateRight``
     - 循环移位

包装类：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - JDK 方法
     - 原生化
   * - ``Integer.valueOf(int)``
     - 直接返回 int（不装箱）
   * - ``Integer.intValue()``
     - 直接返回 int（不拆箱）
   * - ``Boolean.valueOf(boolean)``
     - 同上
   * - ``xxx.parseXxx(String)``
     - 手动解析或库函数

字符串（需要了解 ``String``\ / ``StringBuilder``\ 的内部布局——JDK 8 是 ``char[] value``\ ，JDK 9+ 是 ``byte[] value`` + byte coder）：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - JDK 方法
     - 原生化
   * - ``String.length()``
     - 读取字符串内部 ``length``\ 字段
   * - ``String.charAt(i)``
     - 读取内部 ``value[]``\ 数组 + 偏移
   * - ``String.isEmpty()``
     - ``length() == 0``
   * - ``String.equals(Object)``
     - 先比较引用，再比较长度和内容
   * - ``StringBuilder.append``
     - 直接操作内部 char 数组

数组操作：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - JDK 方法
     - 原生化
   * - ``System.arraycopy``
     - ``memcpy`` / ``memmove``
   * - ``Arrays.fill``
     - 循环赋值或 ``memset``
   * - ``Arrays.copyOf``
     - ``malloc`` + ``memcpy``

语义等价证明
--------------

替换为原生实现前，需要证明语义等价：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 条件
     - 检查
   * - 输入范围
     - 原生实现是否覆盖所有合法输入（如 ``Math.sqrt(-1)``\ 返回 NaN）
   * - 异常行为
     - 原生实现是否抛相同的异常（如 ``String.charAt(-1)``\ 抛 ``IndexOutOfBoundsException``\ ）
   * - 副作用
     - 原生实现是否保持零副作用（intrinsic 不应有额外副作用）
   * - 溢出语义
     - 整型溢出行为是否一致（Java 是 wrap-around，C 有符号溢出是 UB）

.. code-block:: c

   bool canIntrinsic(MethodCall* call) {
       Intrinsic* in = matchIntrinsic(call);
       if (!in) return false;

       // 检查 JDK 版本（布局不同，如 String 在 8 和 9+）
       if (requiresJdkVersion(in, currentVersion)) return false;

       // 检查参数范围（可选，某些 intrinsic 有条件限制）
       if (hasRangeConstraint(in) && !checkRange(call)) return false;

       return true;
   }

生成原生代码
--------------

intrinsic 的生成函数获取调用参数，输出原生代码表达式：

.. code-block:: c

   Value* genMathAbs(IntrinsicCall* call, CodeGen& cg) {
       Value* arg = cg.gen(call->arg(0));
       Type* type = arg->type();

       if (type->isInt()) {
           // abs(x) = (x ^ (x >> 31)) - (x >> 31)
           return cg.emit("(%s ^ (%s >> 31)) - (%s >> 31)",
                          arg->cstr(), arg->cstr(), arg->cstr());
       } else if (type->isLong()) {
           // 同理，移位 63
           return cg.emit("(%s ^ (%s >> 63)) - (%s >> 63)",
                          arg->cstr(), arg->cstr(), arg->cstr());
       } else if (type->isFloat()) {
           // fabsf
           return cg.emit("fabsf(%s)", arg->cstr());
       }
       // ...
   }

内联函数条件
~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 条件
     - 内联
   * - 方法体少于 5 条字节码
     - 始终内联
   * - 方法体是热点（调用频次高）
     - 内联
   * - 方法调用参数全部为常量
     - 内联并折叠
   * - 方法有 JNI 开销
     - 内联（消除 JNI 边界）
   * - 方法体大且非热点
     - 保持 JNI 调用
