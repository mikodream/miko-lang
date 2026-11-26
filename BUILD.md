# miko 编译和运行教程

本文说明如何从源码编译 miko 编译器，并使用生成的 `miko.exe` 处理 `.miko` 程序文件。

## 1. 环境要求

需要先安装：

- CMake 3.20 或更高版本
- 支持 C++17 的 C++ 编译器
- Windows 下推荐使用 MinGW、MSVC 或 Clang

可以先在项目根目录检查 CMake 是否可用：

```powershell
cmake --version
```

项目根目录示例：

```powershell
cd C:\Users\Administrator\project\mikolang
```

## 2. 推荐构建方式

当前项目默认不启用 LLVM，适合先快速构建一个可用的 `miko.exe`。

在项目根目录执行：

```powershell
cmake -S . -B build
cmake --build build
```

构建成功后会生成：

```powershell
.\build\miko.exe
```

验证编译器是否能运行：

```powershell
.\build\miko.exe --version
```

如果看到类似输出，说明编译器已经生成成功：

```text
miko 0.1.0 (LLVM disabled)
```

## 3. 运行示例程序

项目自带示例文件在 `examples` 目录下，例如：

```text
examples\hello.miko
examples\control.miko
examples\functions.miko
```

运行 `hello.miko`：

```powershell
.\build\miko.exe examples\hello.miko --run
```

预期输出：

```text
30
```

继续运行其他示例：

```powershell
.\build\miko.exe examples\control.miko --run
.\build\miko.exe examples\functions.miko --run
```

## 4. 运行自己的 .miko 文件

假设你的程序文件是：

```text
my_program.miko
```

可以这样运行：

```powershell
.\build\miko.exe my_program.miko --run
```

如果文件在其他目录，例如：

```text
examples\hello.miko
```

则执行：

```powershell
.\build\miko.exe examples\hello.miko --run
```

## 5. 查看编译器各阶段输出

当前 `miko.exe` 支持查看词法、语法树和类型分析结果。

查看词法 token：

```powershell
.\build\miko.exe examples\hello.miko --debug-tokens
```

查看 AST：

```powershell
.\build\miko.exe examples\hello.miko --debug-ast
```

查看类型分析：

```powershell
.\build\miko.exe examples\hello.miko --debug-types
```

解释执行程序：

```powershell
.\build\miko.exe examples\hello.miko --run
```

## 6. 运行测试

构建完成后可以运行项目测试：

```powershell
ctest --test-dir build --output-on-failure
```

如果所有测试通过，会看到类似：

```text
100% tests passed
```

## 7. 重新干净构建

如果构建目录状态混乱，可以删除旧的 `build` 目录后重新构建：

```powershell
Remove-Item -Recurse -Force build
cmake -S . -B build
cmake --build build
```

## 8. 启用 LLVM 构建并生成 exe

启用 LLVM 后，`miko.exe` 可以把 `.miko` 文件编译成本地可执行文件。

当前 Windows 环境里的 LLVM 是 MSVC 工具链构建的，所以要使用 Visual Studio 生成器。不要用默认 Ninja/MinGW 去链接这个 LLVM 包。

示例命令：

```powershell
cmake -S . -B build-vs `
  -G "Visual Studio 18 2026" `
  -A x64 `
  -DMIKO_ENABLE_LLVM=ON `
  -DLLVM_DIR="C:\Users\Administrator\env\llvm\lib\cmake\llvm"

cmake --build build-vs --config Release
```

生成的编译器在：

```powershell
.\build-vs\Release\miko.exe
```

验证：

```powershell
.\build-vs\Release\miko.exe --version
```

如果输出中包含 `LLVM enabled`，说明 LLVM 版本构建成功。

把 `.miko` 编译成 exe：

```powershell
.\build-vs\Release\miko.exe examples\hello.miko --compile -o .\build-vs\Release\hello.exe
.\build-vs\Release\hello.exe
```

预期输出：

```text
30
```

编译你自己的文件：

```powershell
.\build-vs\Release\miko.exe your_file.miko --compile -o your_program.exe
.\your_program.exe
```

输出 LLVM IR、目标文件或汇编：

```powershell
.\build-vs\Release\miko.exe examples\hello.miko --compile --emit-ir -o hello.ll
.\build-vs\Release\miko.exe examples\hello.miko --compile --emit-obj -o hello.obj
.\build-vs\Release\miko.exe examples\hello.miko --compile --emit-asm -o hello.s
```

`--emit-ir`、`--emit-obj`、`--emit-asm` 一次只能选择一个。`--emit-obj` 和 `--emit-asm` 会先生成临时 LLVM IR，再调用 clang 输出目标格式；如果需要保留中间 IR，可以加 `--keep-ir`。

## 9. 当前能力说明

目前 `miko.exe` 已支持：

- 读取 `.miko` 源码文件
- 词法分析：`--debug-tokens`
- 语法分析：`--debug-ast`
- 类型分析：`--debug-types`
- 解释执行：`--run`
- LLVM 构建下生成本地 `.exe`：`--compile -o output.exe`
- LLVM 构建下输出 IR、目标文件、汇编：`--compile --emit-ir/--emit-obj/--emit-asm`
- 固定长度数组、数组下标访问和元素赋值
- 字符串拼接、字符串长度、字符串比较
- 简单标准输入输出：`print`、`input`
- 标准库源码模块、命名空间调用、`import ... as ...` 别名
- `pub fn` 可见性控制
- `packages/<name>/src` 形式的本地包导入
- `struct` 声明、结构体字面量、字段访问、结构体参数和返回值

默认非 LLVM 构建只能解释执行：

```powershell
.\build\miko.exe your_file.miko --run
```

如果直接执行：

```powershell
.\build\miko.exe your_file.miko
```

会看到：

```text
error: no compilation stage selected yet. Try --compile, --run, --debug-tokens, --debug-ast, or --debug-types.
```

这是正常的，因为当前必须显式指定一个阶段。

## 10. 常见问题

### 提示 no compilation stage selected

原因是没有指定执行阶段。

改成：

```powershell
.\build\miko.exe your_file.miko --run
```

或者使用调试阶段：

```powershell
.\build\miko.exe your_file.miko --debug-ast
```

### 找不到 LLVMConfig.cmake

如果只是想先运行当前编译器，不需要启用 LLVM，直接使用默认构建：

```powershell
cmake -S . -B build
cmake --build build
```

如果确实要启用 LLVM，需要指定正确的 `LLVM_DIR`：

```powershell
cmake -S . -B build-llvm `
  -DMIKO_ENABLE_LLVM=ON `
  -DLLVM_DIR="C:\Users\Administrator\env\llvm\lib\cmake\llvm"
```

### LLVM 版本链接失败或 CMake 报 MSVC generator

如果启用 LLVM 后出现链接错误，通常是 LLVM 和当前 C++ 编译器工具链不兼容。当前环境里的 LLVM 是 MSVC 工具链构建的，所以 Windows 下要用 Visual Studio 生成器：

```powershell
cmake -S . -B build-vs `
  -G "Visual Studio 18 2026" `
  -A x64 `
  -DMIKO_ENABLE_LLVM=ON `
  -DLLVM_DIR="C:\Users\Administrator\env\llvm\lib\cmake\llvm"
```

用 Ninja 或 MinGW 生成器去链接这个 LLVM 包，会出现 `crtdbg.h not found`、`could not open 'kernel32.lib'` 这类错误——那是工具链不匹配，不是代码问题。

## 11. 自举链：用 miko 编译 miko

### 三个二进制，各管什么

项目里有三个能跑的工具，别混用：

| 二进制 | 用什么写的 | 能做什么 | 什么时候用 |
|---|---|---|---|
| `build-vs-llvm\Release\miko.exe` | C++（stage0） | 解释执行、调试输出、驱动 miko 程序 | 只用来**引导**和 `--run` |
| `dist\miko.exe` | **miko** | 编译 miko → 可执行文件、自编译 | 日常编译用它 |
| `dist\miko_gen2.exe` | miko（由 miko 自己编译出来） | 同上 | 只在验证不动点时用 |

一句话规则：

- **要跑程序看结果** → `build-vs-llvm\Release\miko.exe prog.miko --run`（只有 C++ 那版带解释器）
- **要把 miko 编译成可执行文件** → `dist\miko.exe prog.miko out\p`
- **要重新构建 `dist\miko.exe`** → 用 `build-vs-llvm\Release\miko.exe` 走一次引导（下面有命令）

C++ 那版只在**第一次**需要：它得先把 miko 写的编译器解释一遍，产出第一份 `.ll`。之后再用 miko 写的 `miko.exe` 就完全不需要它了。

### 构建一个 miko 写的 miko.exe

```powershell
cmake -DMIKO_EXE=build-vs-llvm/Release/miko.exe `
      -DCLANG_EXE="C:/Program Files/LLVM/bin/clang.exe" `
      -DSOURCE_DIR=. `
      -DOUTPUT_DIR=dist `
      -P cmake/BuildMikoExe.cmake
```

它做三代：

```text
gen1: stage0(C++) 解释 compiler/main_ll.miko  → gen1.ll → clang → dist/miko.exe
gen2: dist/miko.exe 编译整个编译器            → gen2.ll
gen3: 用 gen2.ll 重建的 miko.exe 再编译一次   → gen3.ll
断言 gen2.ll == gen3.ll（当前 867382 字节）
```

最后还会验证 `dist\miko.exe --version` 报告 miko 实现、能编译 `examples/arrays_ir.miko`、且输出与解释器逐字节相同。

### dist\miko.exe 的用法

编译是**一步**：给源码，拿到可执行文件。

```powershell
.\dist\miko.exe prog.miko out\p        # → out\p.exe（可直接运行）
.\out\p.exe
```

同时会在旁边留下中间产物 `out\p.ll`（IR）和 `out\p.sema`（语义检查结果），方便排查。输出目录不存在会自动创建。

其他模式：

```powershell
.\dist\miko.exe --version              # 版本
.\dist\miko.exe --help                 # 用法
.\dist\miko.exe --self out\c           # 编译整个编译器 → out\c.ll
.\dist\miko.exe --emit-ir prog.miko out\p   # 只出 .ll，不链接
```

必须在**仓库根目录**执行：编译器模块按 `compiler/<name>.miko` 解析，链接要用到 `runtime/miko_runtime.c`。

### 链接是怎么做的

编译到可执行文件需要 clang，而 clang 需要 C 运行时的头文件和库搜索路径（运行时是 C 写的，`<stdio.h>` 得能找到）。这些路径因机器而异，不属于源码。

构建时会把它们写成一个链接脚本，并记录位置：

```text
dist/miko.toolchain   → 一行，指向 dist/miko-link.bat
dist/miko-link.bat    → clang 路径 + 全部 -I / -Xlinker 参数，已正确加引号
```

`miko.exe` 运行时读这个文件，只往脚本传两个路径参数。它**不自己拼命令行**——路径里带空格时拼接极易出错，而且 cmd 对"以引号开头的命令"有自己的剥引号规则，会把参数打散。

查找顺序：当前目录的 `miko.toolchain` → `dist/` → `build/`。当前目录优先，所以可以不重新编译就切换工具链。找不到就退回 `clang`，这在开发者命令提示符里是可用的。

仓库里的编译器本身是用 miko 写的，而且有**两条独立的链**：

| 链 | 后端 | 中间产物 | 汇编/链接 |
|---|---|---|---|
| C 链 | `compiler/codegen_c.miko` | 每个模块一个 `.c` | gcc / clang |
| LLVM 链 | `compiler/codegen_ll.miko` | **一个** `.ll`（整程序） | clang |

两条链共享同一个前端（token / ast / symbols / type / lexer / parser / sema），只是后端不同，各自独立闭环。

### 跑 C 链

```powershell
cd build-vs-llvm
ctest -C Release -R miko_bootstrap_stage0_stage1_stage2_smoke --output-on-failure
```

它做的事：stage0 把 `compiler/*.miko` 逐个译成 C → 链接成 stage1 → stage1 再译一遍得到 stage2 → stage2 再译得到 stage3，然后断言 **stage2 的 C == stage3 的 C**（字节相同）。

### 跑 LLVM 链

```powershell
cd build-vs-llvm
ctest -C Release -R miko_bootstrap_llvm_self_hosting --output-on-failure
```

也可以脱离 ctest 直接跑脚本：

```powershell
cmake -DMIKO_EXE=build-vs-llvm/Release/miko.exe `
      -DCLANG_EXE="C:/Program Files/LLVM/bin/clang.exe" `
      -DSOURCE_DIR=. `
      -DBOOTSTRAP_OUT_DIR=out/llvm-chain `
      -P cmake/BootstrapLLVM.cmake
```

它做的事：stage0 解释 `compiler/main_ll.miko` 产出整程序 `.ll` → clang 汇编 + 链接 `runtime/miko_runtime.c` 得到 `stage1_ll.exe` → 它再编译整个编译器得到 stage2 IR → 再来一次得到 stage3 IR，然后断言 **stage1 IR == stage2 IR == stage3 IR**（三代全部字节相同）。

### 为什么断言字节相同而不是"能跑"

"能跑"只说明这一代产物正确。字节相同说明后端是自己输出的**不动点**：经过 LLVM 往返没有信息损失、没有代际漂移。这才是自举。

### 自己写一个 miko 文件来编译编译器

可以。`compiler/build_compiler.miko` 就是这么干的，它是一段普通 miko 程序，不依赖任何特殊构建钩子：

```powershell
# 1. 用 stage0 解释执行这个 miko 程序，产出整程序 IR
.\build-vs-llvm\Release\miko.exe compiler\build_compiler.miko --run

# 2. 把 IR 链接成编译器可执行文件
clang out\compiler_from_miko.ll runtime\miko_runtime.c -o out\compiler_from_miko.exe

# 3. 用它编译普通程序
.\out\compiler_from_miko.exe examples\arrays_ir.miko out\demo
clang out\demo.ll runtime\miko_runtime.c -o out\demo.exe
.\out\demo.exe
```

它做的四件事和 `main_ll.miko` 完全一样：读全部模块 → lex → parse → 用 `Module <name>` 拼成一个程序 → sema → `codegen_ll.generate_ir` 出 `.ll`。你可以复制它，改模块列表、改输出路径，或者只编译编译器的一个子集来调试。

`cmake/DriverBuildsCompiler.cmake` 把这条路径自动验证：跑这个 miko 程序 → 链接 → 用它编译 `examples/arrays_ir.miko` → 再和解释器的输出逐字节对比。

### 把普通程序编成 IR

`main_ll.miko` 同时是普通 IR 编译器：

```powershell
.\build-vs-llvm\Release\miko.exe compiler/main_ll.miko --run -- examples\structs.miko out\demo
# 生成 out\demo.ll，然后：
clang out\demo.ll runtime\miko_runtime.c -o out\demo.exe
.\out\demo.exe
```

第一个参数是模块子集（逗号分隔，调试用），第二个起才是"源码 + 输出前缀"。不带参数时编译整个编译器。

## 12. 三段式语义一致性

同一个程序在三种执行方式下必须给出相同输出：

```powershell
# 1. 解释执行
.\build-vs-llvm\Release\miko.exe examples\short_circuit.miko --run

# 2. 经 C 链
.\build-vs-llvm\Release\miko.exe compiler/main.miko --run -- examples\short_circuit.miko out\sc_c
clang out\sc_c.c runtime/miko_runtime.c -o out\sc_c.exe; .\out\sc_c.exe

# 3. 经 LLVM IR 链
.\build-vs-llvm\Release\miko.exe compiler/main_ll.miko --run -- examples\short_circuit.miko out\sc
clang out\sc.ll runtime/miko_runtime.c -o out\sc.exe; .\out\sc.exe
```

三者都应输出 `4 4 18 4`。这个例子专门覆盖 `&&` 短路与多行字符串——两处曾经在后端与解释器之间出现语义分歧的地方。

## 13. 两条前端与 IR 后端的支持边界

写代码前需要知道的一件事：项目里有两个前端，能力不完全一样。

- **stage0 前端**：`miko.exe` 自己用 C++ 写的词法/语法/语义分析。`miko xxx.miko --run` 走这里。
- **miko 前端**：`compiler/lexer.miko`、`parser.miko`、`sema.miko`，用 miko 写的。`compiler/main_ll.miko` 走这里。

它们对少数构造的判断不同，所以同一个文件可能在一个前端下能跑、另一个下报语法错。

| 构造 | stage0 前端（解释器） | miko 前端 + IR 后端 |
|---|---|---|
| 函数、if/else、while、for、break/continue | ✅ | ✅ |
| enum | ✅ | ✅ |
| struct 字面量、字段读 | ✅ | ✅ |
| 定长数组字面量、运行时下标读 | ✅ | ✅ |
| 定长数组下标写 `a[i] = v` | ✅ | ✅ |
| `len(数组)` | ✅ | ✅ |
| 字符串、char、`&&` / `\|\|` 短路 | ✅ | ✅ |
| `file_exists` | ✅ | ✅ |
| `run_command` | ✅ | ✅ |
| `else if` | ❌ 语法报错 | ✅ 正确 |
| struct 字段赋值 `p.x = 42` | ❌ 语法报错 | ✅ 正确 |
| `vec<T>` | ✅ | ❌ 明确报错（见下） |

两处"解释器报错但 IR 链正确"的构造属于 **miko 前端比 stage0 前端更宽松**。这意味着你不能拿 `--run` 的结果当成这些语法的判据——它压根解析不了。想验证请直接走 IR 链。

### vec 为什么不行

`runtime/miko_runtime.c` 里没有任何 `vec` 支持函数——没有分配、没有扩容、没有按下标存取。IR 后端因此无法为 `vec_new` / `vec_push` 生成正确的调用。

这种情况下后端**明确报错并退出**，而不是生成能跑但算错的代码：

```text
codegen_ll: `len` applied to a value of type 'i64', which this backend cannot lower
codegen_ll: vec<T> is not supported by the LLVM IR backend (no runtime support yet)
```

编译器自身不用 vec，所以自举链不受影响。

### 三条判定原则

改这个后端时守住这三条，否则会出现最难查的 bug：

1. **宁可报错，不要静默降级。** 数组字面量和 `a[i] = v` 曾经都退化成标量 0 而不报错——IR 合法、能汇编、能运行，只是算出来的数是错的。这类 bug 不会出现在任何日志里。
2. **后端与解释器输出必须逐字节相同。** `cmake/DriverBuildsCompiler.cmake` 就是这么做的：编出来的编译器跑出的输出，和解释器的输出做字符串比较。
3. **改完必须重跑自举不动点。** 后端源码一变，IR 就变，三代字节相同这个断言必须重新成立。

