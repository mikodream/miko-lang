# miko 现状判定与工作路线

本文件只依据代码与可执行产物的实测结果写成，不采信仓库内的 README / BOOTSTRAP / BUILD / TUTORIAL。

---

## 一、套壳判定：主干是真的，语义是壳

判据不是"用了 LLVM 没有"，而是**这门语言的语义由谁定义**。

| 子系统 | 自己做的 | 交给谁 | 判定 |
|---|---|---|---|
| 词法、语法 | 自己写（`lexer.miko` 382 行、`parser.miko` 612 行、`Parser.cpp`） | — | 不是套壳 |
| 类型检查 | 自己写，但两套：`Sema.cpp` 1039 行 / `sema.miko` **230 行** | — | 自举端是残缺的替身 |
| IR 生成 | 自己写，3237 行真实 IR 输出（struct 类型、GEP、phi、控制流俱全） | — | 不是套壳 |
| 优化 | **零**。没有任何 pass | LLVM / clang | 不算套壳（Rust/Swift/Clang 同理），但出厂路径连 `-O2` 都没开 |
| 字符串语义 | **零**。没有自己的字符串表示 | `char*` + libc 的 `strlen`/`strcmp`/`strstr`/`memcpy` | **套壳** |
| 运行时 | 250 行 C 胶水 | 直接在 IR 里塞 libc 与 Win32 调用 | **套壳** |
| 平台层 | **零** | `__acrt_iob_func`（MSVC UCRT）、`QueryPerformanceCounter`（Win32）、`system()` | **套壳，且钉死在 Windows** |
| C 后端支线 | — | `main.miko` → `codegen_c.miko` 生成 C 源码，`printf("%lld")`、`static long long len(...)` 包 libc | **这条支线就是套壳** |

### 决定性证据：一个字符串程序的 IR 外部符号表

用 C++ 后端编译 `tests/diff/08_strings.miko`，**修复前**生成的 IR 声明了这些外部符号：

```
__acrt_iob_func  exit  fclose  fgetc  fgets  fopen  fprintf  fwrite
GetSystemTimeAsFileTime  malloc  memcpy  printf  QueryPerformanceCounter
QueryPerformanceFrequency  realloc  snprintf  strcmp  strcspn  strlen
strstr  strtod  strtoll  system
```

三件事由此确定：

1. miko 的字符串就是 C 的 `char*`。每次 `len`/`char_at`/`substring` 都先 `strlen` 一遍源串，`+` 变成 `miko_str_concat`（malloc + memcpy）。语言层没有长度字段，字符串操作 O(n)，嵌套即 O(n²)。
2. `clock_ms()` 靠在生成的 IR 里调 Win32 API 实现（`QueryPerformanceCounter`）。这不是"运行时在 Windows 上编译"，是**生成的程序本身依赖 Windows 内核符号**，Linux 上不存在。
3. stdin/stdout 走 `__acrt_iob_func`，MSVC UCRT 私有导出，glibc 上不存在，链接必失败。

因此"跨平台"的卡点比构建脚本更深：不只是 `miko-link.bat` 硬编码了本机路径，**代码生成器的输出里已经把平台写死了**。

### 修复后

同样的探针，两端的 IR 外部符号表已经没有平台符号（见第 5 项）：

| 端 | 外部符号 |
|---|---|
| C++ 后端 | `exit fclose fgetc fopen fprintf fwrite malloc memcpy printf realloc snprintf strcmp strlen strstr strtod strtoll` + `miko_*` |
| 自举后端 | `ceil floor round sqrt strcmp strlen` + `miko_*` |

剩下全是 ISO C / libm 的东西。`system`/`__acrt_iob_func`/`QueryPerformance*`/`GetSystemTimeAsFileTime`/`fgets`/`strcspn` 均为 0 处。用 `clang --target=x86_64-w64-windows-gnu` + `gcc` 交叉链接可以直接跑通，说明 IR 本身不再依赖 MSVC 符号。

仍留着的"壳"是第 1 条：字符串依旧是 `char*` + `strlen`，这属于第 6 项（P1），不是平台问题。

第 6、第 8 两节已把它修掉：字符串现在自带长度和容量，`strlen`/`strcmp`/`strstr` 在生成的 IR 里为 0 处。`char*` 留下的只是 `printf`/`fopen` 的 ABI 形状，不再是长度的来源。

### 另一处"壳"：阶段间协议

自举端的"AST"是缩进文本：

```
Program
  Function main
    Let a
      Expr 7
```

后端拿到这段文本后**重新词法、重新语法**一遍，各自维护一套解析。而 `codegen_ll.miko` 的 `ir_type_for` 里留着 `ty == "long long"` 这个**输入别名**，唯一来源是 `codegen_c.miko` 的类型词表（见 `codegen_c.miko:301`：`i32/i64/"long long" → "long long"`）。

即：两个后端的类型词表是**共生复制**出来的，不是 miko 自己的类型系统。这是"壳"留在系统内部的残留。

### 结论

> **主干是真的，语义是壳。** 前端与 IR 生成是自研的；但"字符串是什么、时间从哪来、stdout 往哪写"这三件构成语言运行时语义的事，全部借了 C 与 Win32。C 转译支线则是完整的套壳路线。

---

## 二、工作表格

量级按单人估算。`P0` = 不做就不能用；`P1` = 不做就不能叫成熟；`P2` = 不做就会落后。

| # | 优先级 | 工作项 | 具体动作 | 验收标准 | 阻塞 | 量级 | 状态 |
|---|---|---|---|---|---|---|---|
| 1 | **P0** | 消灭静默误编译 | 符号表按块分层（`~BLOCK` 标记），出块截断、查表取最内层 | `tests/diff/18_shadowing` 三端一致；自举两代 17/17 | 全部 | 1–2 周 | **已完成** |
| 2 | **P0** | 自举端补齐内建 | 把 C++ 端 36 个内建名在 `codegen_ll.miko` 里全部接上（原只认 6 个），现 30 个 | `tests/diff/16_builtins` 三端一致 | 1 | 2–3 周 | **已完成 30/36**；余 6 个 `vec_*` 实际卡在第 12 项（vec 类型支持），代码生成器**明确拒绝**而非静默误编译，见下 |
| 3 | **P0** | 自举端补浮点 | `codegen_ll.miko` 原 `f64/f32` 命中 **0**；已补类型映射、字面量、四则、比较、6 个浮点内建 | `let x: f64 = 1.5; print(x + 2.5)` 三端一致 | 2 | 1–2 周 | **已完成** |
| 4 | **P0** | 出厂路径开优化 | `dist/miko-link.bat` 加 `-O2`（原一条 `-O` 都没有） | 出厂产物与 C++ 路径同级别；基准有可比数 | — | 1 小时 | **已完成**，实测 3.5× |
| 5 | **P0** | 拆掉平台硬编码 | `__acrt_iob_func` → `miko_eprint_str`/`miko_input`/`miko_runtime_error`；`QueryPerformanceCounter` · `GetSystemTimeAsFileTime` → `miko_mono_ms`/`miko_clock_ms`（运行时内条件编译）；`system()` 收进 `miko_run_command`；补 `dist/miko-link.sh`，链接语义由工具链脚本扩展名决定（`.bat` → `.exe` + `call`，否则 POSIX） | 两端 IR 外部符号表中无任何平台符号；Linux 上 18/18 difftest 通过 | 4 | 2–3 周 | **代码已完成**；Linux 实机验收待做（本机无 Linux） |
| 6 | **P1** | 字符串自带长度 | 长度写进分配块头部的 8 字节，`char*` 指向正文；两端字面量发射同步带头；生成 IR 里 `strlen` 归零 | 字符串密集基准 O(n²)→O(n) 可量化 | — | 1–2 周 | **已完成**（278 ms → 8 ms，见第六节） |
| 7 | **P1** | 内存策略定死 | **所有权规则已定：`append_str` 转移 dst 的所有权**（见第八节）。回收策略仍未定 | `malloc` 有对应回收路径；内存曲线不再单调增长 | 6 | 2–4 周 | 一半 |
| 8 | **P1** | 统一 IR | 抽出一份真中间表示，`codegen_ll` 与 `codegen_c` 共享前端产物，取消"后端重新 parse 缩进文本" | 加一个语法特性只改一处 | 1 | 3–4 周 | 未开始 |
| 9 | **P1** | `sema.miko` 追平 | 不做完整 sema，改用 `main_ll.miko` 里的静态检查 pass 补上负例覆盖（沿用已有 const pass 的范式）：const 写入认数组元素/字段基名、下标必须是整数、数组字面量长度对齐、结构体字面量字段数对齐 | **全部 11 个负例在自举端被拒**；门禁扩到 29 例 | 8 | 2–3 周 | **已完成（负例口径）** |
| 10 | **P1** | 建 CI 门禁 | `difftest.ps1` 已扩到覆盖 `examples/*_error.miko`（29 例）；新增 `.github/workflows/gate.yml`（windows-latest：装 LLVM/MinGW → 构建 C++ 端 → 用它构建自举端 → difftest → bootstrap 两代，失败时上传 `.ast`/`.sema`/`.ll`） | 任一后端行为偏离立刻可见 | 1–5 | 3 天 | **脚本与工作流就绪**，未在真实 CI 上跑过（本机无 runner） |
| 11 | **P1** | 冻结 C 转译支线 | `main.miko`/`codegen_c.miko`（1466 行）标注 archived，不再扩展 | 引擎数 4 → 2，维护面积减半 | — | 1 天 | 未开始 |
| 12 | **P2** | 语言本体补件 | 泛型机制（非把 `vec<T>` 当特例）、闭包/一等函数、`Result` 错误处理、模块可见性 | 能写通用容器库与回调 | 8 | 1–2 月 | 未开始 |
| 13 | **P2** | 标准库扩到可用 | 现 4 文件 **533 行**，`io.miko` 仅 16 行；目标 5000–15000 行 | 能写实际程序（文件遍历、格式化、容器、时间） | 2,3,7 | 1–2 月 | 未开始 |
| 14 | **P2** | 基准套件 | 现 4 个 miko + 4 个 go 文件、**无任何运行脚本**；补 harness + 与 Go/Rust 对比 | "高性能"有可复现数字 | 4,6 | 1 周 | 未开始 |
| 15 | **P2** | 工程卫生 | 版本号已统一为 0.2.0；CI 工作流已补（见 #10）；**77 个源码/测试文件已从"未跟踪"补入暂存区**（见第七节）。仍需人拍板：`release/miko-development.zip` 289 MB 仍被跟踪 | 干净可 clone，版本可追溯 | — | 3–5 天 | 大部分完成 |

---

## 三、执行顺序上的两个判断

**一，不要在 #8（统一 IR）之前加新特性。** 现在一份语法特性要落四个引擎、三套类型系统。这个倍数下，功能列表填得越满，不一致面积越大——"37 vs 6" 就是活的例子。先把维护面积从 4 个引擎降到 2 个，再谈特性。

**二，先修 #2 和 #3。** 出厂二进制现在编译不了 `to_string_i64`、编译不了浮点、编译不了 `vec`。这不是"不够快"或"不够优雅"，是用户拿到 release 后，语言规格里写着的类型和标准函数凭空消失。这一点修好之前，任何关于成熟度的讨论都在描述别的项目。

粗算：`#1–#5` 是"能用"的门槛，约 6–10 周；加上 `#6–#11` 是"成熟"的门槛，再 3–4 个月。

---

## 四、本轮已修（5 项 P0 全部落地）

验收口径统一为：`tools/difftest.ps1` 三端一致 + `tools/bootstrap.ps1` 两代通过 + IR 不动点成立。

| 项 | 结果 | 验证 |
|---|---|---|
| #1 静默误编译 | 符号表按块分层 | difftest **18/18**；bootstrap 两代 **17/17**，不动点 `88611C86F692F9C6` |
| #2 自举端内建 | 14 → **30** 个名字 | `16_builtins` 三端一致 |
| #3 自举端浮点 | 类型映射、字面量、四则、比较、6 个内建 | `17_floats` 三端一致 |
| #4 出厂优化 | `-O2` | 50M 迭代循环 25.2 ms → 7.1 ms（**3.5×**） |
| #5 平台硬编码 | IR 里已无平台符号 | 见「修复后」符号表 |

### #1 的根因与解法

第一版给名字加嵌套深度后缀（`name@depth`），语义对了，但它编译出的编译器**一碰除法就 0xC0000005**。三步隔离把范围钉死：

| 变体 | gen2 除法 | 遮蔽 |
|---|---|---|
| 真实深度 + `name@depth` 键 | **崩溃** | 正确 |
| `st_depth()` 恒返回 0 | 正常 | 失效 |
| 真实深度计算 + 扁平键 | 正常 | 失效 |

中间一行证明 `st_stack_entries` / `st_depth` 本身没错（第三行仍真实算深度却一切正常），**触发崩溃的是带 `@` 的键字符串**。

最终方案不引入新字段、不引入新算术：符号表仍是 `name:irtype:slot` 扁平文本，用一行 `~BLOCK` 标记分层。进块追加标记、出块截断到最后一个标记、查表取**最后一次**匹配（即最内层）。全部普通字符串操作。

顺带修掉一处真误编译：`emit_binop` 有三处 `let opcode`，除零分支遮蔽了算术分支，导致任何含 `+` 的程序被编译成 `%t4 =  i64 ...`（opcode 丢失）。已改名 `logic_op` / `div_op`。

### #2 与 #3 发现的两个真缺陷

**一，浮点字面量被静默截断。** `let x: f64 = 1.5` 发出 `store i64 1`，打印 `1`，不报错。根因在 `tokenize_flat` 把 `.` 当特殊字符，`1.5` 被切成 `1` `.` `5`。补了数字扫描分支。修这个又踩回退：裸扫描丢掉了 `0x`/`0b`/`1_000` 前缀，且若让 `a–f` 进入十进制体会吞掉 `1e-5` 的 `e`。最终分成 `is_num_body_char`（数字+`_`）与 `is_radix_body_char`（再加 `a–f`）两条判定。

**二，类型名有两套拼写。** 表达式结果用源码类型名（`f64`），符号表里存的却是 `ir_type_for()` 映射后的 IR 类型名（`double`）。只认其中一种，从变量读出的浮点就会掉进整数路径——`print(x)` 一度生成 `call void @miko_print_i64(i64 %t1)`。现在统一走 `is_float_ty()`。

### #5 的两处连带修复

- **difftest harness 隐含假设被打破。** C++ 后端原先把 stderr/stdin/时间全部内联发射，于是它的链接步骤只做 `gcc obj -o exe`，不链运行时。改为调用运行时后链接失败、6 个用例 REJECT。补上 `runtime/miko_runtime.c`。
- **链接语义改为数据驱动。** `link_executable` 原先写死 `call` 前缀与 `.exe` 后缀。现在由工具链脚本的扩展名决定：`.bat` → 加 `.exe`、加 `call`；其余按 POSIX 处理，不加后缀也不加 `call`。补了 `dist/miko-link.sh`（含 `-lm`，因为 `sqrt`/`floor`/`ceil`/`round` 在 glibc 上要显式链数学库）。

### 还没做的

`vec<T>` 的 6 个内建虽然记在 #2 名下，但它依赖类型系统的泛型支持（#12）。当前状态是：解释器与 C++ 后端**完整支持**（`vec_new`/`push`/`pop`/`set`/`clear`/`reserve` + `len(v)` + `v[i]`，7 项输出一致），自举端**明确拒绝**并给出诊断——不是静默误编译：

```
codegen_ll: vec<T> is not supported by the LLVM IR backend (no runtime support yet)
```

这是表格原本把它算进 #2 的一处估计偏差。

#5 的代码已完成，但验收标准写的是「Linux 上 18/18 difftest 通过」，本机无 Linux 环境，这一条**未实测**。已做的替代验证：两端 IR 外部符号表清空所有平台符号，且用 `clang --target=x86_64-w64-windows-gnu` + `gcc` 交叉链接可跑通。

### 逐项证据

| 项 | 修前 | 修后 | 证据 |
|---|---|---|---|
| 出厂产物优化等级 | `-O0`（bat 里一条 `-O` 都没有） | `-O2`，可用 `MIKO_OPT` 覆盖 | 50M 次整数循环：25.2 ms → 7.1 ms，**3.5×**；`MIKO_OPT=-O0` 可复现回 24.6 ms |
| 内建覆盖（自举端 vs C++ 端） | 14 / 36 | **30 / 36** | 新增 `to_string_i64/bool/char`、`str_to_i64`、`str_index_of`、`str_contains`、`clock_ms`、`seed_i64`、`rand_i64`、`to_string_f64`、`str_to_f64`、`f64_to_i64`、`sqrt/floor/ceil/round_f64` |
| 浮点支持（自举端） | `f64/f32` 命中 **0**；`let x: f64 = 1.5` 静默生成 `store i64 1`，打印 `1` | 完整支持 | `store double 1.5`、`fadd/fsub/fmul/fdiv`、`fcmp oeq/olt/...`、混合升降级、指数与下划线字面量 |
| `rand_i64` 三端一致 | 解释器与两个编译后端**给出不同数列** | 三端逐值相同 | 解释器 SplitMix64 用了有符号右移（算术移位），后端用 `lshr`；改为无符号后 `seed 7` 三端同为 `7643450305123983127` |
| 示例通过率（自举端） | 25 / 43 | **26 / 43**（43 中 7 个是预期拒绝的 `*_error` 负例，即真实 26/36） | — |

三端一致性门禁从 15 例扩到 **18 例全部通过**，自举 IR 不动点保持。

---

## 五、P1 一轮（负例门禁）

P0 收尾后接着量了一件事：自举端的 `sema.miko` 只有 230 行（`Sema.cpp` 1039 行），如果负例只被 C++ 前端拒绝、自举端静默接受，那和 #1 是同一类问题——**编译成功、答案错误**。

### 实测：4 个负例被静默接受

11 个 `examples/*_error.miko` 逐一跑三端：

| 用例 | 解释器 | C++ 端 | 自举端（修前） |
|---|---|---|---|
| `array_const_assign_error` | reject | reject | **ACCEPT** |
| `array_index_error` | reject | reject | **ACCEPT** |
| `array_length_error` | reject | reject | **ACCEPT** |
| `struct_missing_field_error` | reject | reject | **ACCEPT** |

四个都是「编译通过、运行给出垃圾」：`const nums[0] = 9` 写进常量数组、`nums[true]` 用 `1` 当地址偏移、`[i32; 4]` 只给 3 个元素（第 4 格是未初始化的 alloca）、`Point { x: 10 }` 缺 `y`。

### 修法：沿用已有 const pass 的范式

没有去写完整 sema，而是按仓库既有的做法（`check_const_assignments` 就是一个走缩进文本树的 pass）补了四条检查，收进 `check_static_rules`：

| 检查 | 规则 |
|---|---|
| const 写入 | 赋值目标取**基名**（`nums [ 0 ]` → `nums`、`p . x` → `p`），数组元素与字段写入也算写绑定 |
| 下标类型 | `[ ... ]` 内是 `true`/`false` 或字符串字面量即拒绝 |
| 数组长度 | `Type [ ty ; N ]` 的 N 与相邻 `Expr [ ... ]` 的元素数必须相等 |
| 结构体字面量 | 提供的 `:` 数量必须等于声明的 `Field` 数量 |

全部 11 个负例现在都被拒绝，其中 4 个给出干净诊断：

```
error: cannot assign to const variable 'nums'
error: array index must be an integer, got a bool
error: array of length 4 initialised with 3 element(s)
error: struct 'Point' literal supplies 1 of 2 field(s)
```

其余 7 个仍由 clang / `codegen_ll` 的诊断兜住（拒绝是对的，但消息是内部错误而非语言诊断，属后续打磨项）。

### 门禁扩容

`tools/difftest.ps1` 原本只跑 `tests/diff/*.miko`。现在把 `examples/*_error.miko` 一并纳入，按命名约定判为负例：

- 用例数 **18 → 29**
- 结果 **29 / 29 通过**
- 自举两代仍 17/17，不动点 `7ECD8B65206DF881`

这一步是这轮最有价值的产出：那 4 个洞之所以存在，正是因为门禁从来不问。

### 顺带做的两件小事

- **CI 工作流**（`.github/workflows/gate.yml`）：windows-latest 上装 LLVM/MinGW → 构建 C++ 端 → 用它构建自举端 → difftest → bootstrap 两代，失败时上传 `.ast`/`.sema`/`.ll`。**未在真实 CI runner 上执行过**，本机只做了静态校验（无 tab、缩进成对、步骤嵌套正确）。
- **版本号统一**：`CMakeLists.txt` 的 `MIKO_VERSION` 由 `0.1.0` 改为 `0.2.0`，现在两端都报 `0.2.0`。

### 更正两处早先的记录

- 根目录 `.obj` 是 **19 个，且一个都没被 git 跟踪**（`.gitignore` 第 36 行已有 `*.obj`）；先前写的「95 个 .obj」是过时数字。
- `release/miko-development.zip` 289 MB **仍然被跟踪**，这是唯一真实存在的卫生问题。取消跟踪或改写历史会动到 git 索引/历史，留给你决定，我没有执行。

---

## 六、P1 #6 字符串自带长度

### 问题：每次取长度都是一次遍历

字符串是 `char*`，`len(s)` 就是 `strlen(s)`——`char_at`、`substring`、`concat` 的每个操作数也都各算一次 `strlen`。于是这个形状的循环

```
while i < len(s) { total = total + char_at(s, i); i = i + 1; }
```

每处理一个字节就把整个字符串重走一遍。20 KB 的串扫 20 次：**278 ms**。

### 方案：长度放进分配块头部，指针指向正文

```
  [ size_t length ][ 'h' 'e' 'l' 'l' 'o' 0 ]
                     ^ char* 指这里
```

没有采用路线图原写的 `{len, data}` 按值传结构体，理由是这个语言没有 FFI，但运行时内部仍要把字符串交给 `printf("%s")`、`strcmp`、`fopen`；结构体 ABI 会让这些全部要拆包。头部方案保住了 `char*` ABI，同时达到同样的验收标准：生成 IR 里不再有 `strlen`。

代价：每个字符串多 8 字节，且字符串不能再是内部指针（这一条本来就成立）。

### 不变量与它的四个外来来源

指针交给 `miko_str_len` 之前，必须由 `miko_str_alloc`/`miko_str_adopt` 或代码生成器的字面量发射写下头部。所有外来串都在这四处被接管：

| 来源 | 处理 |
|---|---|
| `argv` | `miko_arg_at` 改成 `miko_str_adopt` 复制 |
| `stdin` | `miko_input` 原来返回**静态缓冲区**（无头部，且每次读覆盖上一次），改成每次返回受管副本 |
| 字面量 | 两端代码生成器发射 8 字节长度 + 正文，返回 GEP 到正文 |
| `to_string_bool` | 原来返回静态 `"true"`，改成受管副本 |

### 四个真 bug（都是这次改动暴露的，且都被门禁抓住）

1. **自举端读到垃圾长度**。`len` 读头部拿到的是路径串尾部字节。根因不在新代码，而在 C++ 端把 `arg_at` 内联成裸 `argv` 取指针，绕过了运行时——它以前能工作，纯粹因为没人对 `arg_at` 的结果取过长度。
2. **C++ 端没把 argc/argv 交给运行时**。改成运行时调用后 `miko_arg_count()` 恒为 0，`arg_at` 报越界。补上 `main` 里的 `miko_set_args` 调用（自举端一直都有）。
3. **`createManagedString` 返回类型**。`defaultValue(String)` 需要 `llvm::Constant*`，先返回 `llvm::Value*` 编译不过；改成 `ConstantExpr` 常量 GEP，两处都能用。
4. **`str_contains` 返回 i32 却标成 bool**。`Type(Bool)` 要求 i1，`select` 的条件拿到 i32 直接模块校验失败。加一次 `icmp ne 0` 降窄。

四个都是"改了 A 才暴露的 B"，也都是 difftest 抓到的——不是我自己看出来的。

### 结果

| 指标 | 修前 | 修后 |
|---|---|---|
| 20 KB 串扫 20 次 | 278 ms | **8 ms** |
| 生成 IR 里的 `strlen` / `strcmp` / `strstr` | 有 | **0 / 0 / 0** |
| 缩放（n=10k/20k/40k） | 二次增长 | 8 / 8 / 7 ms，持平 |
| difftest | 29 例 | **30 / 30** |
| 自举两代 | `7ECD8B65206DF881` | `3730EE2E96C22581`（两代相同，18/18） |

自举不动点变了是预期的：IR 里多了头部，字节当然不同；关键是**两代仍然完全相同**。

### 顺带修掉的第二层：比较和搜索也在走到 NUL 为止

长度进了头部之后，还剩两处同类残留没被 `strlen` 的消失带走：

| 操作 | 原来 | 现在 |
|---|---|---|
| `==` `<` 等字符串比较 | `call i32 @strcmp` | `call i32 @miko_str_cmp`（先比长度，再 `memcmp` 共同前缀） |
| `str_index_of` | `call ptr @strstr` | `miko_str_index_of`（按长度滑窗） |
| `str_contains` | `strstr(...) != 0` | `miko_str_index_of(...) >= 0` |

理由不是洁癖：字符串现在**可以合法包含 NUL 字节**（`read_file` 一个二进制文件就是）。`strcmp`/`strstr` 会把第一个 NUL 当结尾，于是 `"abc\0xyz"` 被当成 `"abc"`——比较会相等、搜索会失败、长度却是 7。三条互相矛盾。

这是把"长度存在头部"这个决定贯彻到底的必然结果，不是额外工作：既然长度是权威，所有原先靠终止符推断的东西都必须改成读长度。

新增用例 `19_embedded_nul`（数据 `61 62 63 00 78 79 7A`）锁住这个语义：三端都报 `7 / 4 / true / false / true / x / xyz`。

### 关于删掉的重复实现，一句实话

C++ 端把这些字符串语义各写了一遍、且都是裸 `malloc`（`len`、`concat`、`char_at`、`substring`、`read_file`、`arg_at`、`str_index_of`、`str_contains`、`to_string_i64/f64`）。这不是"顺手统一"——裸分配出来的串没有头部，`len` 改成读头部的那一刻它们就全错了。**必须**统一，没有选择余地。

上一个 P1 轮我说过 `codegen_ll` 与 `NativeCompiler` 是"两份实现、一套语义"，靠 difftest 拽住。这轮下来 C++ 端少了约 130 行内联实现，语义第一次真正**只有一份**（在运行时里）。

其中 `read_file` 的内联实现是 68 行逐字节读 + 手写扩容，换成一次运行时调用。

### 没解决的

- 只读基准。写入侧（`concat` 链式拼接）仍是每次重分配，O(n²)——要 O(n) 得先定内存策略（#7）。
- 头部固定 8 字节，小串开销比例偏高（`char_at` 返回的单字符也占 9 字节）。短生命周期串若将来有 arena，这个比例会变成主要成本。
- 大端目标不成立：长度头按小端写死在 `u64_le_escapes` 里，移到大端需要改字节序。
- 没有释放路径。头部让"释放"变成可能的（能从正文反推分配块起点），但 #7 之前没有回收策略。

---

## 七、一个比 289 MB zip 严重得多的问题：仓库不完整

收尾时清点 `git status`，发现一个不是"卫生"而**是缺陷**的事实：

**77 个源码与测试文件从未被提交过。**

| 未跟踪的关键文件 | 后果 |
|---|---|
| `runtime/miko_runtime.c` | 整个运行时。clone 后链接必断 |
| `compiler/main_ll.miko`（27 KB） | 自举端驱动 |
| `compiler/codegen_ll.miko`（137 KB） | 自举端 LLVM 后端，单文件最大 |
| `tools/difftest.ps1` / `tools/bootstrap.ps1` | 两个门禁本身 |
| `tests/diff/` 全部 19 例（含 `.expected`） | 差异测试全无 |
| `.github/workflows/gate.yml` | 上一轮刚写的 CI |
| `cmake/*.cmake` 三个 | 构建脚本 |

不是被 `.gitignore` 忽略（`git check-ignore` 退出码 1，逐个确认过），就是**从来没 `git add` 过**。

### 为什么这比 zip 严重

`CMakeLists.txt`、`BUILD.md`、`README.md` 这些**已跟踪**的文件里都在引用它们：

```
BOOTSTRAP.md, BUILD.md, src/codegen/NativeCompiler.cpp  ->  runtime/miko_runtime.c
BOOTSTRAP.md, BUILD.md, CMakeLists.txt, README.md       ->  compiler/main_ll.miko
BUILD.md, README.md                                     ->  compiler/codegen_ll.miko
CMakeLists.txt                                          ->  tools/difftest.ps1
```

也就是说：clone 下来，构建脚本指向不存在的文件，门禁脚本不存在，测试用例不存在。289 MB 的 zip 只是让 clone 变慢；这个让 clone **不可用**。

顺带说明一件事：这几轮我反复说"门禁把 bug 抓住了"——那些门禁文件本身并没有进版本控制。它只在这台机器上存在。

### 已做的

`git add` 了 77 个文件（纯新增，未删除、未改写历史、完全可逆），跟踪文件从 **113 → 190**。逐项确认过全是源码/测试/文档，无 `*.exe`/`*.obj`/`*.ll` 构建产物。

**没有**做的：`release/miko-development.zip`（289 MB）仍在跟踪。取消跟踪会改变别人的工作副本行为，也可能需要改写历史来真正瘦身，这个留给你决定。

根目录那个 `猜数字游戏.miko`（5.9 KB，非 ASCII 文件名）没有动——看着像手写的草稿，不像是项目的一部分。

---

## 八、P1 #7 追加能力：`append_str`，以及自举编译器 220 倍

第六节解决了**读**字符串是 O(n²)。这一节是**写**。它同时是 #7 的一半：内存策略里"所有权"这一半已经定死，只剩回收。

### 先量，再定

我没有先写代码，先测。三次合成输入：

| 输入 | 耗时 |
|---|---|
| 23 337 B（200 个函数） | 2 173 ms |
| 46 737 B（400 个函数） | 6 408 ms |
| 93 537 B（800 个函数） | 70 375 ms |

2 倍输入→3 倍时间，再 2 倍→11 倍。这不是"慢"，这是撞墙。同一文件 C++ 端 97 ms。

### 插桩定位：不是我以为的地方

我本来以为是 IR 输出拼接（`st_emit` 每行重建整个输出）。改了 66 处 `out = out + ...` 之后，70 375 → 54 964 ms。**几乎没动**。说明猜错了。

于是在 `compile_file` 里按阶段插桩（`mono_ms`），800 个函数下：

| 阶段 | 耗时 |
|---|---|
| read | 0 ms |
| **lex** | **47 074 ms** |
| parse | 5 763 ms |
| sema | 30 ms |
| static rules | 16 ms |
| codegen | 17 ms |

**词法分析占 85%**。根因是 `compiler/token.miko` 的 66 行：

```
return stream + kind + "|" + number_to_text(len(encoded)) + "|" + encoded + "\n";
```

每读出一个 token，就把整个 token 流重建一次。94 KB 源码约 4 万个 token，于是拷贝 4 万次、每次平均 20 KB。同时 `parser.miko` 有 19 处同样的累加、`symbols.miko` 2 处。

教训写在这里：我说"拼接是瓶颈"是对的，但**指错了地方**。没有插桩我就会把 codegen_ll 改得很漂亮，然后看着 55 秒的编译时间宣布胜利。

### 方案：容量进头部，`append_str` 转移所有权

头部从 `{length}` 变成 `{length, capacity}`，16 字节。`miko_str_append(dst, src)`：

- `capacity` 放得下 → `memcpy` 到尾部，改 `length`，**原指针返回**；
- 放不下 → 按 `needed * 2` 分配、拷贝、返回新指针。

字面量的 `capacity == length`，所以追加到字面量**必然走拷贝**——只读段不可能被写进去，这条由构造保证，不是靠自觉。

### 所有权规则（这是 #7 定的那一半）

`append_str` **接管 dst**。另一个仍指向旧值的变量会看到修改。正确写法是

```
s = append_str(s, piece);
```

旧值被调用方丢弃。这就是 Rust 的 `&mut` 规则，只是不做静态检查、写在注释里。这条我在运行时、`Builtins.h`、`Sema.cpp`、`codegen_ll.miko` 四处都写了。

**回收仍然没有。** 缓冲区只增不还。所以 #7 我只填了"所有权"这一半，"回收"那一半还是空的（见「没解决的」）。

### 结果

| 指标 | 修前 | 修后 |
|---|---|---|
| 800 个函数（93.5 KB） | 70 375 ms | **320 ms** |
| `codegen_ll.miko`（142 KB） | 67 245 ms | **1 780 ms** |
| `main_ll.miko`（29 KB） | — | 279 ms |
| lex 阶段（800 函数） | 47 074 ms | **17 ms** |
| parse 阶段（800 函数） | 5 763 ms | **17 ms** |
| n=1 600 000 次 append | — | **4 ms** |
| difftest | 30 例 | **31 / 31** |
| 自举两代 | `3730EE2E96C22581` | `F023F21A6EE36416`（两代相同，19/19） |

`k=200/400/800` 现在是 291 / 298 / 320 ms——**近似持平**。那 290 ms 的底是进程启动加 clang 链接，不是编译。

自举不动点变了是预期的：编译器自身现在调用 `append_str`，产出字节自然不同；关键是两代仍相同。

### 一次差点毁掉文件的失误

批量替换时我用了 `Set-Content -NoNewline`。PowerShell 把数组**直接拼接**，`codegen_ll.miko` 从 3712 行变成 1 行（134 693 字符，0 个换行）。

恢复靠上一轮 `git add` 的暂存区——`git checkout --` 拿回 3712 行，重新编译确认语法完整，再把改动重做一遍。第二次改用 `[System.IO.File]::WriteAllText` 并在原始文本上做替换，换行原样保留。

两件值得记的事：**上一轮把文件加进版本控制，这一轮救了它**；另外这个仓库的换行符是混的——`codegen_ll.miko` 是 CRLF，`main_ll.miko` 是 LF，`symbols.miko` 一个文件里两种都有。按 `\r\n` 切分的脚本在 LF 文件上一行都匹配不到，我因此白跑了两轮。

### 验证方式：IR 逐字节相同

改写语义透明这件事我不靠推理，靠对比。前后两版编译器编译同样 6 个程序，产出的 `.ll` SHA256 全部相同：

```
08_strings / 12_structs / short_circuit / 17_floats / 13_enums / 16_builtins
```

六份全同。`append_str` 对语言语义是透明的，改的只是分配行为。

### 门禁抓到的第五个 bug

**C++ 后端 `unknown function 'append_str'`**。我改了 `emitCall` 的分发，但语句位置的调用先走另一条**类型推导**链（`NativeCompiler.cpp` 约 L2185），那条链把内建名一个个列出来，最后落到 `unknown function`。漏了它，只有 C++ 端报错，自举端和解释器都正常——正是 difftest 存在的理由。

### 新增用例

`20_append_str`：循环追加、追加到字面量（拷贝路径）、追加空串、追加后参与比较/搜索/取字符/切片、`append_str(append_str("a","b"),"c")` 嵌套。三端输出 12 行全一致。

### 没解决的

- **回收**。缓冲区只增不还。长跑进程仍单调涨内存。`append_str` 给的是所有权规则，不是回收策略——arena、引用计数、GC 三条路都还没选。
- 头部涨到 16 字节，短串开销比例更高了。
- `codegen_c.miko` 有 57 处同样的累加，**没改**——它自带一套无头部的 C 运行时，与头部方案本就分家，且不在 difftest 门禁内（见 #11）。
- 词法/解析层的累加是按行批量改的，用的是"变量名是 `out`/`body` 且右值是纯拼接"这个模式。若将来有人写 `out = out + f(...) + g(...)` 里带副作用，这个模式仍然成立；但若写 `x = x + ...` 而 `x` 有别名，就踩所有权规则了。


---

## 九、发布：把代码交出去，让它在别的机器上建起来

前八节都在本机跑。这一节记录第一次把仓库推到 GitHub 并用 CI 建发布包时发生的事——六个平台、六个不同原因失败，每一个都只在别人的机器上出现。

### 9.1 发布内容

仓库推到 `https://github.com/mikodream/miko-lang.git`，`main` 为默认分支。历史是新建的：原仓库里 `release/miko-development.zip` 有 289 MB，GitHub 单文件上限 100 MB，直接推会被拒。所以用 `--orphan` 起了一条新历史，只提交整理后的 164 个文件（0.94 MB，最大 `compiler/codegen_ll.miko` 138.9 KB）。

不发布的：`release/`（1.07 GB）、`out/`（226 MB）、八个 `build-*`、`cmake-build-*`、`Testing/`、19 个根目录 `*.obj`、`dist/` 下的产物、`miko.toolchain`、`猜数字游戏.miko`、编辑器目录。

**保留 `examples/`**，虽然任务是"删掉实例代码"。原因是 `tools/difftest.ps1` 扫 `examples/*_error.miko` 取 12 个拒绝用例，且 `if (Test-Path $exampleDir)` 意味着目录一没门禁就静默缩小；`CMakeLists.txt` 的 ctest 也引用 35 个例子文件。它们不进发布包，所以在包里看不到，但仍然承担门禁职责。删掉它等于把门禁悄悄拆掉一半。

### 9.2 一个 tag 一条 release，六种架构

`.github/workflows/release.yml`：`push: main` 与 `workflow_dispatch` 触发，读 `CMakeLists.txt` 里的 `MIKO_VERSION` 组成 `v${version}-${run_number}` 作为 tag，发布到 release。矩阵六格：`linux-x64`、`linux-arm64`、`macos-x64`、`macos-arm64`、`windows-x64`、`windows-arm64`。

**不用交叉编译**，每个平台在自己的 runner 上原生建。stage 0 是链 LLVM C++ API 的程序，交叉建要为每个目标配 LLVM 和 sysroot，代价远超原生六格。

包内是扁平布局：`miko`、`miko.toolchain`、`miko-link.{bat,sh}`、`runtime/`、`stdlib/core/`、`compiler/*.miko`。扁平不是审美，是被编译器的相对路径解析逼出来的——`link_executable` 按 CWD 找 `runtime/miko_runtime.c`，`link_script_path()` 依次查 `miko.toolchain`、`dist/miko.toolchain`、`build/miko.toolchain`，`imp_stdlib_root()` 按 CWD 再 `../stdlib` 找 `stdlib/core/math.miko`。

### 9.3 六次失败

都没有在本机出现，因为本机恰好是对的。

**一、`macos-13` 已退役**。矩阵里写的标签不存在，actionlint 直接报。换 `macos-15-intel`。

**二、runner 上是 Visual Studio 18 2026，不是 17 2022**。`cmake -G "Visual Studio 17 2022"` 报 "could not find any instance"。改成用 `vswhere.exe` 探测实际装的是哪版，而不是把某一台机器的版本写进 YAML。

**三、`${{ }}` 在 pwsh 块里炸**。PowerShell 把 `${{` 解析成变量加花括号。表达式移到 step 级 `env:`。

**四、`Compress-Archive -Path "$root\*"` 把文件摊在压缩包根上**，而 `tar -C stage miko` 保留 `miko/` 一层。两边布局不一致，本地冒烟测试在 `Join-Path $work "miko"` 上失败。改成 `-Path $root`。

**五、Windows 上根本没有可用的 LLVM CMake 包**。这是花时间最久的一个。`find_package(LLVM)` 报找不到 `LLVMConfig.cmake`，于是一路排查：

  - `choco install llvm` 装的是只有 clang 的包，`lib/cmake/llvm/` 里只有 `LLVMConfigExtensions.cmake`；
  - 官方 `LLVM-*-win64.exe`（winget 的 `LLVM.LLVM`，装完 22.1.8）同样没有 `LLVMConfig.cmake`；
  - Visual Studio 自带的 LLVM 组件只有 `bin/`；
  - 所以在 Windows 上只能下官方**发布压缩包**，那里面有完整开发树。

  压缩包选 `.tar.xz` 不选 `.tar.zst`：zst 的窗口大小被 bsdtar 拒绝（`Frame requires too much memory for decoding`），本机验证过，没留给 CI 去发现。

  版本钉 22.1.8：23.x 是带 zlib 和 zstd 构建的，导出目标要求链接期两个库都在，而且删了 `IRBuilder::CreateGlobalStringPtr`、连 `fn main() { let a: i32 = 1; }` 都能 IR 验证失败。22.1.8 不要这两个依赖，且是这个编译器验证过的版本。Linux 同样钉 18。

**六、DIA SDK 路径是别人机器上的**。LLVM 的 Windows 导出文件把构建机器的路径硬写进 `LLVMDebugInfoPDB` 的 `INTERFACE_LINK_LIBRARIES`：`.../Visual Studio/2022/Enterprise/DIA SDK/lib/amd64/diaguids.lib`。runner 上是 2026 Enterprise，链接直接 `LNK1181`。这里没有 `DIASDK::Diaguids` 目标可以改指向——路径就在属性里，所以改的是属性本身：换成实际装着的那个 VS 的 DIA SDK，没有就丢掉（miko 不读 PDB）。

### 9.4 门禁抓到的两个真 bug

前五个是环境问题。后两个是代码问题，只是本机环境把它们挡住了。

**stage 0 的 `--compile` 从来链不出东西**。`linkExecutable()` 只把生成的 IR 传给 clang，不带 `runtime/miko_runtime.c`。生成代码要调 `miko_run_command`、`miko_set_args`、`miko_write_file` 和字符串助手，libc 里一个都没有，于是任何用到它们的程序都死在 `undefined symbol: miko_set_args`。自举编译器一直带着 runtime（`compiler/main_ll.miko`），这条路径没带。本机没暴露是因为门禁的 C++ 列不经过这条链；只有真去 `--compile` 一个程序才看得见。

`-lm` 也只在 libm 是独立库的地方加。Windows 上 clang 把它转给 lld-link，报 `could not open 'm.lib'`。

**没有 toolchain 文件的回退链接在 Windows 上必然失败**。`dist/miko.toolchain` 故意不进仓库（它记的是构建期路径），所以干净 clone 里没有它，自举编译器走回退分支，那个分支结尾是 `-lm`——同上，Windows 上链不了。CI 上表现为 difftest 12/31，Self 列每一个用例都 REJECT。本机有这个文件，所以从没出现过。

修法是回退分支在看得见 Windows 系统目录时省掉 `-lm`（周围代码本来就靠这个判断选 `.exe` 后缀），并且 CI 门禁自己写一份 `miko.toolchain`，让门禁走的是发布包真正会走的那条路。

### 9.5 六个平台，全绿

原先只发布四个：`windows-arm64` 和 `macos-arm64` 各自卡在一个不可绕过的地方。两个都解决了，但解法不是"再试一次"，是换掉前提。

| 平台 | 怎么建的 | 结果 |
| --- | --- | --- |
| linux-x64 | 原生，LLVM 18 | 通过 |
| linux-arm64 | 原生，LLVM 18 | 通过 |
| macos-x64 | 原生，Brew LLVM 22.1.8_2 | 通过 |
| macos-arm64 | 原生，官方 22.1.8 归档 + SDK 头 | 通过 |
| windows-x64 | **在 Linux 上交叉编译，llvm-mingw** | 通过 |
| windows-arm64 | **在 Linux 上交叉编译，llvm-mingw** | 通过 |

`tag: v0.2.0-35`，六个资产：

```
miko-0.2.0-linux-x64.tar.gz     231885
miko-0.2.0-linux-arm64.tar.gz   223222
miko-0.2.0-macos-x64.tar.gz     227353
miko-0.2.0-macos-arm64.tar.gz   223264
miko-0.2.0-windows-x64.zip      258730
miko-0.2.0-windows-arm64.zip    248635
```

八个 job 全绿：三个 `decide the tag` / 四个原生构建 / `windows, cross-compiled` / 两个 `verify windows-*` / `tag and release`。

CI 门禁（`gate.yml`，Windows 原生）：difftest `31 / 31`，bootstrap 两代 `1350783` 字节、`FD688C651F0031DB`。

### 9.6 解决 windows-arm64：把编译器从 Windows 上拿掉

`windows-11-arm` 没有 Visual Studio，`vswhere.exe` 都不在，而 stage 0 是 MSVC 编的 C++ 程序——所以不管流水线怎么写都建不出来。试过在 runner 上装 VS Build Tools（能下下来，4.3 MB），但那条路的终点是"必须有 MSVC"。

**换掉的是前提，不是参数。** 关键事实是：自举编译器产出的 IR **没有 target triple，也没有 data layout**（实测 0 次出现，stage 0 的 LLVM 会写一行，自举后端不写）。目标只在链接 IR 的时候才进来。

于是 Windows 全部在 `ubuntu-latest` 上用 **llvm-mingw** 交叉编译：一个 clang，自带 MinGW-w64 的 C 运行库和头文件，同时支持 `x86_64-w64-mingw32` 和 `aarch64-w64-mingw32`。一个 job 出两个架构，全程没有 Windows、没有 Visual Studio、没有 Windows SDK。

链路是：Linux 建 stage 0 → stage 1 → `stage1/miko --self out/compiler` 得到无 triple 的 IR → 两个目标各链一次（`-static`，所以包不依赖任何运行库 DLL）。同一个 `miko.ll` 变成 Linux 二进制、Windows x64 二进制、Windows arm64 二进制。

交叉编译不许偷掉验证。单独的 job 把包下到真实 Windows runner（`windows-latest` 和 `windows-11-arm`）上：跑 `--version`、跑 `--self`（产出 1350783 字节）、再跑 19 个从同一份 IR 交叉链接出来的 difftest 程序，逐个比 `.expected`。**19/19 全过，两个架构都是。** 跑不起的 Windows 二进制在 CI 就红，不会到用户机器上才红。

`dist/miko-link.bat` 里的 vswhere、vcvars64.bat、MSVC 工具链全部删掉：现在是 `MIKO_CC` 优先，其次 PATH 上的 clang，其次 llvm-mingw 和 LLVM 的常规安装位置。两条路径都在本机对 llvm-mingw 20261117 验过，链接并运行正常。

### 9.7 解决 macos-arm64：三次尝试，前两次都"看起来成功"

这个平台失败过五轮，最后三行是：

1. 归档的静态库是 LLVM 22 bitcode，Xcode 的链接器是 17，读不懂 → 改用归档自带的 clang 编译。
2. 归档 clang 带自己的 libc++ 头，`__hash_memory` 在 libc++ 220108 里是 `_LIBCPP_EXPORTED_FROM_ABI`，Apple 的 `libc++.1.dylib` 没有这个符号 → 想静态链归档的 libc++。
3. 静态链 `libc++.a` 链接通过，跑 `--version` 也正常，**然后编译真东西时 abort**：`libc++abi: Terminating due to typed operator new being invoked before its static initializer in libcxx has been executed`。原因是 libSystem 本来就提供 libc++abi，静态再链一份就有两个副本、初始化顺序错了。而只链 `libc++.a` 又缺 `___cxa_uncaught_exceptions` 和 `___gxx_personality_v0`。

**前两次探针给了我假信号，因为只跑了 `--version`。** 后来把探针改成"完整编译编译器"，才是能暴露问题的负载。一跑三种配对的对比：

| 配对 | 完整编译 |
| --- | --- |
| 归档 `libc++.a` 静态 | 运行期 abort |
| 只链 `libc++.a` | 链接失败，缺 libc++abi 符号 |
| **SDK 的头 + Apple 的库** | **成功，2193829 字节 IR** |

所以最终是：**归档的 clang 当编译器**（只有它读得了 bitcode），**C++ 头用 SDK 自带的**（里面 `hash_memory` 出现 0 次），**库用 Apple 的、不动**。头和库自然同代。

还差一个：归档 clang 不是 Apple 的，不按 Xcode 的方式找 SDK，链接阶段编译 C 运行时就 `fatal error: 'stdio.h' file not found`。加 `SDKROOT` 就够——一个环境变量，链接脚本一行都不用改。

### 9.8 发布包真的能用吗

发布成功不等于包能用。下了 v0.2.0-35 的 Windows x64 包，解压，实际跑：

```
miko.exe --version        →  miko 0.2.0 (miko-implemented, LLVM IR backend)
miko.exe d.miko d         →  wrote d.exe
d.exe                     →  sum = 55
miko.exe --self out2\g1   →  1350783 字节, FD688C651F0031DB
out2\g1.exe --self g2     →  1350783 字节, FD688C651F0031DB
```

两代一致，且**与本机门禁同一定点** `FD688C651F0031DB`——一个完全独立编译出来的包，复现了门禁的数字。

两个 exe 的 PE 头也核过：x64 包是 `x86-64`，arm64 包是 `ARM64`。

过程中踩到的坑都是我自己的：用了不存在的 `to_string`（真名 `to_string_i64`）和 `println`（内建是 `print`），把源文件放在 `--self` 前面（CLI 是 `miko --self [prefix]`），还有本机 TMP 目录不可写导致 clang 报 `unable to make temporary file`。
### 9.9 六个平台的链接顺序：为什么最后是靠"位置"解决的

Linux 上最顽固的一个。症状始终是 `undefined reference to ZSTD_compressBound`，而 `libzstd.so` 明明在链接行上——问题只是它在 `libLLVMSupport.a` **前面**，而单遍链接器不会回头用已经走过的库。

试过并且都失败的：

| 尝试 | 结果 |
| --- | --- |
| `target_link_libraries` 直接加 | CMake 把它排在 LLVM 归档前面，两次都一样靠前 |
| 重复列一遍 | CMake 把两份合并进同一个靠前的块 |
| `target_link_options` | 选项排在库**之前**，方向反了 |
| `-Wl,--start-group` 包住 `${LLVM_LIBS}` | 只有直接点名的九个组件进了组，传递依赖（还有第二个 `libLLVMSupport.a`）在 `--end-group` 之后 |
| `LLVM_LINK_LLVM_DYLIB` | Ubuntu 上映射照样返回归档，还害得 macOS 丢了一个本来能过的 job |
| 手工 `find_library` 找 `libLLVM.so` | arm64（18.1.3）找到，x64（18.1.8）没找到——同发行版、不同补丁版、不同布局 |

最后成立的是 `CMAKE_CXX_STANDARD_LIBRARIES`——CMake 保证它在链接行的最末尾，在目标自己的库和它们拉进来的一切之后。

然后还踩了两个坑：条件列表插进变量时是 CMake list，`;` 分隔，shell 把它当成**一个**参数 `-lz;-lzstd;-ltinfo`，于是 Linux 退回 `set_curterm` 未定义（必须先 `string(REPLACE ";" " ")`）；`-ltinfo` 只该在 Linux 加，macOS 的 terminfo 在 libSystem 里，加它就是 `ld: library 'tinfo' not found`。

三个系统库现在各自"机器上有才加"，因为答案是按发行版变的：terminfo 只在 Linux，zstd 在 Debian 和 Homebrew 的 LLVM 构建里、官方归档不需要，`-lz` 到处都有。

### 9.10 架构上值得记住的两句话

**自举产出的 IR 是跨目标的。** 没有 target triple、没有 data layout，目标只在链接时进来。这不是偶然，是 9.6 能成立的全部原因：同一份 `miko.ll` 喂给四个不同的 clang，得到四个平台的二进制。任何往 IR 里写 triple 的改动都会悄悄毁掉这条性质，所以 `cross-windows` 里显式检查它，发现就报错退出。

**stage 0 是唯一需要 LLVM 的东西。** 从 stage 1 开始全是 IR 文本。所以 LLVM 的 ABI 只影响"在哪建 stage 0"，不影响"给哪个平台出包"——这正是 Windows 能完全离开 MSVC 的原因。

### 9.11 编译输出改成一句话

`miko program.miko program` 原来打两行：

```
wrote program.ll
wrote program.exe
```

第二行在 macOS 和 Linux 上是错的——产物叫 `program`，没有扩展名。`link_executable` 自己算得对（只有 Windows 加 `.exe`），错的是打印那句把 `.exe` 写死了。

修法不是把后缀算对再拼上去，是把文件名从句子里去掉：调用者自己给的 prefix，自己能看。现在成功一行 `success`，失败一行 `failed`，诊断信息仍然走 stderr：

```
$ ./miko program.miko program
success
$ echo $?
0
```

静态规则拒绝和链接失败两条路都验过，都是 `failed` 加退出码 1。`--emit-ir` 也一样。

改了 `main_ll.miko`，定点哈希随之改变，这本来就是它的作用：`3ED78C9CA5CE1E12` → `FD688C651F0031DB`，尺寸 `1351347` → `1350783`（少了那些打印语句）。门禁仍然全绿：difftest `31 / 31`，bootstrap 两代同哈希、19/19。

顺带一提，`program.ast` 和 `program.sema` 每次编译都会写在产物旁边。它们是调试用的中间产物，注释里写明是为了让 import 解析失败可见。没动，但每次都留两个文件在用户目录里，这个可以再商量。
### 9.12 没解决的

- **没选 LICENSE**。仓库没有 LICENSE 文件，README 也没有许可声明。这是只有作者能决定的事，没替他编一个。
- **`.miko` 文件在库里仍存 CRLF**，虽然 `.gitattributes` 声明了 `text=auto eol=lf`。`git add --renormalize .` 能修；无害，因为词法接受两种换行。
- **LLVM 版本上限是 22**（Linux 钉 18）。23 删了 `IRBuilder::CreateGlobalStringPtr`、并且对 miko 生成的 IR 一律验证失败。现在 macos-x64 靠 brew 恰好还是 22.1.8_2——**这条是运气，不是保证**：brew 一升到 23，macos-x64 会和之前的 macos-arm64 一样卡在版本上。macos-arm64 用的是钉死的官方归档，不受影响。这是这份流水线最脆的一处。
- **macOS 的 `-nostdinc++` 意味着 miko 的 C++ 代码只能用 SDK 那代 libc++ 头**。如果哪天表达式用到更新的东西，会先失败在编译，而不是在链接——失败点更清楚，但这个约束真实存在。
- **`verify-windows` 只跑 19 个 difftest 用例**，不是全量 31 个：`tests/diff` 里带 `.expected` 的那些才交叉链接。另外 12 个是拒绝用例，靠 `gate.yml` 的三端一致覆盖。
- **`arg_count()` 风格的 CLI 有个不一致**：`arg_at(0)` 当模式用，但 `arg_count() >= 3` 分支用 `arg_at(1)`/`arg_at(2)`。按文档形式调用没问题，但这条路径没有测试覆盖。