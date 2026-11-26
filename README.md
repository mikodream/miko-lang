# miko 编程语言开发文档

miko 是一个使用 C++ 和 LLVM 实现的轻量级静态编程语言。它采用接近 C / Java 的语法风格，面向 Windows、Linux、macOS 以及嵌入式平台，包括 ARM、RISC-V、ESP32 等目标。

本文档从零开始说明 miko 应该先做什么、怎么搭建工程、怎么设计语言、怎么实现编译器，以及后续如何支持跨平台和嵌入式部署。

## 1. 项目目标

miko 的核心目标：

- 语法接近 C / Java，降低学习成本。
- 静态编译，生成高效本地代码。
- 弱类型体验，减少显式类型声明。
- 支持变量自动类型推断。
- 编译器实现保持轻量，便于嵌入式平台裁剪。
- 基于 LLVM 生成多平台目标代码。
- 第一阶段优先完成可运行的最小编译器，再逐步扩展语言能力。

需要特别说明的是，miko 这里的“弱类型”建议定义为：

- 源码层允许较多隐式类型转换，例如 `int` 到 `float`、数字到 `bool`。
- 变量一旦通过推断确定类型，后续仍然保持静态类型约束。
- 不建议在底层实现成完全动态类型语言，否则会明显增加运行时复杂度，不利于嵌入式平台。

也就是说，miko 更适合做成“静态类型 + 自动推断 + 宽松转换”的语言。

## 2. 当前开发环境

你当前机器上的工具路径：

```text
CMake / MinGW: C:\mingw64
LLVM:         C:\Users\Administrator\env\llvm
Project:      C:\Users\Administrator\project\mikolang
```

推荐使用：

- C++17 或 C++20。
- CMake 管理工程。
- LLVM 提供词法后端、IR 生成、优化和目标代码生成。
- MinGW 作为 Windows 下的 C++ 编译工具链。
- CLion 作为主 IDE，用于编辑、CMake 配置、构建、调试、运行测试和管理编译器开发流程。

后续 CMake 配置会重点适配这两个路径：

```powershell
$env:PATH="C:\mingw64\bin;C:\Users\Administrator\env\llvm\bin;$env:PATH"
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## 3. 推荐项目结构

建议从一开始就把编译器拆成清晰模块：

```text
mikolang/
  CMakeLists.txt
  README.md
  docs/
    language-design.md
    compiler-pipeline.md
    embedded-targets.md
  examples/
    hello.miko
    variables.miko
    functions.miko
  include/
    miko/
      ast/
      lexer/
      parser/
      sema/
      codegen/
      driver/
  src/
    ast/
    lexer/
    parser/
    sema/
    codegen/
    driver/
    main.cpp
  tests/
    lexer/
    parser/
    sema/
    codegen/
  runtime/
    miko_runtime.c
    miko_runtime.h
```

各目录职责：

- `src/lexer`: 词法分析，把源码拆成 token。
- `src/parser`: 语法分析，把 token 转成 AST。
- `src/ast`: 抽象语法树节点定义。
- `src/sema`: 语义分析、类型推断、隐式转换检查。
- `src/codegen`: LLVM IR 生成。
- `src/driver`: 命令行参数、编译流程调度。
- `runtime`: 小型运行时，负责打印、内存辅助、嵌入式适配等。
- `examples`: miko 示例程序。
- `tests`: 单元测试和端到端测试。

## 4. 第一阶段：先做最小可运行语言

不要一开始就实现完整语言。第一阶段只实现能跑起来的最小闭环。

目标：

```java
fn main() {
    let x = 10;
    let y = 20;
    print(x + y);
}
```

编译后输出：

```text
30
```

第一阶段语法：

```text
program      := function*
function     := "fn" identifier "(" param_list? ")" block
param_list   := param ("," param)*
param        := identifier ":" type
block        := "{" statement* "}"
statement    := let_stmt | return_stmt | expr_stmt
let_stmt     := "let" identifier ("=" expression)? ";"
return_stmt  := "return" expression? ";"
expr_stmt    := expression ";"
expression   := binary_expr
binary_expr  := primary (operator primary)*
primary      := number | string | identifier | call | "(" expression ")"
call         := identifier "(" arg_list? ")"
arg_list     := expression ("," expression)*
```

第一阶段类型：

```text
i32
i64
f32
f64
bool
string
void
```

第一阶段关键字：

```text
fn let return if else while true false
```

第一阶段先支持：

- `fn main()`
- `let` 局部变量
- 整数、浮点数、字符串、布尔值
- `+ - * /`
- `== != < <= > >=`
- 函数调用
- `print`
- `return`
- 结构体声明、字面量和字段访问

第一阶段暂时不要实现：

- 类
- 泛型
- 闭包
- GC
- 协程
- 宏系统

## 5. miko 语法设计

### 5.1 变量声明

miko 支持自动类型推断：

```java
let a = 10;       // 推断为 i32
let b = 3.14;     // 推断为 f64
let c = true;     // 推断为 bool
let s = "hello";  // 推断为 string
```

也支持显式类型：

```java
let count: i64 = 100;
let ratio: f32 = 0.5;
```

变量默认可变。如果后续想支持不可变变量，可以增加：

```java
const pi = 3.14159;
```

### 5.2 函数

```java
fn add(a: i32, b: i32): i32 {
    return a + b;
}

fn main() {
    let result = add(1, 2);
    print(result);
}
```

返回值也可以推断，但第一阶段建议要求显式返回类型，`main` 和无返回值函数可省略：

```java
fn log(message: string) {
    print(message);
}
```

### 5.3 条件语句

```java
if score >= 60 {
    print("pass");
} else {
    print("fail");
}
```

为了保持 C / Java 风格，也可以允许括号：

```java
if (score >= 60) {
    print("pass");
}
```

建议编译器同时支持有括号和无括号，但代码风格文档推荐无括号。

### 5.4 循环

第一阶段先实现 `while`：

```java
let i = 0;
while i < 10 {
    print(i);
    i = i + 1;
}
```

第二阶段再实现 C 风格 `for`：

```java
for let i = 0; i < 10; i = i + 1 {
    print(i);
}
```

### 5.5 注释

```java
// 单行注释

/*
   多行注释
*/
```

### 5.6 弱类型与隐式转换

推荐隐式转换规则：

```text
i32 -> i64
i32 -> f32
i32 -> f64
i64 -> f64
f32 -> f64
bool -> i32
number -> bool
```

不推荐自动转换：

```text
string -> number
number -> string
pointer -> number
```

字符串拼接可以单独支持：

```java
let name = "miko";
print("hello " + name);
```

### 5.7 枚举（enum）

`enum` 声明一组有作用域的整数常量（从 0 开始编号），成员用 `枚举名::成员名` 引用：

```java
enum Color {
    Red,
    Green,
    Blue,
}

fn describe(c: Color): string {
    if c == Color::Red {
        return "red";
    }
    return "not red";
}

fn main() {
    let favorite: Color = Color::Green;
    print(favorite == Color::Green);   // true
    print(describe(favorite));
}
```

说明：

- enum 值在运行期是 `i32`，可以直接用 `==` / `!=` 比较；
- enum 可以作为参数类型、返回类型、struct 字段和数组元素；
- 模块中用 `pub enum` 声明的枚举可被导入方以 `模块别名.枚举名::成员名` 使用；
- enum 与整数之间不隐式转换。

### 5.8 动态数组（vec）

`vec<T>` 是可增长的动态数组，具有引用语义（赋值 / 传参共享同一份存储）：

```java
fn sum(values: vec<i32>): i64 {
    let total: i64 = 0;
    for let i = 0; i < len(values); i = i + 1 {
        total = total + values[i];
    }
    return total;
}

fn main() {
    let v: vec<i32> = vec_new();
    vec_push(v, 10);
    vec_push(v, 20);
    print(len(v));        // 2
    print(v[1]);          // 20
    v[1] = 99;            // 下标赋值（带越界检查）
    print(vec_pop(v));    // 99
    print(sum(v));        // 10
}
```

vec 相关内建函数：

```text
vec_new()              创建空 vec（元素类型来自 let 的类型标注）
vec_push(v, x)         尾部追加
vec_pop(v)             弹出并返回最后一个元素（空 vec 报错）
vec_set(v, i, x)       下标赋值（带越界检查）
vec_reserve(v, n)      预留容量
vec_clear(v)           清空
len(v)                 元素个数
```

说明：

- `let v = vec_new();` 无法推断元素类型，必须写 `let v: vec<T> = vec_new();`；
- struct 字段、函数参数 / 返回值、数组元素都可以是 `vec<T>`；
- 把 vec 赋给另一个变量（或通过 struct 拷贝）后，两者共享同一份元素存储。

### 5.9 内建函数一览

除 `print` / `len` / `input` / `arg_count` / `arg_at` / `char_at` / `substring` /
`read_file` / `write_file` 之外，stage0 还提供：

```text
字符串 / 数值转换
  to_string_i64(x: i64): string
  to_string_f64(x: f64): string
  to_string_bool(x: bool): string
  to_string_char(code: i64): string
  str_to_i64(text: string): i64        解析失败报运行时错误
  str_to_f64(text: string): f64
  str_index_of(text, needle): i64      未找到返回 -1
  str_contains(text, needle): bool

数学
  sqrt_f64(x: f64): f64
  floor_f64(x: f64): f64
  ceil_f64(x: f64): f64
  round_f64(x: f64): f64
  f64_to_i64(x: f64): i64              向零截断

系统
  exit(code: i32)                      立即退出进程
  eprint(message: string)              输出到 stderr（自动换行）
  file_exists(path: string): bool
  run_command(command: string): i64    交给 shell 执行，返回退出码；
                                       子进程输出直接进终端。自举出来的
                                       miko.exe 用它调用 clang 链接

时间
  clock_ms(): i64                      Unix 纪元起的毫秒（墙钟，等价
                                       System.currentTimeMillis()）
  mono_ms(): i64                       单调时钟毫秒（测耗时用它，
                                       不受系统对时 / 校时影响）

随机数
  seed_i64(seed: i64)                  设置种子
  rand_i64(): i64                      SplitMix64 伪随机数
```

### 5.10 标准库（stdlib/core）

标准库全部用 miko 编写（解释器与原生后端行为一致），通过点号路径导入：

```java
import core.string as string;
import core.math as math;
import core.io as io;
import core.collections as collections;

fn main() {
    print(string.trim("  hello  "));          // hello
    print(string.upper("miko"));              // MIKO

    let parts = string.split("a,b,c", ",");
    print(string.join(parts, "-"));           // a-b-c

    print(math.gcd_i64(48, 18));              // 6
    print(math.pow_i64(2, 10));               // 1024

    let m = collections.map_new();
    collections.map_set(m, "ada", "100");
    print(collections.map_get(m, "ada"));     // 100
    print(collections.map_has(m, "bob"));     // false
}
```

各模块提供的能力：

```text
core/string       trim / trim_start / trim_end、upper / lower、split / join、
                  replace、repeat、reverse、pad_start / pad_end、
                  starts_with / ends_with / contains / index_of、
                  is_digit_char / is_alpha_char / is_space_char
core/math         i32 / i64 / f64 的 min / max / abs / clamp、
                  gcd_i64 / lcm_i64 / pow_i64、is_even_i64 / is_odd_i64
core/io           read_line / print_line / eprint_line
core/collections  StringMap（string -> string）与 IntMap（string -> i64）：
                  map_new / map_get / map_get_or / map_set / map_has /
                  map_remove / map_len / map_keys（IntMap 对应 int_map_*）
```

## 6. 编译器整体流程

miko 编译器建议采用以下流水线：

```text
source code
  -> lexer
  -> token stream
  -> parser
  -> AST
  -> semantic analyzer
  -> typed AST
  -> LLVM IR generator
  -> LLVM optimizer
  -> object file / executable / assembly / IR
```

对应命令：

```powershell
miko hello.miko --compile -o hello.exe
miko hello.miko --compile --emit-ir -o hello.ll
miko hello.miko --compile --emit-asm -o hello.s
miko hello.miko --compile --target riscv32-unknown-elf --emit-obj -o hello.o
miko hello.miko --run
```

## 7. 词法分析器设计

词法分析器负责把源码转换成 token。

Token 类型示例：

```cpp
enum class TokenKind {
    EndOfFile,
    Identifier,
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,

    KwFn,
    KwLet,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwTrue,
    KwFalse,

    Plus,
    Minus,
    Star,
    Slash,
    Equal,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    Comma,
    Colon,
    Semicolon
};
```

Token 应该保存：

- token 类型
- 原始文本
- 行号
- 列号
- 字面量值，可选

第一阶段先手写 lexer，不需要引入 flex。

## 8. 语法分析器设计

Parser 输入 token，输出 AST。

建议使用递归下降解析器，因为 miko 初期语法简单、错误定位也更容易控制。

表达式解析建议使用 Pratt Parser 或 precedence climbing，方便处理运算符优先级。

运算符优先级：

```text
1: =
2: == !=
3: < <= > >=
4: + -
5: * /
6: unary - !
7: call ()
```

AST 节点建议：

```cpp
class Expr;
class Stmt;
class Decl;

class IntegerExpr;
class FloatExpr;
class StringExpr;
class BoolExpr;
class VariableExpr;
class BinaryExpr;
class UnaryExpr;
class CallExpr;

class LetStmt;
class ReturnStmt;
class ExprStmt;
class IfStmt;
class WhileStmt;
class BlockStmt;

class FunctionDecl;
class Program;
```

## 9. 语义分析与类型推断

语义分析负责：

- 符号表管理。
- 检查变量是否已声明。
- 检查函数是否存在。
- 推断 `let` 变量类型。
- 检查二元表达式类型。
- 插入隐式类型转换。
- 检查函数返回值类型。
- 生成 typed AST。

示例：

```java
let x = 10;
let y = 2.5;
let z = x + y;
```

推断过程：

```text
x: i32
y: f64
x + y: f64
z: f64
```

作用域建议：

```text
GlobalScope
  FunctionScope
    BlockScope
```

每进入一个 `{}` 创建一个新作用域。

## 10. LLVM IR 生成

CodeGen 把 typed AST 转成 LLVM IR。

核心类：

```cpp
llvm::LLVMContext context;
llvm::IRBuilder<> builder;
std::unique_ptr<llvm::Module> module;
```

类型映射：

```text
miko i32    -> llvm::Type::getInt32Ty(context)
miko i64    -> llvm::Type::getInt64Ty(context)
miko f32    -> llvm::Type::getFloatTy(context)
miko f64    -> llvm::Type::getDoubleTy(context)
miko bool   -> llvm::Type::getInt1Ty(context)
miko string -> llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(context))
miko void   -> llvm::Type::getVoidTy(context)
```

表达式生成：

- 整数常量生成 `llvm::ConstantInt`。
- 浮点常量生成 `llvm::ConstantFP`。
- 字符串常量生成 global string pointer。
- 局部变量使用 `alloca`。
- 变量读取使用 `load`。
- 变量赋值使用 `store`。
- 算术运算根据整数/浮点生成不同指令。

例如：

```text
i32 + i32 -> CreateAdd
f64 + f64 -> CreateFAdd
i32 < i32 -> CreateICmpSLT
f64 < f64 -> CreateFCmpOLT
```

### 10.1 自举用的文本 IR 后端

上面讲的是 stage0（C++ 实现）通过 LLVM C++ API 生成 IR。编译器自举用的是另一条路：**用 miko 自己写一个后端，直接输出 LLVM IR 文本**，交给 clang 汇编。

后端在 `compiler/codegen_ll.miko`，驱动在 `compiler/main_ll.miko`。

设计上有三个关键决定。

**整程序单模块。** 所有模块的 AST 先拼成一个程序，再一次生成一个 `.ll`。这样跨模块调用能按真实签名解析——`sema` 调 `symbols.has_symbol` 时后端知道它返回 `i1` 而不是 `i64`。如果按文件分成多个 `.ll`，每个模块只看得到自己的函数表，跨模块调用就只能猜类型；猜错不会报错，但运行时会读到垃圾。

**按模块名修饰符号。** 单模块带来命名冲突：多个模块都有 `find_char` 这样的私有辅助函数。定义统一写成 `模块__函数`（`parser__trim_spaces`、`sema__find_char`），调用拼 `alias.fn` 时按裸函数名在全局签名表里解析到真实定义——因为 `import "type.miko" as types;` 里的别名可以和模块名不等。`main` 保持原名。

**无 phi 节点。** 控制流的值流转全部用 `alloca` 槽 + `load`/`store`，生成器状态是纯字符串拼接。代价是 IR 冗余，好处是生成逻辑简单到能用 miko 写出来并自举。

自举结果：

```text
stage0 (C++)        → stage1_ll.ll → clang → stage1_ll.exe
stage1_ll.exe       → stage2_ll.ll
stage2_ll.exe       → stage3_ll.ll
断言 stage1_ll.ll == stage2_ll.ll == stage3_ll.ll（当前 809246 字节，三者字节相同）
```

字节相同才是自举的判据：它说明后端是自己输出的不动点，经过 LLVM 往返没有信息损失、没有代际漂移。只证明"能跑"是不够的——那只能说明这一代产物正确。

验证：

```powershell
cd build-vs-llvm
ctest -C Release -R miko_bootstrap_llvm_self_hosting --output-on-failure
```

## 11. 运行时设计

第一阶段运行时只需要很薄的一层：

```c
void miko_print_i32(int value);
void miko_print_i64(long long value);
void miko_print_f64(double value);
void miko_print_bool(int value);
void miko_print_string(const char* value);
```

源码里的：

```java
print(x);
```

在语义分析或 IR 生成阶段解析成对应运行时函数：

```text
print(i32)    -> miko_print_i32
print(i64)    -> miko_print_i64
print(f64)    -> miko_print_f64
print(bool)   -> miko_print_bool
print(string) -> miko_print_string
```

嵌入式平台上可以替换运行时实现，比如：

- Windows/Linux/macOS 使用 `printf`。
- ARM bare-metal 使用串口输出。
- ESP32 使用 UART 或 ESP-IDF 日志接口。

## 12. CMake 工程配置方向

根目录 `CMakeLists.txt` 后续应做这些事：

```cmake
cmake_minimum_required(VERSION 3.20)
project(miko LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(LLVM_LIBS
    core
    support
    irreader
    native
    target
    passes
)

add_executable(miko
    src/main.cpp
)

target_link_libraries(miko PRIVATE ${LLVM_LIBS})
```

Windows + MinGW 配置时，可能需要显式传入：

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DLLVM_DIR="C:\Users\Administrator\env\llvm\lib\cmake\llvm"
```

如果 LLVM 是用 MSVC 编译的，而项目使用 MinGW，可能会出现 ABI 或链接问题。最稳妥的方式是：

- 使用同一工具链编译 LLVM 和 miko。
- 或者安装/构建 MinGW 兼容的 LLVM。
- 或者改用 Visual Studio 生成器构建 miko。

因为你当前 CMake 使用 MinGW，所以第一步要确认 `C:\Users\Administrator\env\llvm` 是否也是 MinGW 构建产物。

## 13. CLion 完整开发配置

miko 的本地开发目标是：打开 CLion 后，可以完成以下所有工作：

- 自动加载 CMake 工程。
- 使用 `C:\mingw64` 里的 MinGW 编译器。
- 正确找到 `C:\Users\Administrator\env\llvm` 里的 LLVM。
- 直接构建 `miko.exe`。
- 直接运行 `miko --version`、`miko examples/hello.miko --compile --emit-ir -o hello.ll` 等命令。
- 能在 lexer、parser、sema、codegen 中打断点调试。
- 能运行单元测试和端到端测试。
- 能切换 Debug / Release / Sanitizer 等构建配置。

### 13.1 CLion 打开项目

启动 CLion 后选择：

```text
File -> Open -> C:\Users\Administrator\project\mikolang
```

打开后，CLion 会识别根目录的 `CMakeLists.txt`。如果当前还没有 `CMakeLists.txt`，CLion 只能把它当成普通目录，无法完整使用构建和调试能力。

所以 miko 的第一份代码必须先创建：

```text
CMakeLists.txt
src/main.cpp
examples/hello.miko
```

### 13.2 配置 MinGW Toolchain

进入：

```text
File -> Settings -> Build, Execution, Deployment -> Toolchains
```

新增或修改一个 Toolchain：

```text
Name: MinGW miko
Environment: MinGW
MinGW home: C:\mingw64
C Compiler: C:\mingw64\bin\gcc.exe
C++ Compiler: C:\mingw64\bin\g++.exe
Debugger: C:\mingw64\bin\gdb.exe
CMake: bundled 或系统 cmake
Build Tool: C:\mingw64\bin\mingw32-make.exe
```

如果 CLion 自动检测失败，手动填上这些路径。

确认下面命令在 CLion 的 Terminal 里能运行：

```powershell
C:\mingw64\bin\gcc.exe --version
C:\mingw64\bin\g++.exe --version
C:\mingw64\bin\gdb.exe --version
```

### 13.3 配置 CMake Profile

进入：

```text
File -> Settings -> Build, Execution, Deployment -> CMake
```

建议创建两个 Profile。

Debug Profile：

```text
Name: Debug-MinGW
Build type: Debug
Toolchain: MinGW miko
Generator: MinGW Makefiles
Build directory: cmake-build-debug
CMake options:
  -DMIKO_ENABLE_LLVM=OFF
  -DLLVM_DIR=C:/Users/Administrator/env/llvm/lib/cmake/llvm
```

Release Profile：

```text
Name: Release-MinGW
Build type: Release
Toolchain: MinGW miko
Generator: MinGW Makefiles
Build directory: cmake-build-release
CMake options:
  -DMIKO_ENABLE_LLVM=OFF
  -DLLVM_DIR=C:/Users/Administrator/env/llvm/lib/cmake/llvm
```

注意：CLion / CMake 选项里建议使用 `/`，也就是：

```text
C:/Users/Administrator/env/llvm/lib/cmake/llvm
```

这样可以避免 Windows 反斜杠转义问题。

### 13.4 配置环境变量

CLion 的 CMake Profile 可以设置 Environment。

建议加入：

```text
PATH=C:\mingw64\bin;C:\Users\Administrator\env\llvm\bin;%PATH%
```

如果不设置，可能出现：

- 找不到 `llvm-config`。
- 找不到 LLVM 运行时 DLL。
- 构建成功但运行 `miko.exe` 时报 DLL 缺失。

### 13.5 推荐 CMakeLists.txt 形态

为了让 CLion 体验稳定，`CMakeLists.txt` 应该从第一天就支持：

- 明确 C++ 标准。
- 输出 `compile_commands.json`。
- 显示 LLVM 路径。
- 允许暂时关闭 LLVM 链接，先开发 lexer / parser / sema。
- 对 MinGW 给出清晰错误提示。
- 把 include、src、runtime 分开。
- 后续能自然接入测试。

建议第一版：

```cmake
cmake_minimum_required(VERSION 3.20)
project(miko LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MIKO_ENABLE_LLVM "Enable LLVM integration" OFF)

if(MIKO_ENABLE_LLVM AND NOT LLVM_DIR)
    set(LLVM_DIR "C:/Users/Administrator/env/llvm/lib/cmake/llvm")
endif()

if(MIKO_ENABLE_LLVM)
    find_package(LLVM REQUIRED CONFIG)

    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS "LLVM_DIR: ${LLVM_DIR}")
    message(STATUS "LLVM include dirs: ${LLVM_INCLUDE_DIRS}")

    include_directories(${LLVM_INCLUDE_DIRS})
    add_definitions(${LLVM_DEFINITIONS})
endif()

add_executable(miko
    src/main.cpp
)

target_include_directories(miko PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MIKO_ENABLE_LLVM)
    llvm_map_components_to_libnames(LLVM_LIBS
        core
        support
        native
        target
        passes
    )

    target_link_libraries(miko PRIVATE ${LLVM_LIBS})
endif()
```

当前阶段建议在 CLion 里先使用：

```text
-DMIKO_ENABLE_LLVM=OFF
```

原因是当前机器上的 LLVM 配置引用了 MSVC 的 `diaguids.lib`，而项目使用 MinGW 构建时会在链接阶段失败。lexer、parser、语义分析都不需要 LLVM，可以先完整开发；等进入 LLVM IR 生成阶段，再切换到以下方案之一：

```text
1. 使用 MinGW 构建的 LLVM。
2. 在 CLion 中改用 Visual Studio Toolchain。
3. 保持 MinGW 编译器，但改用 LLVM C API 或重新构建兼容版本 LLVM。
```

后续文件多起来以后，再把源文件拆成变量：

```cmake
set(MIKO_SOURCES
    src/main.cpp
    src/lexer/Lexer.cpp
    src/parser/Parser.cpp
    src/sema/Sema.cpp
    src/codegen/CodeGen.cpp
)

add_executable(miko ${MIKO_SOURCES})
```

### 13.6 CLion 运行配置

进入：

```text
Run -> Edit Configurations...
```

建议创建这些运行配置。

#### miko version

```text
Target: miko
Executable: miko.exe
Program arguments: --version
Working directory: C:\Users\Administrator\project\mikolang
```

#### debug tokens

```text
Target: miko
Program arguments: examples/hello.miko --debug-tokens
Working directory: C:\Users\Administrator\project\mikolang
```

#### debug ast

```text
Target: miko
Program arguments: examples/hello.miko --debug-ast
Working directory: C:\Users\Administrator\project\mikolang
```

当前 `hello.miko` 的 AST 输出应类似：

```text
Program
  FunctionDecl main
    BlockStmt
      LetStmt a
        IntegerExpr 10
      LetStmt b
        IntegerExpr 20
      ExprStmt
        CallExpr print
          BinaryExpr +
            VariableExpr a
            VariableExpr b
```

#### emit ir

```text
Target: miko
Program arguments: examples/hello.miko --compile --emit-ir -o build/hello.ll
Working directory: C:\Users\Administrator\project\mikolang
```

#### compile hello

```text
Target: miko
Program arguments: examples/hello.miko --compile -o build/hello.exe
Working directory: C:\Users\Administrator\project\mikolang
```

#### run hello

```text
Target: miko
Program arguments: examples/hello.miko --run
Working directory: C:\Users\Administrator\project\mikolang
```

#### run control

```text
Target: miko
Program arguments: examples/control.miko --run
Working directory: C:\Users\Administrator\project\mikolang
```

这样你可以在 CLion 顶部运行配置下拉框里直接切换不同编译器阶段。

### 13.7 CLion 调试建议

推荐常用断点位置：

```text
src/lexer/Lexer.cpp      nextToken()
src/parser/Parser.cpp    parseFunction(), parseExpression()
src/sema/Sema.cpp        inferExpr(), checkStmt()
src/codegen/CodeGen.cpp  codegenFunction(), codegenExpr()
src/driver/Driver.cpp    compile()
```

调试 lexer 时使用：

```text
examples/hello.miko --debug-tokens
```

调试 parser 时使用：

```text
examples/hello.miko --debug-ast
```

调试 LLVM IR 时使用：

```text
examples/hello.miko --compile --emit-ir -o build/hello.ll
```

CLion 调试窗口里重点看：

- 当前 token。
- AST 节点类型。
- 推断出的 miko 类型。
- LLVM value 的类型。
- 当前 basic block。
- 符号表作用域栈。

### 13.8 CLion 中查看 LLVM IR

建议把 `.ll` 文件输出到 `build/` 或 `examples/out/`：

```text
build/hello.ll
```

CLion 可以直接打开 `.ll` 文件查看。

后续可以增加自定义 External Tool：

```text
File -> Settings -> Tools -> External Tools
```

添加：

```text
Name: llvm-dis
Program: C:\Users\Administrator\env\llvm\bin\llvm-dis.exe
Arguments: $FilePath$
Working directory: $ProjectFileDir$
```

也可以添加：

```text
Name: lli
Program: C:\Users\Administrator\env\llvm\bin\lli.exe
Arguments: $FilePath$
Working directory: $ProjectFileDir$
```

### 13.9 CLion 测试集成

推荐使用 CTest，CLion 能直接识别。

CMake 后续加入：

```cmake
enable_testing()

add_test(NAME miko_version
    COMMAND miko --version
)

add_test(NAME miko_hello_tokens
    COMMAND miko ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello.miko --debug-tokens
)
```

然后在 CLion 里可以：

```text
Run -> Run 'All CTest'
```

或者在 CMake 窗口中直接运行单个测试。

### 13.10 CLion 目录标记

建议在 CLion Project 窗口中右键目录并标记：

```text
include/  -> Mark Directory as -> Project Sources and Headers
src/      -> Project Sources and Headers
tests/    -> Test Sources
examples/ -> 普通目录
docs/     -> 普通目录
runtime/  -> Project Sources and Headers
```

这样搜索、跳转、补全和测试导航会更舒服。

### 13.11 代码风格

建议在 CLion 中设置：

```text
File -> Settings -> Editor -> Code Style -> C/C++
```

基础风格：

```text
Indent: 4 spaces
Continuation indent: 4 spaces
Braces: K&R 或 Stroustrup
Header guards: MIKO_<PATH>_<FILE>_H
```

命名建议：

```text
Class: PascalCase
Function: camelCase
Variable: camelCase
Enum class: PascalCase
Enum value: PascalCase
Private field: trailing underscore, e.g. currentToken_
File: Lexer.cpp / Lexer.h
Namespace: miko
```

### 13.12 CLion 开发工作流

每天开发时建议按照这个循环：

```text
1. 打开 CLion。
2. 等 CMake Reload 完成。
3. 运行 miko version，确认工具链正常。
4. 修改某个小模块，例如 Lexer。
5. 运行对应 debug 配置，例如 debug tokens。
6. 加断点调试。
7. 运行 All CTest。
8. 再进入下一个模块。
```

每个功能都应该有：

- 一个 `.miko` 示例。
- 一个 debug 输出。
- 一个 CTest 测试。
- 必要时一个端到端运行测试。

### 13.13 CLion 常见问题

#### 找不到 LLVMConfig.cmake

现象：

```text
Could not find a package configuration file provided by "LLVM"
```

解决：

```text
Settings -> Build, Execution, Deployment -> CMake -> CMake options
```

加入：

```text
-DLLVM_DIR=C:/Users/Administrator/env/llvm/lib/cmake/llvm
```

并确认这个文件存在：

```text
C:\Users\Administrator\env\llvm\lib\cmake\llvm\LLVMConfig.cmake
```

#### 运行 miko.exe 缺 DLL

现象：

```text
The code execution cannot proceed because LLVM-xxx.dll was not found
```

解决：

在 CMake Profile 的 Environment 中加入：

```text
PATH=C:\mingw64\bin;C:\Users\Administrator\env\llvm\bin;%PATH%
```

#### MinGW 链接 LLVM 失败

现象可能是大量 undefined reference，或者 C++ ABI 相关符号错误。

原因通常是 LLVM 和 miko 使用了不同 C++ ABI，例如 LLVM 是 MSVC 编译，而 miko 用 MinGW 编译。

解决路线：

```text
优先方案：使用 MinGW 版本 LLVM。
备选方案：把 CLion Toolchain 切换到 Visual Studio，并使用 MSVC 构建 miko。
长期方案：自己用 C:\mingw64 编译一份 LLVM。
```

#### CLion 没有识别头文件跳转

解决：

- 确认头文件在 `include/` 下。
- 确认 `target_include_directories(miko PRIVATE include)` 已配置。
- 点击 CMake Reload。
- 必要时 `File -> Invalidate Caches`。

#### GDB 断点不命中

确认：

```text
CMake Profile 是 Debug。
没有打开过高优化级别。
使用的是 C:\mingw64\bin\gdb.exe。
Run Configuration 的 Target 是 miko。
```

## 14. 命令行工具设计

miko 编译器命令建议：

```text
miko <input> [options]
```

选项：

```text
--compile              编译成本地可执行文件，或配合 emit 选项输出中间产物
-o <file>              输出文件
--emit-ir              输出 LLVM IR
--emit-asm             输出汇编
--emit-obj             输出目标文件
--run                  用解释器运行
--target <triple>      指定目标平台
--opt <0|1|2|3>        优化级别
--keep-ir              链接或输出目标文件时保留中间 LLVM IR
--clang <path>         指定 clang 路径
--debug-tokens         输出 token
--debug-ast            输出 AST
--debug-types          输出类型推断结果
```

示例：

```powershell
miko examples/hello.miko --run
miko examples/hello.miko --compile -o hello.exe
miko examples/hello.miko --compile --emit-ir -o hello.ll
miko examples/hello.miko --compile --emit-asm -o hello.s
miko examples/hello.miko --compile --emit-obj -o hello.obj
miko examples/hello.miko --compile --target arm-none-eabi --emit-obj -o hello.o
```

`--emit-ir`、`--emit-asm`、`--emit-obj` 一次只能选择一个。

## 15. 跨平台路线

### 15.1 Windows

第一优先级。

目标：

- 使用 MinGW 编译 miko 编译器。
- 生成 Windows `.exe`。
- runtime 使用标准 C 库。

命令：

```powershell
miko examples/hello.miko --compile -o hello.exe
.\hello.exe
```

### 15.2 Linux

目标：

- 使用 Clang 或 GCC 编译 miko 编译器。
- 生成 ELF 可执行文件。
- 支持 x86_64 Linux。

命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/miko examples/hello.miko --compile -o hello
./hello
```

### 15.3 macOS

目标：

- 使用 Apple Clang。
- 链接 LLVM。
- 生成 Mach-O 可执行文件。

注意：

- macOS 上 LLVM 路径通常来自 Homebrew。
- 需要处理 `@rpath` 或静态链接问题。

### 15.4 嵌入式 ARM / RISC-V / ESP32

嵌入式不建议第一阶段直接生成完整可执行文件。推荐先支持：

```text
miko source.miko --compile --target riscv32-unknown-elf --emit-obj -o source.o
```

输出 `.o` 文件，然后交给目标平台 SDK 链接。

嵌入式限制：

- 不默认使用动态内存。
- 不默认启用异常。
- 不默认启用 RTTI。
- runtime 可替换。
- 字符串功能先保持简单。
- 标准库分层设计。

目标 triple 示例：

```text
arm-none-eabi
thumbv7em-none-eabi
riscv32-unknown-elf
riscv64-unknown-elf
xtensa-esp32-none-elf
```

ESP32 需要特别注意：LLVM 对 Xtensa 支持和具体 ESP-IDF 工具链支持存在版本差异。实践上可以先让 miko 生成 C 或 LLVM IR，再由 ESP-IDF 工具链继续处理。后续再评估直接输出 Xtensa 目标代码。

## 16. 标准库规划

当前内建函数：

```java
print(value)
len(value)
input()
```

当前已有源码标准库：

```text
stdlib/core/math.miko    max_i32, min_i32, abs_i32, clamp_i32
stdlib/core/string.miko  string_len, string_eq, string_concat
stdlib/core/io.miko      read_line, print_line
```

使用方式：

```java
import core.math as math;
import core.string;

fn main() {
    print(math.max_i32(10, 3));
    print(string.string_concat("hello ", "miko"));
}
```

`import core.string;` 默认别名是最后一段 `string`，也可以用 `as` 指定显式别名。模块内函数默认私有，只有 `pub fn` 会进入导出表：

```java
fn helper(value: i32): i32 {
    return value + 1;
}

pub fn exported(value: i32): i32 {
    return helper(value);
}
```

也支持相对路径导入：

```java
import "../some_file.miko" as local;
```

包目录放在 `packages/` 下。例如：

```text
packages/acme/src/math.miko
```

可以这样使用：

```java
import acme.math as pkg;

fn main() {
    print(pkg.triple(4));
}
```

模块导入会在语义分析前重写为内部唯一函数名，因此不同模块可以拥有同名私有 helper，外部只能通过别名访问 `pub fn`。

后续标准库可以继续增加：

```java
assert(condition)
panic(message)
```

标准库建议分层：

```text
core    不依赖操作系统，适合嵌入式
std     依赖文件、进程、时间等 OS 能力
embed   针对 bare-metal / RTOS / ESP32 的适配层
```

目录：

```text
stdlib/
  core/
  std/
  embed/
```

## 17. 错误诊断设计

错误信息要从第一天就认真设计。

当前诊断已经输出文件位置、源码行和箭头：

```text
error: undefined variable 'name'
 --> examples/hello.miko:3:11
  |
3 |     print(name);
  |           ^
```

编译器内部应保存：

- 文件名
- 行号
- 列号
- token 长度
- 错误码
- 错误消息
- 可选修复建议

错误分类：

```text
E1xxx lexer errors
E2xxx parser errors
E3xxx semantic errors
E4xxx type errors
E5xxx codegen errors
E6xxx driver/linker errors
```

## 18. 测试策略

测试分三类：

### 18.1 单元测试

测试 lexer、parser、类型推断。

示例：

```text
tests/lexer/basic_tokens.miko
tests/parser/function_decl.miko
tests/sema/type_inference.miko
```

### 18.2 快照测试

输入 miko 源码，检查输出 AST / IR 是否符合预期。

```powershell
miko tests/parser/function_decl.miko --debug-ast
```

### 18.3 端到端测试

输入源码，编译运行，检查输出。

```text
input:  examples/hello.miko
expect: 30
```

## 19. 实现顺序建议

建议按下面顺序推进：

1. 创建 `CMakeLists.txt`、`src/main.cpp`、`examples/hello.miko`。
2. 在 CLion 中配置 `MinGW miko` Toolchain。
3. 在 CLion 中配置 `Debug-MinGW` CMake Profile。
4. 点击 CMake Reload，确认能生成 `miko` target。
5. 创建 `miko version` Run Configuration，确认能运行 `miko --version`。
6. 接入 LLVM，确认 CMake 能找到 `LLVMConfig.cmake`。
7. 在 `src/main.cpp` 中生成一个简单 LLVM module，确认 LLVM 链接正常。
8. 创建 `emit ir` Run Configuration。
9. 实现基础命令行参数。
10. 实现 lexer。
11. 增加 `--debug-tokens`。
12. 创建 `debug tokens` Run Configuration，并在 CLion 中断点调试 lexer。
13. 实现 AST 数据结构。
14. 实现 parser。
15. 增加 `--debug-ast`。
16. 创建 `debug ast` Run Configuration，并在 CLion 中断点调试 parser。
17. 实现符号表。
18. 实现基础类型系统。
19. 实现 `let` 类型推断。
20. 实现函数声明和函数调用检查。
21. 实现 LLVM IR 生成。
22. 增加 `--emit-ir`。
23. 在 CLion 中打开生成的 `.ll` 文件检查 IR。
24. 实现 runtime 的 `print`。
25. 生成目标文件。
26. 链接 runtime 生成可执行文件。
27. 加入 CTest，并让 CLion 能运行测试。
28. 增加 `if` / `else`。
29. 增加 `while`。
30. 增加跨平台目标 triple。
31. 增加嵌入式 runtime 适配。

每一步都应该有一个可以运行的例子，不要让工程长期处于不可运行状态。

## 20. 第一个里程碑

第一个里程碑定义为：

```java
fn main() {
    let a = 10;
    let b = 20;
    print(a + b);
}
```

可以执行：

```powershell
miko examples/hello.miko --compile --emit-ir -o hello.ll
miko examples/hello.miko --compile -o hello.exe
.\hello.exe
```

并输出：

```text
30
```

完成这个里程碑后，miko 就已经具备完整的编译器闭环：

```text
源码 -> token -> AST -> 类型检查 -> LLVM IR -> 可执行文件
```

## 21. 第二个里程碑

第二个里程碑加入控制流：

```java
fn main() {
    let i = 0;
    while i < 5 {
        if i == 3 {
            print("three");
        } else {
            print(i);
        }
        i = i + 1;
    }
}
```

需要完成：

- `if / else`
- `while`
- 变量赋值
- 字符串打印
- bool 表达式
- 基础块作用域

## 22. 第三个里程碑

第三个里程碑开始面向真实项目：

- 多文件编译。
- `import` 模块系统。
- 简单标准库。
- 静态库输出。
- 交叉编译输出 `.o`。
- ARM / RISC-V 示例。
- ESP32 集成方案验证。

示例：

```java
import core.math as math;

fn main() {
    let value = math.max_i32(10, 20);
    print(value);
}
```

## 23. 风险与关键决策

### 23.1 MinGW 与 LLVM ABI

Windows 下最大的风险是 LLVM 的构建工具链是否和 MinGW 匹配。如果 LLVM 是 MSVC 编译的，MinGW 链接 LLVM C++ 库可能失败。

建议尽早运行一个最小 CMake + LLVM 链接测试。

### 23.2 弱类型边界

如果弱类型设计太自由，编译器和运行时会迅速复杂化。建议第一版只做“宽松隐式转换”，不要做动态类型盒子。

### 23.3 嵌入式支持

嵌入式支持不只是目标代码生成，还涉及：

- runtime 大小
- 内存分配策略
- libc 可用性
- 链接脚本
- 启动文件
- 中断和外设接口

因此第一阶段只承诺输出目标文件或 LLVM IR，最终固件链接交给平台 SDK。

### 23.4 标准库范围

标准库越早膨胀，编译器越难稳定。先保持 `print`、`assert`、基础数学函数即可。

## 24. 推荐下一步

现在最应该做的是搭建能被 CLion 完整识别的最小工程骨架：

```text
1. CMakeLists.txt
2. src/main.cpp
3. include/miko/...
4. examples/hello.miko
```

然后先在 CLion 里完成：

```text
1. 配置 Toolchain: MinGW miko
2. 配置 CMake Profile: Debug-MinGW
3. CMake options 加入 -DLLVM_DIR=C:/Users/Administrator/env/llvm/lib/cmake/llvm
4. Reload CMake Project
5. Build target: miko
6. Run Configuration: miko version
7. Debug Configuration: debug tokens
```

同时，CLion Terminal 里也应该能跑通：

```powershell
$env:PATH="C:\mingw64\bin;C:\Users\Administrator\env\llvm\bin;$env:PATH"
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DMIKO_ENABLE_LLVM=OFF `
  -DLLVM_DIR="C:\Users\Administrator\env\llvm\lib\cmake\llvm"
cmake --build build
.\build\miko.exe --version
.\build\miko.exe examples\hello.miko --debug-tokens
.\build\miko.exe examples\hello.miko --debug-ast
.\build\miko.exe examples\hello.miko --debug-types
.\build\miko.exe examples\hello.miko --run
.\build\miko.exe examples\control.miko --run
.\build\miko.exe examples\functions.miko --run
ctest --test-dir build --output-on-failure
```

等空编译器能在 CLion 中运行和断点调试后，再开始写 lexer。这样每次扩展语言功能时，都有一个可靠的 IDE、构建和调试基线。

## 25. 长期愿景

miko 可以逐步发展成：

- 面向系统和嵌入式的小型静态语言。
- 比 C 更舒适，拥有类型推断和更好的错误提示。
- 比 C++ 更轻量，避免复杂模板和大型运行时。
- 可以生成 LLVM IR、对象文件、静态库和可执行文件。
- 可以和 C ABI 互操作。
- 可以在桌面、服务器和微控制器平台之间共享一部分核心代码。

最终理想效果：

```java
extern fn gpio_write(pin: i32, value: bool);

fn main() {
    let led = 2;
    while true {
        gpio_write(led, true);
        delay_ms(500);
        gpio_write(led, false);
        delay_ms(500);
    }
}
```

同一门语言，既能写桌面命令行工具，也能写嵌入式逻辑。这就是 miko 的方向。
