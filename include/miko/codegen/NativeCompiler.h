#pragma once

#include "miko/ast/Ast.h"

#include <string>
#include <vector>

namespace miko {

struct NativeCompileOptions {
    std::string inputPath;
    std::string outputPath;
    std::string targetTriple;
    std::string clangPath = "clang";
    bool keepIr = false;
    bool emitIr = false;
    bool emitObj = false;
    bool emitBc = false;
    bool emitAsm = false;
    int optLevel = 0;
};

struct NativeCompileResult {
    bool success = false;
    std::vector<std::string> errors;
    std::string irPath;
    std::string objPath;
    std::string asmPath;
    std::string bcPath;
};

NativeCompileResult compileNativeExecutable(const Program& program, const NativeCompileOptions& options);

} // namespace miko
