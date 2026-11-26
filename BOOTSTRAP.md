# Miko 自举路线方案

本文记录 mikolang 从当前 C++ 实现的编译器走向自举的建议路线。

## 当前状态

当前项目已经具备一个可用的 stage0 编译器：

- stage0 由 C++ 实现；
- 构建目标为 `miko`；
- 已有 lexer、parser、sema、interpreter、native codegen 等模块；
- 已支持部分 `--run`、`--debug-*`、`--compile` / LLVM 相关输出能力；
- 仓库中已有 examples、stdlib、packages、benchmarks 等目录。

项目也已经有第一版 Miko 编写的 stage1 原型：

```text
compiler/main.miko
compiler/token.miko
compiler/lexer.miko
compiler/parser.miko
compiler/ast.miko
compiler/symbols.miko
compiler/type.miko
compiler/sema.miko
compiler/codegen_c.miko
```

这版 stage1 目前采用“文本 token stream / AST dump / sema dump”的方式推进，已经可以处理一批 examples 并生成可编译的 C。严格自举闭环还没有完成，因为 stage1 还不能可靠地编译 `compiler/*.miko` 生成可运行的新编译器。下一阶段重点是把这个原型从 example-driven C 后端推进到能覆盖 compiler 自身所用语言子集。

目标中的完整自举链条应类似：

```text
stage0: C++ miko 编译器
    ↓ 编译
stage1: miko 写的 miko 编译器
    ↓ 编译自身
stage2: stage1 生成的新 miko 编译器
```

当 stage1 和 stage2 能够编译同一批程序并得到一致行为时，可以认为自举闭环初步完成。

## 核心结论

当前最现实的路线不是直接让 miko 编译器绑定 LLVM API，而是：

```text
冻结一个 bootstrap 子集
补齐最小语言和标准库能力
用 miko 写一个子集编译器
先生成 C 或 LLVM IR 文本
完成 stage0 -> stage1 -> stage2 闭环
```

其中，优先推荐第一版后端采用：

```text
miko -> C
```

原因：

- 比直接生成 object 或调用 LLVM C++ API 简单；
- 可以利用现有 C 编译器和链接器；
- 更容易调试生成结果；
- 可以逐步将运行时能力放到 C runtime 中。

## 当前已具备的基础

### stage0 编译器

现有编译器由 C++ 实现，主要模块包括：

```text
src/main.cpp
src/lexer/Lexer.cpp
src/parser/Parser.cpp
src/sema/Sema.cpp
src/runtime/Interpreter.cpp
src/codegen/NativeCompiler.cpp
```

CMake 构建目标为：

```text
miko
```

这可以作为未来自举流程中的 stage0。

### 基础语言能力

当前语言已经具备：

- 整数、浮点、字符、字符串、布尔；
- `let` / `const`；
- 函数；
- `if` / `else`；
- `while` / `for`；
- `break` / `continue`；
- `return`；
- 固定长度数组；
- `struct`；
- import / module 雏形；
- interpreter 运行路径；
- LLVM native codegen 雏形。

### 已有测试基础

当前 CMake 中已经包含大量：

- `--debug-tokens`；
- `--debug-ast`；
- `--debug-types`；
- `--run`；
- 错误用例测试。

这些可以作为语言稳定性的基础。

## 关键缺口

## P0：必须先解决

### 1. stage1 原型还不是可闭环的自举编译器

仓库中已经有 `compiler/*.miko`，当前能力包括：

- `compiler/lexer.miko`：读取源码并输出文本 token stream；
- `compiler/parser.miko`：把 token stream 转成文本 AST dump；
- `compiler/sema.miko`：基于 AST dump 做最小类型/符号检查；
- `compiler/codegen_c.miko`：把 AST dump 生成 C；
- `compiler/main.miko`：串起 read/lex/parse/sema/codegen/write。

当前缺口是：parser/sema/codegen 仍然主要面向 examples，内部结构是字符串而不是真正的 token list / AST / type graph。下一步不是“新建编译器源码”，而是逐步让现有 stage1 覆盖它自身用到的语言子集。

待办：

1. 固化 `compiler/main.miko --run -- <input> <output-prefix>` 的 smoke test；
2. 让 `codegen_c.miko` 支持 stage1 自身用到的内建函数、模块调用、普通函数调用语句、赋值、嵌套 if/while；
3. 让 `parser.miko` 对复杂表达式保持足够结构，避免 codegen 只能靠字符串猜测；
4. 逐步把 token/AST/symbol 从字符串协议替换为结构化数据；
5. 加入 stage0 -> stage1 -> stage2 的 CTest 闭环。

### 2. 缺少写编译器需要的数据结构

编译器实现至少需要：

- 动态数组 / `Vec<T>`；
- 字符串切片；
- 字符访问；
- Map / HashMap；
- enum / tagged union；
- Option / Result；
- 递归数据结构；
- 字符串 builder；
- arena allocator 或类似内存策略。

当前已有固定长度数组和 struct，但还不足以舒服地表达 token list、AST list、symbol table 等结构。

优先补齐：

1. `enum`；
2. 动态数组；
3. 基础字符串操作；
4. 简单 map 或专用符号表；
5. arena allocator。

### 3. 标准库不足以支撑命令行编译器

当前标准库 I/O 很小，只能读一行和打印字符串。自举编译器至少需要：

- `read_file(path): string`；
- `write_file(path, content)`；
- `args()` 或 `argv`；
- `exit(code)`；
- stderr 输出；
- 路径拼接 / 规范化；
- 判断文件是否存在；
- 可选：执行外部命令，例如调用 `clang` 或 `lld`。

短期可以先通过 stage0 内建函数提供这些能力，不必一开始实现完整 FFI。

### 4. 后端路线需要明确

当前 native 后端依赖 C++ 调用 LLVM API。miko 语言目前还不能直接使用 LLVM C++ API。

可选路线：

1. `miko -> C`，然后调用 C 编译器；
2. `miko -> LLVM IR 文本`，然后调用 LLVM 工具链；
3. miko 实现 FFI，调用 LLVM C API；
4. miko 直接生成 object / machine code。

第一版推荐路线：

```text
miko -> C
```

后续再考虑 LLVM C API 或直接 object writer。

## P1：自托管前应补齐

### 5. extern fn / FFI

语言中已经预留了 `extern` 关键字，但还需要完整实现：

```miko
extern fn puts(s: string): i32;
extern fn malloc(size: i64): ptr;
```

不过这会牵涉 `ptr` 类型和 ABI。为了快速完成第一版自举，可以先不做完整 FFI，而是提供少量编译器内建函数。

### 6. native compile 测试

当前测试主要验证解释器和 debug 输出。自举必须依赖真实编译路径，所以需要增加：

- `--compile` smoke tests；
- `--emit-ir` tests；
- `--emit-obj` tests；
- 编译后运行生成程序的端到端测试；
- stdlib / package native compile 测试；
- bootstrap stage1 / stage2 测试。

### 7. 内存模型

编译器会创建大量对象：

- token；
- AST node；
- type；
- symbol；
- string；
- diagnostic。

第一版建议使用：

```text
arena allocator
```

编译过程中分配，进程结束后整体释放即可。这样比一开始实现 GC 或复杂所有权系统简单很多。

### 8. 诊断系统

自举编译器需要统一诊断结构，例如：

```miko
struct SourceLocation {
    file: string,
    line: i32,
    column: i32,
}

struct Diagnostic {
    location: SourceLocation,
    message: string,
}
```

这可以让 lexer、parser、sema 共享错误报告机制。

## P2：质量和维护性

### 9. 语言子集冻结

在完成第一版自举前，应冻结一个 bootstrap subset，不要无限增加语法。

建议第一版子集包含：

- `fn`；
- `let` / `const`；
- `if` / `else`；
- `while`；
- `return`；
- `struct`；
- `enum`；
- `string`；
- `i32` / `i64` / `bool`；
- 动态数组；
- 简单模块；
- 文件 I/O 内建函数。

第一版可以暂时不支持：

- 泛型；
- 高级 trait / interface；
- GC；
- 完整 FFI；
- 复杂优化；
- 直接生成 object；
- LLVM API binding。

### 10. interpreter 与 native 语义一致性

当前项目同时有解释器路径和 native codegen 路径。自举时应保证关键语言特性在两边行为一致。

建议：

- 每个语言特性都同时跑 `--run` 和 `--compile`；
- 对数组、struct、string、module 等重点特性做 native 端到端测试；
- bootstrap 编译器只使用 native 后端已稳定支持的子集。

## 推荐实施路线

## 阶段 0：稳定 stage0

目标：

```text
现有 C++ miko 编译器可以稳定编译 bootstrap subset
```

待办：

1. 补充 `--compile` 相关测试；
2. 确认 examples 中关键程序既能 `--run`，也能 native compile；
3. 明确 Windows 下 clang / lld 依赖；
4. 固化 bootstrap subset 文档。

## 阶段 1：补最小语言和标准库能力

优先顺序：

1. 实现 `enum`；
2. 实现动态数组；
3. 实现基础字符串 API：
   - `char_at`；
   - `substring`；
   - `starts_with`；
   - `string_builder`；
4. 实现文件 I/O：
   - `read_file`；
   - `write_file`；
5. 实现命令行参数；
6. 实现简单 map 或符号表；
7. 实现 arena allocator。

## 阶段 2：加强现有 Miko 版 lexer / parser

已有：

```text
compiler/token.miko
compiler/lexer.miko
compiler/parser.miko
compiler/ast.miko
compiler/main.miko
```

目标：

```text
Miko 写的前端可以稳定读取 compiler/*.miko，并保留 codegen 所需的结构信息
```

优先改进点：

- 复杂表达式和嵌套调用不要退化成难以识别的字符串；
- import alias / qualified call 要有一致的表示；
- block 边界、if/else/while 嵌套要能让后端正确闭合作用域。

## 阶段 3：加强 Miko 版 sema

已有：

```text
compiler/type.miko
compiler/symbols.miko
compiler/sema.miko
```

目标：

```text
能够检查 stage1 编译器自己用到的语言子集
```

第一版 sema 不需要覆盖完整语言，只覆盖 bootstrap subset。

## 阶段 4：加强 miko -> C 后端

已有：

```text
compiler/codegen_c.miko
```

目标：

```text
miko 源码 -> C 源码
```

可配套新增独立 C runtime，或继续把最小 runtime 内联到生成的 C 中：

```text
runtime/miko_runtime.c
runtime/miko_runtime.h
```

生成 C 时可以先以可读、可调试为优先，不追求优化。

## 阶段 5：完成第一次自举闭环

目标流程：

```text
stage0: C++ miko 编译器
    ↓ 编译 compiler/*.miko
stage1: miko 写的 miko 编译器
    ↓ 编译 compiler/*.miko
stage2: stage1 生成的新 miko 编译器
```

验证方式：

1. stage1 编译一组 examples；
2. stage2 编译同一组 examples；
3. 比较两者输出行为；
4. 第一版不要求二进制完全一致，行为一致即可。

## 建议的第一批 TODO

如果要马上推进，建议按这个顺序：

1. 保持 bootstrap C e2e tests 通过；
2. 让 `compiler/main.miko --run -- compiler/main.miko out/bootstrap_compiler_main` 生成的 C 包含完整 file I/O、args、字符串 helper 和普通调用语句；
3. 补齐 parser/codegen 对 block 边界、嵌套 if/while、赋值、break/continue 的支持；
4. 将 stage1 生成的 `compiler/main.miko` C 与各 `compiler/*.miko` 模块合并/链接为可运行 stage1；
5. 用 stage1 重新编译 examples，并和 stage0 产物做行为对比；
6. 再逐步推进 `enum`、动态数组和结构化 AST，减少字符串协议的脆弱性。

## 完成标准

第一版自举完成标准：

```text
1. stage0 能编译 stage1；
2. stage1 能编译自身生成 stage2；
3. stage1 和 stage2 能编译同一批 bootstrap tests；
4. stage1 和 stage2 编译出的程序行为一致；
5. bootstrap 流程可以通过一条构建命令或 CTest 自动执行。
```

最终更严格的完成标准：

```text
stage1 和 stage2 输出字节完全一致
```

### 当前进度：字节级固定点已达成 ✅

第一版（行为一致）与最终（字节一致）两项标准目前都已满足：

- `cmake/BootstrapStage1.cmake` 的 `miko_bootstrap_stage0_stage1_stage2_smoke`
  CTest 在运行完整 stage0→stage1→stage2 链条后，**逐模块断言**
  `stage1_<module>.c == stage2_<module>.c`（共 9 个模块，全部字节一致）。
- 手动生成 8 代链条（g1…g8）验证：g1 到 g8 每个模块的 C 输出完全字节相同，
  即链条是真正的**字节固定点**，而非只是行为收敛。

关键修复（见 `compiler/codegen_c.miko`）：

- `replace_text` 现在**感知 C 字符串/字符字面量**——只在字面量外部做子串替换。
  之前它会对编译器自身发出的 C 字符串常量（如 `infer_c_expr_type` / `convert_len_expr`
  里的 `"len ( "` needle）盲目替换，导致每一代自举把自身源码里的 needle 文本改写，
  进而让 `sema`/`type`/`codegen_c` 三个模块在 `len ( x )` 与 `strlen(x )` 之间**永
  恒震荡**，永远收不到字节固定点。改为字面量感知后，重写是幂等的（对已重写文本再
  重写结果不变），链条从第一代起即字节稳定。

后续若继续向 P1/P2 演进（FFI、arena、结构化 AST、枚举、动态数组），应保持
"codegen 重写幂等 / 字节固定点" 这一不变量：每次修改 `codegen_c.miko` 后重跑
`miko_bootstrap_stage0_stage1_stage2_smoke`，确认 9 个模块仍字节一致。

## 2026-10 语言能力补齐：自举子集 v2 基础 ✅

字节固定点达成后，按 P0 清单为 stage0 补齐了写"真正的编译器"所需的语言基础
（P0 清单中的 enum、动态数组、字符串 API、符号表支撑）。这些能力目前只在
stage0（解释器 + LLVM 原生后端）落地，`compiler/*.miko`（stage1）保持原样，
自举链与字节固定点不受影响。

### 新语言特性

- `enum`：i32 表示的有作用域常量，`Color::Red` 引用；支持 `pub enum`
  从模块导出（`alias.Enum::Member` 引用）；可作为参数 / 返回 / 字段类型。
- `vec<T>`：引用语义的动态数组。解释器侧用 shared storage 实现，LLVM 后端
  用堆上 `{len, cap, data}` 头结构实现，语义一致。支持 `vec_new` /
  `vec_push` / `vec_pop` / `vec_set` / `vec_reserve` / `vec_clear`、
  `v[i]` 读写（带越界检查）、`len(v)`。

### 新内建函数

- 转换：`to_string_i64` / `to_string_f64` / `to_string_bool` /
  `to_string_char` / `str_to_i64` / `str_to_f64`
- 搜索：`str_index_of` / `str_contains`
- 数学：`sqrt_f64` / `floor_f64` / `ceil_f64` / `round_f64` / `f64_to_i64`
- 系统：`exit` / `eprint` / `file_exists`

### 核心库（stdlib/core，全部纯 miko）

- `core/string`：trim / split / join / replace / upper / lower / repeat /
  reverse / pad / starts_with / ends_with 等；
- `core/math`：i32/i64/f64 的 min/max/abs/clamp、gcd / lcm / pow；
- `core/io`：read_line / print_line / eprint_line；
- `core/collections`：StringMap / IntMap（基于两个平行 vec 实现的符号表，
  map_set / map_get / map_has / map_remove / map_keys）。

### 顺带修复的既有缺陷

- 解释器：char 与整数的 `==` / `!=` 曾按字符串比较（`' ' == 32` 为 false），
  现按数值比较，与原生后端一致；
- LLVM 后端：函数返回 struct / 数组（右值聚合）在 let / return / 传参时
  无法拷贝，现统一"右值聚合先物化到临时 alloca"；
- LLVM 后端：数组 / vec 的 struct 元素读出后不再是值而是地址，
  `v[0].x` 链式字段访问可用；vec_push / vec_set 对 struct 元素做 memcpy；
- LLVM 后端：补上 `seed_i64` / `rand_i64` 的实现（此前只有调用点没有定义，
  `--compile` 一旦用到即链接失败）；
- Windows 下 VS 生成器构建的测试现在优先用 clang 作为测试 C 编译器
  （自举 e2e 链接参数是 clang 风格，cl.exe 无法执行）。

### 对自举的影响

- `miko_bootstrap_stage0_stage1_stage2_smoke` 在 ninja(clang) 与 VS(LLVM)
  两种构建下均 78/78 通过，9 个模块字节固定点保持；
- stage1 尚未使用上述任何新特性。后续若要让 `compiler/*.miko` 用 enum /
  vec 重写 token / AST / symbol（摆脱字符串协议），需要先在 stage1 的
  parser / sema / codegen_c 中支持这些特性，并保持重写幂等不变量。

## 2026-10 第二阶段：stage1 支持 enum ✅

stage1（miko 写的 miko→C 编译器）现在能解析、检查并编译使用 `enum` 的
源程序。这是"让 compiler/*.miko 改用结构化数据"的前置条件——只有 stage1
自己认识 enum，它才能编译出"用 enum 写的下一版 stage1"。

### 实现方式（保持字符串协议，最小侵入）

- `compiler/lexer.miko`：`::` 现在产出单个 `DoubleColon` token（此前是两个
  `Colon`）；
- `compiler/parser.miko`：顶层 `enum` / `pub enum` 声明解析为 AST dump 的
  `Enum <Name>` + `Member <Name>` 行；
- `compiler/sema.miko`：注册 `Enum` 声明；裸 `Enum :: Member` 表达式推断为
  该 enum 类型（比较表达式不受影响）；
- `compiler/codegen_c.miko`：
  - `collect_enums` 先扫描树收集枚举声明；
  - `rewrite_enum_types` 把 enum 类型标注（Type/Param/ReturnType/Field）
    重写为 `i64`，使既有 `map_type` 路径直接映射为 `long long`；
  - `emit_enum_decls` 生成 C 匿名枚举：`enum { Color__Red = 0, ... };`；
  - `lower_expr` 中新增 `convert_enum_refs`：表达式文本 `Enum :: Member`
    重写为 `Enum__Member`（用字面量感知的 `replace_text`，源码里含 `::` 的
    字符串字面量原样保留——这是字节固定点不被破坏的关键）；
  - `number_to_text`：手写十进制转换（stage1 运行时没有 to_string，且编译
    器源码从不做 string+int 拼接——新代码也必须遵守这个子集纪律）。

### 验证

- 新 e2e：`miko_bootstrap_enum_stage1_c_e2e` —— enum 程序经
  stage0→(stage1 生成 C)→clang→运行，输出与 stage0 解释器语义一致；
- **字节固定点保持**：改完 `compiler/*.miko` 后，9 个模块的
  stage1/stage2 C 输出仍逐字节一致（新代码只用 stage1 已支持的旧子集：
  if/while/字符串操作，不用 else-if、不用 vec/enum 自身）；
- ninja 构建 81/81、VS(LLVM) 构建 81/81 全部通过。

### 下一步：stage1 支持 vec（设计草图）

在 codegen_c 中引入 C 侧 vec 运行时（与 stage0 LLVM 后端的堆上
`{len, cap, data}` 头结构对齐）：

1. 生成 C 头部新增：
   ```c
   typedef struct { long long len; long long cap; void* data; } miko_vec;
   static miko_vec* miko_vec_new(long long elem_size);
   static void miko_vec_push(miko_vec* v, void* elem, long long elem_size);
   static void* miko_vec_at(miko_vec* v, long long i, long long elem_size);
   static void  miko_vec_pop(miko_vec* v); /* + vec_set/vec_clear/vec_reserve */
   ```
2. `vec<T>` 类型标注在 rewrite 阶段映射为 `miko_vec*`（T 需要保留以确定
   `elem_size`，可在 lowering 时通过 `infer_c_expr_type` 推断元素大小）；
3. 表达式重写：`vec_new ( )` → `miko_vec_new(sizeof(...))`、`vec_push ( v , x )`
   → `miko_vec_push(v, &tmp, sizeof(...))`（struct 元素需先物化临时变量）、
   下标读写 `v [ i ]` → `(*(T*)miko_vec_at(v, i, sizeof(T)))`；
4. `len ( v )` 的 convert_len_expr 需区分 vec 参数（`->len`）；
5. 每步改完重跑固定点冒烟，保持重写幂等。

vec 落地后，即可开始把 token/AST/symbol 从字符串协议重写为
`enum NodeKind + struct Node { kids: vec<Node> }` 的结构化版本。

## 自举链条说明

`cmake/BootstrapStage1.cmake` 目前驱动的链条：

```text
stage0 (C++ miko, 仓库里叫 miko.exe)
   │  用 --run 把 compiler/*.miko 逐个译成 C 并写出
   ▼
stage1 (由 stage0 译出的 C 链接成的编译器二进制)
   │  用它再编译 compiler/*.miko，得到 stage2 的 C
   ▼
stage2 (由 stage1 译出的 C 链接成的编译器二进制)
   │  再编译一批 examples，并与 stage1 产物做行为对比
   ▼
stage1 与 stage2 对每个 compiler 模块的 C 输出必须字节一致（本次已达成）
```

## 第二条自举链：直接生成 LLVM IR（2026-11 达成 ✅）

前面那条链（`codegen_c.miko`）是"miko → C → 二进制"，借 C 编译器做汇编。现在项目里还有第二条完全独立的链：**miko 直接生成 LLVM IR 文本**，由 clang 汇编，不经过任何中间语言。

### 为什么是两条链而不是一条

`codegen_c.miko` 的实现方式是字符串重写，它靠标识符名字猜类型（看到 `.text`、`.kind`、`.index` 这类字段名就知道该写什么 C）。这套规则是为最初那 9 个模块量身写的，不是通用类型推导。让它去编译 `codegen_ll.miko`（里面有 `ty`、`.out`、`.structs` 这些它没见过的名字）会推出 `unknown` 类型，产出编译不过的 C。

所以两条链各自闭环，互不依赖：

```text
C 链     : compiler/main.miko    + codegen_c.miko   → 9 个 .c → gcc/clang
LLVM 链  : compiler/main_ll.miko + codegen_ll.miko  → 1 个 .ll → clang
```

两条链共享同一个前端（`token` / `ast` / `symbols` / `type` / `lexer` / `parser` / `sema`），只有后端不同。

### LLVM 链的关键决定：整程序单模块

IR 链把**所有模块合成一个 LLVM 模块**再生成，而不是一个源文件一个 `.ll`。原因是跨模块调用需要真实签名：`sema` 调 `symbols.has_symbol` 时，后端必须知道它返回 `i1` 而不是 `i64`。分成多个模块时，每个模块只看得到自己的函数表，跨模块调用就只能靠猜——猜错就是 `call i64 @symbols__has_symbol` 去调一个 `define i1` 的函数，链接器不报错，运行时读到垃圾。

合成一个模块后，所有定义都在眼前，调用按签名解析，不需要任何外部声明。

代价是命名空间冲突：多个模块都有叫 `find_char` 的私有辅助函数。解决办法是按模块名修饰——定义是 `parser__find_char`、`sema__find_char`，调用拼 `alias.fn` 时解析成 `alias__fn`，裸名调用解析成当前模块的 `当前模块__fn`。`main` 保持原名作为入口。

### chain 与不动点

`cmake/BootstrapLLVM.cmake` 驱动的链条：

```text
stage0 (C++ miko)
   │  解释执行 compiler/main_ll.miko：读全部模块 → lex → parse → 拼成一个程序
   │  → sema → codegen_ll → out/whole_program.ll
   ▼
stage1_ll (clang 把 .ll 汇编 + 链接 runtime/miko_runtime.c)
   │  用它再编译整个编译器 → stage2 IR
   ▼
stage2_ll
   │  再编译一次 → stage3 IR
   ▼
stage1 IR == stage2 IR == stage3 IR，三者字节一致（当前 809246 字节，已达成 ✅）
```

三者字节一致才是自举的真正判据。只证明"它能跑起来"不够——那只能说明这一代产物正确；字节相同说明**后端是自己输出的不动点**，经过 LLVM 往返没有信息损失、没有漂移。

实测比预期更强：连 `stage1 IR` 都和 `stage2 IR` 相同。也就是说**第一次经过 LLVM 的输出就已经在不动点上**，根本不存在需要收敛的代际漂移。测试脚本因此断言三代的全部两两相等，而不是只比较最后两代。

### 过程中修掉的真实缺陷

这一步把 LLVM 后端从"能给玩具程序生成 IR"推到"能编译整个编译器"，暴露并修掉了一批语义分歧。每一个都是后端与解释器行为不一致，而不是代码写得难看：

- **`&&` / `||` 不短路**。原本折叠成 `and i1` / `or i1`，两侧都求值。于是 `i < n && char_at(t, i) == ' '` 会越界读字符串——解释器短路的写法在原生代码里崩掉。改成用控制流 + i1 槽真正短路，右操作数只在需要时求值，且各自按正确优先级（`&&` 取相等层、`||` 取 and 层）。这是栈溢出和越界崩溃的共同根源。
- **alloca 落在循环体内**。LLVM 的 `alloca` 在执行到它时才分配栈空间，放在循环里就是每轮迭代都增长栈，`stage1_ll` 直接 0xC00000FD 栈溢出。改成把 alloca 全部提升到函数入口块，函数收尾时拼到头部之后。顺带抽出了 `st_begin_function`，消掉 8 处重复的 main 函数头。
- **`miko_char_at` 的 ABI 不匹配**。运行时返回 C 的 `char`（i8），IR 却声明成 `i32`。x86-64 上子字长返回值的寄存器高位未定义，`char_at(...) == '\n'` 就开始随机失败。改成按 i8 声明 + `sext` 扩展。
- **`'\r'` 转义缺失**。`unescape_code` 有 `'n'`、`'t'`、`'0'`、`'\\'`、`'"'` 但没有 `'r'`，于是 `'\r'` 被当成字母 `r`（114）。编译出来的编译器在 `encode_text` 里把所有字母 `r` 前插了 `\r` 转义，标识符 `number_to_text` 变成 `numbe\r_to_text`。stage0 的 C++ 解码器早先踩过同一个坑，这是它在 IR 后端的镜像。
- **赋值语句丢掉右值**。`total = total + i` 只生成了自赋值 `load; store`——`lower_line_expr` 拿到 `Assign` 行时按表达式解析，`=` 根本没被处理。改成识别顶层赋值并转交语句 lowering。
- **控制流标签计数器被丢弃**。分配完 then/else/end 三个标签后又用回旧状态，导致每个 if 都复用 `if.then.0`，IR 里出现三个同名标签。
- **空基本块**。LLVM 不允许标签紧跟标签（块必须有终结指令）。`st_label` 现在会在上一块未终结时补 fallthrough `br`。
- **模块别名的命名修饰**。`sema.miko` 用 `import "type.miko" as types;` 导入，调用写 `types.type_unknown`，但定义按文件名修饰成 `type__type_unknown`。别名和模块名可以不相等，所以跨模块调用改为按裸函数名在全局签名表里解析到真实定义名。

### 怎么跑

```powershell
# 只跑 LLVM 自举链（约 3 分钟）
cmake -DMIKO_EXE=build-vs-llvm/Release/miko.exe `
      -DCLANG_EXE="C:/Program Files/LLVM/bin/clang.exe" `
      -DSOURCE_DIR=. -DBOOTSTRAP_OUT_DIR=out/llvm-chain `
      -P cmake/BootstrapLLVM.cmake

# 或通过 ctest（需要 -DMIKO_ENABLE_LLVM=ON 配置）
cd build-vs-llvm; ctest -C Release -R miko_bootstrap_llvm_self_hosting --output-on-failure
```

### 单文件模式

`main_ll.miko` 同时是个普通 IR 编译器，可以直接把一个 miko 程序编成 `.ll`：

```powershell
miko.exe compiler/main_ll.miko --run -- examples/structs.miko out/demo
# out/demo.ll → clang -c → 和 runtime 一起链接即可运行
```

第一个参数是模块子集（逗号分隔，用于隔离调试），第二个参数起是单文件编译。无参数时编译整个编译器。
