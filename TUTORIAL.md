# miko 语言教程

本文是一份面向 miko 使用者的入门教程，内容按当前编译器实现编写。它覆盖快速开始、第一个程序、编译运行、命令行选项、基本语法、数据类型、函数、控制流、运算符、注释、调试输出和当前限制。

## 1. 快速开始

miko 是一门轻量级静态语言，语法接近 C / Java，并支持局部变量类型推断。当前编译器可以：

- 读取 `.miko` 源码文件
- 词法分析：`--debug-tokens`
- 语法树输出：`--debug-ast`
- 类型分析输出：`--debug-types`
- 解释执行：`--run`
- 在启用 LLVM 后编译生成 Windows `.exe`：`--compile -o output.exe`

### 1.1 构建解释执行版本

默认构建不启用 LLVM，适合调试语言前端和解释执行：

```powershell
cmake -S . -B build
cmake --build build
```

验证：

```powershell
.\build\miko.exe --version
```

如果输出类似下面内容，说明构建成功：

```text
miko 0.1.0 (LLVM disabled)
```

### 1.2 构建 LLVM 原生编译版本

Windows 下当前 LLVM 包是 MSVC 工具链构建的，所以启用 LLVM 时请使用 Visual Studio 生成器：

```powershell
cmake -S . -B build-vs `
  -G "Visual Studio 18 2026" `
  -A x64 `
  -DMIKO_ENABLE_LLVM=ON `
  -DLLVM_DIR="C:\Users\Administrator\env\llvm\lib\cmake\llvm"

cmake --build build-vs --config Release
```

验证：

```powershell
.\build-vs\Release\miko.exe --version
```

预期输出包含：

```text
LLVM enabled
```

## 2. 第一个程序

创建文件 `hello.miko`：

```miko
fn main() {
    let a = 10;
    let b = 20;
    print(a + b);
}
```

这个程序做了三件事：

- 定义入口函数 `main`
- 用 `let` 声明两个局部变量
- 调用内置函数 `print` 输出表达式结果

解释执行：

```powershell
.\build\miko.exe hello.miko --run
```

或使用 LLVM 版本：

```powershell
.\build-vs\Release\miko.exe hello.miko --run
```

输出：

```text
30
```

## 3. 编译成可执行文件

启用 LLVM 构建后，可以把 `.miko` 文件编译成 `.exe`：

```powershell
.\build-vs\Release\miko.exe hello.miko --compile -o hello.exe
.\hello.exe
```

输出：

```text
30
```

也可以直接编译项目自带示例：

```powershell
.\build-vs\Release\miko.exe examples\hello.miko --compile -o hello.exe
.\hello.exe
```

当前原生编译已经支持：

- `main` 入口函数
- 局部变量
- `const` 常量
- 函数定义和调用
- `if / else`
- `while`
- `for`
- `break / continue`
- `return`
- 固定长度数组、下标读取和元素赋值
- 整数、浮点数、布尔值、字符、字符串字面量
- `print`、`len`、`input`
- 字符串打印、字符串长度、字符串比较、`string + string` 拼接

## 4. 编译器命令行选项

基本格式：

```text
miko <input.miko> [options]
```

### 4.1 通用选项

```text
--help              打印帮助
--version           打印版本
--run               用解释器运行程序
--debug-tokens      输出词法 token
--debug-ast         输出 AST
--debug-types       输出类型分析结果
```

示例：

```powershell
.\build\miko.exe examples\hello.miko --debug-tokens
.\build\miko.exe examples\hello.miko --debug-ast
.\build\miko.exe examples\hello.miko --debug-types
.\build\miko.exe examples\hello.miko --run
```

### 4.2 LLVM 编译选项

这些选项需要 `MIKO_ENABLE_LLVM=ON`：

```text
--compile           编译成本地可执行文件
-o <path>           输出文件路径
--target <triple>   指定目标 triple
--opt <0|1|2|3>     优化级别
--emit-ir           输出 LLVM IR 文件
--emit-obj          输出目标文件
--emit-asm          输出汇编文件
--keep-ir           保留中间 LLVM IR 文件
--clang <path>      指定 clang 路径，默认使用 PATH 中的 clang
```

编译示例：

```powershell
.\build-vs\Release\miko.exe examples\hello.miko --compile -o hello.exe
```

输出 LLVM IR：

```powershell
.\build-vs\Release\miko.exe examples\hello.miko --compile --emit-ir -o hello.ll
```

输出目标文件：

```powershell
.\build-vs\Release\miko.exe examples\hello.miko --compile --emit-obj -o hello.obj
```

输出汇编：

```powershell
.\build-vs\Release\miko.exe examples\hello.miko --compile --emit-asm -o hello.s
```

保留中间 IR：

```powershell
.\build-vs\Release\miko.exe examples\hello.miko --compile --keep-ir -o hello.exe
```

指定 clang：

```powershell
.\build-vs\Release\miko.exe examples\hello.miko --compile --clang "C:\Program Files\LLVM\bin\clang.exe" -o hello.exe
```

## 5. 程序结构

miko 程序由一个或多个函数组成：

```miko
fn add(a: i32, b: i32): i32 {
    return a + b;
}

fn main() {
    let result = add(10, 20);
    print(result);
}
```

当前程序入口必须是：

```miko
fn main() {
    // ...
}
```

原生编译时，`main` 当前不能带参数。`main` 可以省略返回类型；编译器会把它作为可执行文件入口处理。

## 6. 注释

支持单行注释：

```miko
// 这是一行注释
fn main() {
    print(123); // 行尾注释
}
```

支持多行注释：

```miko
/*
   这是多行注释
*/
fn main() {
    print("hello");
}
```

## 7. 变量

使用 `let` 声明局部变量。

### 7.1 自动类型推断

```miko
fn main() {
    let a = 10;
    let b = 3.14;
    let ok = true;
    let name = "miko";
}
```

编译器会推断：

```text
a: i32
b: f64
ok: bool
name: string
```

可以用下面命令查看：

```powershell
.\build\miko.exe examples\hello.miko --debug-types
```

### 7.2 显式类型

```miko
fn main() {
    let count: i64 = 100;
    let ratio: f32 = 0.5;
    let message: string = "hello";
}
```

如果初始化表达式可以隐式转换到声明类型，语义分析会通过；否则会报类型错误。

### 7.3 赋值

变量默认可重新赋值：

```miko
fn main() {
    let i = 0;
    i = i + 1;
    print(i);
}
```

赋值语句必须以分号结尾。

### 7.4 常量

使用 `const` 声明不可重新赋值的局部常量：

```miko
fn main() {
    const answer = 42;
    print(answer);
}
```

`const` 必须带初始化表达式。对 `const` 变量或 `const` 数组元素赋值会在语义分析阶段报错。

## 8. 数据类型

当前 miko 支持这些基础类型：

```text
i32      32 位整数，也是整数字面量默认类型
i64      64 位整数
f32      32 位浮点数
f64      64 位浮点数，也是浮点字面量默认类型
bool     布尔值
string   字符串
void     无返回值
```

### 8.1 整数

```miko
let a = 10;
let b: i64 = 100000;
```

当前整数字面量按十进制解析。

### 8.2 浮点数

```miko
let pi = 3.14;
let half: f32 = 0.5;
```

当前浮点字面量需要小数点后有数字，例如 `1.0`。

### 8.3 布尔值

```miko
let ok = true;
let failed = false;
```

布尔值可以参与逻辑运算：

```miko
let result = ok && failed == false;
```

也支持一元逻辑非：

```miko
let result = !failed;
```

### 8.4 字符串

```miko
let text = "hello";
print(text);
```

字符串支持常见转义：

```miko
let line = "hello\nworld";
let quote = "say \"hi\"";
let path = "C:\\tmp";
```

字符串支持 `string + string` 拼接，解释器和 LLVM 原生编译都支持：

```miko
fn main() {
    let name = "miko";
    print("hello " + name);
}
```

字符串还支持长度和比较：

```miko
fn main() {
    let text = "hello";
    print(len(text));
    print(text == "hello");
    print(text < "world");
}
```

当前语义分析要求字符串拼接两边都是 `string`。例如 `"age=" + 18` 暂不支持。

### 8.5 数组

miko 当前支持固定长度数组。可以用数组字面量推断类型：

```miko
fn main() {
    let nums = [10, 20, 30];
    print(nums[0]);
    print(len(nums));
}
```

也可以显式声明数组类型，语法是 `[元素类型; 长度]`：

```miko
fn main() {
    let nums: [i32; 4] = [10, 20, 30, 40];
    nums[1] = 99;
    print(nums[1]);
}
```

解释器会检查数组下标越界。当前 LLVM 原生编译还没有数组越界检查，越界访问属于未定义行为。

### 8.6 结构体

miko 支持 `struct` 声明、结构体字面量、字段访问，以及把结构体作为函数参数或返回值：

```miko
struct Point {
    x: i32;
    y: i32;
}

fn sum(point: Point): i32 {
    return point.x + point.y;
}

fn main() {
    let point: Point = Point { x: 10, y: 20 };
    print(point.x);
    print(point.y);
    print(sum(point));
}
```

结构体字面量必须初始化所有字段，字段名写错、字段缺失、字段类型不兼容都会在语义分析阶段报错。当前还不支持字段赋值，例如 `point.x = 1;`。

## 9. 隐式类型转换

miko 当前允许一些宽松的隐式转换：

```text
bool -> i32
i32  -> i64
i32  -> f32
i32  -> f64
i64  -> f64
f32  -> f64
number -> bool
```

示例：

```miko
fn main() {
    let a: i64 = 10;
    let b: f64 = 20;
    let ok: bool = 1;
    print(a);
    print(b);
    print(ok);
}
```

不建议依赖过多隐式转换。写公共函数参数时，推荐显式标注类型。

## 10. 运算符

### 10.1 算术运算

```text
+   加法，也可用于 string + string 拼接
-   减法
*   乘法
/   除法
%   取模
```

示例：

```miko
fn main() {
    let a = 10;
    let b = 3;
    print(a + b);
    print(a - b);
    print(a * b);
    print(a / b);
    print(a % b);
}
```

### 10.2 比较运算

```text
==   等于
!=   不等于
<    小于
<=   小于等于
>    大于
>=   大于等于
```

示例：

```miko
fn main() {
    let age = 18;
    print(age >= 18);
}
```

### 10.3 逻辑运算

```text
&&   逻辑与
||   逻辑或
```

示例：

```miko
fn main() {
    let age = 20;
    let active = true;
    if age >= 18 && active {
        print("ok");
    }
}
```

### 10.4 运算符优先级

从低到高：

```text
|| 
&&
== !=
< <= > >=
+ -
* / %
函数调用和括号表达式
```

可以用括号改变优先级：

```miko
let value = (1 + 2) * 3;
```

## 11. 条件语句

`if` 支持无括号条件：

```miko
fn main() {
    let score = 80;
    if score >= 60 {
        print("pass");
    } else {
        print("fail");
    }
}
```

也支持带括号：

```miko
fn main() {
    let score = 80;
    if (score >= 60) {
        print("pass");
    }
}
```

当前 `else if` 没有单独语法，可以用嵌套 `if`：

```miko
fn main() {
    let score = 75;
    if score >= 90 {
        print("A");
    } else {
        if score >= 60 {
            print("pass");
        } else {
            print("fail");
        }
    }
}
```

## 12. 循环

当前支持 `while`：

```miko
fn main() {
    let i = 0;
    while i < 5 {
        print(i);
        i = i + 1;
    }
}
```

也支持带括号条件：

```miko
while (i < 5) {
    print(i);
    i = i + 1;
}
```

`break` 可以退出循环，`continue` 可以跳过当前迭代：

```miko
fn main() {
    let i = 0;
    while i < 5 {
        i = i + 1;
        if i == 3 {
            continue;
        }
        if i == 5 {
            break;
        }
        print(i);
    }
}
```

也支持 C 风格 `for`：

```miko
fn main() {
    for let i = 0; i < 3; i = i + 1 {
        print(i);
    }
}
```

## 13. 函数

函数使用 `fn` 定义。

### 13.1 无返回值函数

```miko
fn sayHello() {
    print("hello");
}

fn main() {
    sayHello();
}
```

省略返回类型表示 `void`。

### 13.2 有返回值函数

```miko
fn add(a: i32, b: i32): i32 {
    return a + b;
}

fn main() {
    let result = add(10, 20);
    print(result);
}
```

返回类型也可以使用箭头写法：

```miko
fn add(a: i32, b: i32) -> i32 {
    return a + b;
}
```

### 13.3 参数

函数参数必须写类型：

```miko
fn max(a: i32, b: i32): i32 {
    if a > b {
        return a;
    }
    return b;
}
```

当前不支持默认参数、可变参数、函数重载。

## 14. 内置函数

当前内置函数：

```text
print(value)
len(value)
input()
```

`print` 接受一个参数，并输出一行。

示例：

```miko
fn main() {
    print(123);
    print(3.14);
    print(true);
    print("hello");
}
```

`len` 支持字符串和数组，返回 `i64`：

```miko
fn main() {
    print(len("miko"));
    print(len([1, 2, 3]));
}
```

`input` 从标准输入读取一行并返回字符串：

```miko
fn main() {
    print("name?");
    let name = input();
    print("hello " + name);
}
```

## 15. 模块和标准库

miko 支持 `import` 导入标准库模块：

```miko
import core.math as math;
import core.string;

fn main() {
    print(math.max_i32(10, 3));
    print(string.string_concat("hello ", "miko"));
}
```

标准库文件位于 `stdlib/` 目录。`import core.math;` 会解析到：

```text
stdlib/core/math.miko
```

当前已提供：

```text
core.math    max_i32, min_i32, abs_i32, clamp_i32
core.string  string_len, string_eq, string_concat
core.io      read_line, print_line
```

导入模块默认使用最后一段作为命名空间别名，例如 `import core.string;` 后通过 `string.string_len("miko")` 调用。也可以显式写别名：

```miko
import core.math as m;

fn main() {
    print(m.clamp_i32(42, 0, 9));
}
```

模块中的函数默认私有，只有 `pub fn` 会导出给其他文件：

```miko
fn helper(value: i32): i32 {
    return value + 1;
}

pub fn exported(value: i32): i32 {
    return helper(value);
}
```

也支持导入相对路径：

```miko
import "../some_file.miko" as local;
```

包放在 `packages/` 目录下。`import acme.math as pkg;` 会依次查找：

```text
packages/acme/src/math.miko
packages/acme/math.miko
```

如果只写 `import acme;`，会查找 `packages/acme/src/main.miko` 或 `packages/acme/main.miko`。编译器会把命名空间调用重写为内部唯一符号，所以不同模块里的私有函数名可以重复。

## 16. 作用域

函数体和 `{}` 块会形成作用域：

```miko
fn main() {
    let x = 10;
    if x > 0 {
        let y = 20;
        print(y);
    }
    print(x);
}
```

`y` 只在 `if` 的块中有效。离开块后不能再使用。

当前语义分析会检查：

- 变量是否已声明
- 同一作用域内是否重复声明
- 函数是否存在
- 函数参数个数是否匹配
- 返回值类型是否兼容
- 赋值类型是否兼容

## 17. 调试编译过程

### 17.1 查看 token

```powershell
.\build\miko.exe examples\hello.miko --debug-tokens
```

可以看到源码被词法分析成的 token。

### 17.2 查看 AST

```powershell
.\build\miko.exe examples\hello.miko --debug-ast
```

示例输出大致类似：

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

### 17.3 查看类型分析

```powershell
.\build\miko.exe examples\functions.miko --debug-types
```

可以看到每个函数、参数、变量和表达式推断出的类型。

## 18. 完整示例

### 18.1 函数示例

```miko
fn add(a: i32, b: i32): i32 {
    return a + b;
}

fn main() {
    let result = add(10, 20);
    print(result);
}
```

运行：

```powershell
.\build\miko.exe examples\functions.miko --run
```

输出：

```text
30
```

### 18.2 控制流示例

```miko
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

输出：

```text
0
1
2
three
4
```

### 18.3 类型转换示例

```miko
fn main() {
    let a: i64 = 10;
    let b: f64 = a + 2;
    let ok: bool = b;
    print(a);
    print(b);
    print(ok);
}
```

## 19. 当前已识别但未完整实现的语法

词法分析器已经识别这些关键字：

```text
extern
enum
```

但当前解析器、语义分析和代码生成还没有完整实现它们。因此现在不要在实际程序中使用这些语法。`import`、`pub`、`as` 和 `struct` 已经可用。

当前也还没有实现：

- 枚举
- `else if` 专门语法
- 函数重载
- 泛型
- 类
- 闭包
- 异常

## 20. 当前原生编译限制

`--compile` 当前通过 LLVM 生成 IR，然后调用 clang 链接成可执行文件。

已经验证可编译：

- `examples/hello.miko`
- `examples/functions.miko`
- `examples/control.miko`
- `examples/arrays.miko`
- `examples/array_assign.miko`
- `examples/strings_runtime.miko`
- `examples/input.miko`
- `examples/structs.miko`

当前原生编译限制：

- 需要 LLVM 构建版本：`MIKO_ENABLE_LLVM=ON`
- Windows 下需要 MSVC 生成器构建 miko 编译器
- 需要系统中能找到 clang，或用 `--clang <path>` 指定
- 字符串拼接当前支持 `string + string`，暂不支持字符串和数字自动拼接
- `input()` 单次最多读取 4096 字节
- LLVM 原生编译当前还没有数组越界检查
- `--emit-ir / --emit-obj / --emit-asm` 已支持真实输出，三个选项一次只能选一个

## 21. 推荐编码风格

推荐：

```miko
fn add(a: i32, b: i32): i32 {
    return a + b;
}

fn main() {
    let result = add(10, 20);
    if result > 10 {
        print(result);
    }
}
```

风格建议：

- 函数名和变量名使用小写驼峰或小写加下划线
- 每条语句以 `;` 结尾
- `if` 和 `while` 条件可以不写括号
- 公共函数参数和返回值显式标注类型
- 局部变量优先使用类型推断
- 用 `--debug-types` 检查推断是否符合预期

## 22. 常见问题

### 22.1 no compilation stage selected

原因：没有指定执行阶段。

错误示例：

```powershell
.\build\miko.exe hello.miko
```

正确写法：

```powershell
.\build\miko.exe hello.miko --run
```

或：

```powershell
.\build-vs\Release\miko.exe hello.miko --compile -o hello.exe
```

### 22.2 --compile requires LLVM support

原因：当前 `miko.exe` 没有启用 LLVM。

解决：按第 1.2 节重新构建 LLVM 版本。

### 22.3 Windows 下 LLVM 链接失败

原因通常是工具链不兼容。例如用 MinGW 去链接 MSVC 构建的 LLVM。

解决：使用 Visual Studio 生成器：

```powershell
cmake -S . -B build-vs `
  -G "Visual Studio 18 2026" `
  -A x64 `
  -DMIKO_ENABLE_LLVM=ON `
  -DLLVM_DIR="C:\Users\Administrator\env\llvm\lib\cmake\llvm"
```

### 22.4 生成 exe 时找不到 clang

解决方案一：确保 clang 在 `PATH` 中。

解决方案二：显式传入 clang 路径：

```powershell
.\build-vs\Release\miko.exe hello.miko --compile --clang "C:\Program Files\LLVM\bin\clang.exe" -o hello.exe
```

## 23. 学习路线

建议按这个顺序学习和使用 miko：

1. 写 `fn main()` 和 `print`
2. 学会 `let` 和类型推断
3. 学会 `i32 / i64 / f32 / f64 / bool / string`
4. 学会算术、比较和逻辑运算
5. 学会 `if / else`
6. 学会 `while`
7. 学会定义函数和返回值
8. 使用 `--debug-ast` 看语法树
9. 使用 `--debug-types` 看类型推断
10. 使用 LLVM 版本 `--compile -o` 生成 exe

完成这些内容后，你就能写出当前 miko 支持范围内的完整小程序。
