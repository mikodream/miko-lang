#include "miko/codegen/NativeCompiler.h"

#if MIKO_ENABLE_LLVM

#include "miko/Builtins.h"
#include "miko/StringLiterals.h"
#include "miko/NumericLiterals.h"
#include "miko/sema/Type.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/OptimizationLevel.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace miko {

namespace {

struct ValueInfo {
    llvm::Value* value = nullptr;
    Type type = Type::unknown();
    bool isAddress = false;
};

struct VariableInfo {
    llvm::AllocaInst* alloca = nullptr;
    Type type = Type::unknown();
    bool isConst = false;
};

struct FunctionInfo {
    llvm::Function* function = nullptr;
    Type returnType = Type::voidType();
    std::vector<Type> parameterTypes;
    bool isEntryMain = false;
};

struct LoopTargets {
    llvm::BasicBlock* continueTarget = nullptr;
    llvm::BasicBlock* breakTarget = nullptr;
};

struct StructInfo {
    llvm::StructType* type = nullptr;
    std::vector<Parameter> fields;
    std::unordered_map<std::string, std::size_t> fieldIndices;
};

struct EnumInfo {
    std::vector<std::string> members;
    std::unordered_map<std::string, std::size_t> memberIndices;
};

std::string unquoteString(const std::string& value) {
    return decodeStringLiteral(value);
}

// Integer literal decoding lives in miko/NumericLiterals.h so the interpreter
// and this backend cannot disagree about what a literal means.
bool integerLiteralFitsI32(long long value) {
    return value >= -2147483648LL && value <= 2147483647LL;
}

char unquoteChar(const std::string& value) {
    return decodeCharLiteral(value);
}

class LlvmNativeCompiler {
public:
    explicit LlvmNativeCompiler(const NativeCompileOptions& options)
        : options_(options),
          context_(),
          module_(std::make_unique<llvm::Module>(moduleName(), context_)),
          builder_(context_) {
        const std::string triple = options_.targetTriple.empty() ? llvm::sys::getDefaultTargetTriple() : options_.targetTriple;
        module_->setTargetTriple(llvm::Triple(triple));
    }

    NativeCompileResult compile(const Program& program) {
        result_.success = false;

        try {
            declareRuntime();
            declareStructs(program);
            declareEnums(program);
            declareFunctions(program);
            defineFunctions(program);
            verifyModule();
            optimizeModule();
            writeIr();

            if (options_.emitIr) {
                result_.success = true;
                return result_;
            }

            if (options_.emitObj || options_.emitAsm) {
                if (!emitNativeArtifact()) {
                    return result_;
                }
                cleanupIntermediateIr();
                result_.success = true;
                return result_;
            }

            if (!linkExecutable()) {
                return result_;
            }

            cleanupIntermediateIr();
            result_.success = true;
        } catch (const std::exception& error) {
            report(error.what());
        }

        return result_;
    }

private:
    std::string moduleName() const {
        if (!options_.inputPath.empty()) {
            return std::filesystem::path(options_.inputPath).filename().string();
        }
        return "miko_module";
    }

    llvm::Type* llvmType(Type type) {
        if (type.isVec()) {
            return llvm::PointerType::get(context_, 0);
        }
        if (type.isArray()) {
            return llvm::ArrayType::get(llvmType(type.elementType()), type.arrayLength());
        }
        if (type.kind() == TypeKind::Struct) {
            const auto found = structs_.find(type.structName());
            if (found == structs_.end()) {
                throw std::runtime_error("unknown struct type '" + type.structName() + "' in native codegen");
            }
            return found->second.type;
        }

        switch (type.kind()) {
        case TypeKind::Void:
            return llvm::Type::getVoidTy(context_);
        case TypeKind::Byte:
            return llvm::Type::getInt8Ty(context_);
        case TypeKind::Char:
            return llvm::Type::getInt32Ty(context_);
        case TypeKind::I32:
            return llvm::Type::getInt32Ty(context_);
        case TypeKind::I64:
            return llvm::Type::getInt64Ty(context_);
        case TypeKind::F32:
            return llvm::Type::getFloatTy(context_);
        case TypeKind::F64:
            return llvm::Type::getDoubleTy(context_);
        case TypeKind::Bool:
            return llvm::Type::getInt1Ty(context_);
        case TypeKind::String:
            return llvm::PointerType::get(context_, 0);
        case TypeKind::Enum:
            return llvm::Type::getInt32Ty(context_);
        case TypeKind::Vec:
            return llvm::PointerType::get(context_, 0);
        case TypeKind::Struct:
            break;
        case TypeKind::Unknown:
            break;
        }

        throw std::runtime_error("cannot lower unknown type to LLVM");
    }

    llvm::Constant* defaultValue(Type type) {
        if (type.isVoid()) {
            return nullptr;
        }
        if (type.isVec()) {
            return llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0));
        }
        if (type.isArray()) {
            return llvm::ConstantAggregateZero::get(llvmType(type));
        }
        if (type.kind() == TypeKind::Struct) {
            return llvm::ConstantAggregateZero::get(llvmType(type));
        }
        if (type.kind() == TypeKind::Bool) {
            return llvm::ConstantInt::getFalse(context_);
        }
        if (type.kind() == TypeKind::Byte || type.kind() == TypeKind::Char) {
            return llvm::ConstantInt::get(llvmType(type), 0, false);
        }
        if (type.isInteger()) {
            return llvm::ConstantInt::get(llvmType(type), 0, true);
        }
        if (type.isFloat()) {
            return llvm::ConstantFP::get(llvmType(type), 0.0);
        }
        if (type.kind() == TypeKind::String) {
            return createManagedString("");
        }
        throw std::runtime_error("cannot create default value for type '" + type.name() + "'");
    }

    void declareRuntime() {
        auto* int32Type = llvm::Type::getInt32Ty(context_);
        auto* int64Type = llvm::Type::getInt64Ty(context_);
        auto* ptrType = llvm::PointerType::get(context_, 0);
        auto* printfType = llvm::FunctionType::get(int32Type, {ptrType}, true);
        printf_ = llvm::Function::Create(printfType, llvm::Function::ExternalLinkage, "printf", module_.get());

        // String services all live in the runtime now. They used to be inlined
        // here with raw malloc, but a miko string carries its length in a header
        // written by the runtime allocator (see MikoStringHeader in
        // runtime/miko_runtime.c); a string built any other way has no header,
        // so len() would read whatever sat in front of the bytes.
        auto* strlenType = llvm::FunctionType::get(int64Type, {ptrType}, false);
        strLen_ = llvm::Function::Create(strlenType, llvm::Function::ExternalLinkage, "miko_str_len", module_.get());

        auto* strConcatType = llvm::FunctionType::get(ptrType, {ptrType, ptrType}, false);
        strConcat_ = llvm::Function::Create(strConcatType, llvm::Function::ExternalLinkage, "miko_str_concat", module_.get());

        auto* strAppendType = llvm::FunctionType::get(ptrType, {ptrType, ptrType}, false);
        strAppend_ = llvm::Function::Create(strAppendType, llvm::Function::ExternalLinkage, "miko_str_append", module_.get());

        auto* charAtType = llvm::FunctionType::get(int32Type, {ptrType, int64Type}, false);
        charAt_ = llvm::Function::Create(charAtType, llvm::Function::ExternalLinkage, "miko_char_at", module_.get());

        auto* substringType = llvm::FunctionType::get(ptrType, {ptrType, int64Type, int64Type}, false);
        substring_ = llvm::Function::Create(substringType, llvm::Function::ExternalLinkage, "miko_substring", module_.get());

        auto* toStringI64Type = llvm::FunctionType::get(ptrType, {int64Type}, false);
        toStringI64_ = llvm::Function::Create(toStringI64Type, llvm::Function::ExternalLinkage, "miko_to_string_i64", module_.get());

        auto* toStringF64Type = llvm::FunctionType::get(ptrType, {llvm::Type::getDoubleTy(context_)}, false);
        toStringF64_ = llvm::Function::Create(toStringF64Type, llvm::Function::ExternalLinkage, "miko_to_string_f64", module_.get());

        auto* readFileType = llvm::FunctionType::get(ptrType, {ptrType}, false);
        readFile_ = llvm::Function::Create(readFileType, llvm::Function::ExternalLinkage, "miko_read_file", module_.get());

        // write_file goes through the runtime for the same reason read_file does,
        // and for one more: the runtime creates missing parent directories, while
        // the inlined fopen/fwrite this replaces did not. That difference was
        // invisible until the compiler was run from a directory with no out/, which
        // is exactly what a release package is - `miko --self` there failed with
        // "failed to open file for writing" instead of writing the file.
        auto* writeFileType = llvm::FunctionType::get(llvm::Type::getVoidTy(context_), {ptrType, ptrType}, false);
        writeFile_ = llvm::Function::Create(writeFileType, llvm::Function::ExternalLinkage, "miko_write_file", module_.get());

        auto* argAtType = llvm::FunctionType::get(ptrType, {int64Type}, false);
        argAt_ = llvm::Function::Create(argAtType, llvm::Function::ExternalLinkage, "miko_arg_at", module_.get());

        auto* strcmpType = llvm::FunctionType::get(int32Type, {ptrType, ptrType}, false);
        // miko_str_cmp, not strcmp: a string may hold NUL bytes now that its
        // length is stored explicitly, and strcmp would stop at the first one.
        strcmp_ = llvm::Function::Create(strcmpType, llvm::Function::ExternalLinkage, "miko_str_cmp", module_.get());

        // `run_command` shells out through the runtime (miko_run_command), so a
        // compiled miko program can drive a toolchain the same way the
        // interpreter does, and the shell dependency stays in one place.
        auto* runCommandType = llvm::FunctionType::get(int32Type, {ptrType}, false);
        runCommand_ = llvm::Function::Create(runCommandType, llvm::Function::ExternalLinkage, "miko_run_command", module_.get());

        auto* mallocType = llvm::FunctionType::get(ptrType, {int64Type}, false);
        malloc_ = llvm::Function::Create(mallocType, llvm::Function::ExternalLinkage, "malloc", module_.get());

        auto* reallocType = llvm::FunctionType::get(ptrType, {ptrType, int64Type}, false);
        realloc_ = llvm::Function::Create(reallocType, llvm::Function::ExternalLinkage, "realloc", module_.get());

        auto* memcpyType = llvm::FunctionType::get(ptrType, {ptrType, ptrType, int64Type}, false);
        memcpy_ = llvm::Function::Create(memcpyType, llvm::Function::ExternalLinkage, "memcpy", module_.get());

        // stderr output, stdin, and fatal diagnostics all go through the C
        // runtime, so the emitted IR names no platform-specific symbols.
        auto* eprintStrType = llvm::FunctionType::get(llvm::Type::getVoidTy(context_), {ptrType}, false);
        eprintStr_ = llvm::Function::Create(eprintStrType, llvm::Function::ExternalLinkage, "miko_eprint_str", module_.get());

        auto* inputType = llvm::FunctionType::get(ptrType, {}, false);
        input_ = llvm::Function::Create(inputType, llvm::Function::ExternalLinkage, "miko_input", module_.get());

        auto* runtimeErrorType = llvm::FunctionType::get(llvm::Type::getVoidTy(context_), {ptrType}, false);
        runtimeError_ = llvm::Function::Create(runtimeErrorType, llvm::Function::ExternalLinkage, "miko_runtime_error", module_.get());

        auto* fopenType = llvm::FunctionType::get(ptrType, {ptrType, ptrType}, false);
        fopen_ = llvm::Function::Create(fopenType, llvm::Function::ExternalLinkage, "fopen", module_.get());

        auto* fgetcType = llvm::FunctionType::get(int32Type, {ptrType}, false);
        fgetc_ = llvm::Function::Create(fgetcType, llvm::Function::ExternalLinkage, "fgetc", module_.get());

        auto* fwriteType = llvm::FunctionType::get(int64Type, {ptrType, int64Type, int64Type, ptrType}, false);
        fwrite_ = llvm::Function::Create(fwriteType, llvm::Function::ExternalLinkage, "fwrite", module_.get());

        auto* fcloseType = llvm::FunctionType::get(int32Type, {ptrType}, false);
        fclose_ = llvm::Function::Create(fcloseType, llvm::Function::ExternalLinkage, "fclose", module_.get());

        auto* exitType = llvm::FunctionType::get(llvm::Type::getVoidTy(context_), {int32Type}, false);
        exit_ = llvm::Function::Create(exitType, llvm::Function::ExternalLinkage, "exit", module_.get());
        exit_->setDoesNotReturn();

        auto* snprintfType = llvm::FunctionType::get(int32Type, {ptrType, llvm::Type::getInt64Ty(context_), ptrType}, true);
        snprintf_ = llvm::Function::Create(snprintfType, llvm::Function::ExternalLinkage, "snprintf", module_.get());

        auto* fprintfType = llvm::FunctionType::get(int32Type, {ptrType, ptrType}, true);
        fprintf_ = llvm::Function::Create(fprintfType, llvm::Function::ExternalLinkage, "fprintf", module_.get());

        auto* strtollType = llvm::FunctionType::get(int64Type, {ptrType, llvm::PointerType::get(context_, 0), int32Type}, false);
        strtoll_ = llvm::Function::Create(strtollType, llvm::Function::ExternalLinkage, "strtoll", module_.get());

        auto* strtodType = llvm::FunctionType::get(llvm::Type::getDoubleTy(context_), {ptrType, llvm::PointerType::get(context_, 0)}, false);
        strtod_ = llvm::Function::Create(strtodType, llvm::Function::ExternalLinkage, "strtod", module_.get());

        // Length-aware search in the runtime, not strstr: a string may hold NUL
        // bytes, and strstr stops at the first one in either argument.
        auto* strIndexOfType = llvm::FunctionType::get(int64Type, {ptrType, ptrType}, false);
        strIndexOf_ = llvm::Function::Create(strIndexOfType, llvm::Function::ExternalLinkage, "miko_str_index_of", module_.get());

        auto* strContainsType = llvm::FunctionType::get(int32Type, {ptrType, ptrType}, false);
        strContains_ = llvm::Function::Create(strContainsType, llvm::Function::ExternalLinkage, "miko_str_contains", module_.get());

        auto* clockMsType = llvm::FunctionType::get(int64Type, {}, false);
        clockMs_ = llvm::Function::Create(clockMsType, llvm::Function::ExternalLinkage, "miko_clock_ms", module_.get());
        monoMs_ = llvm::Function::Create(clockMsType, llvm::Function::ExternalLinkage, "miko_mono_ms", module_.get());

        rngState_ = new llvm::GlobalVariable(
            *module_,
            int64Type,
            false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(int64Type, 0x9E3779B97F4A7C15LL, true),
            "__miko_rng_state");

        programArgc_ = new llvm::GlobalVariable(
            *module_,
            int32Type,
            false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(int32Type, 0, true),
            "__miko_argc");
        programArgv_ = new llvm::GlobalVariable(
            *module_,
            ptrType,
            false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(ptrType),
            "__miko_argv");
    }

    Type resolveType(Type type) const {
        if (type.kind() == TypeKind::Struct && structs_.find(type.structName()) == structs_.end()
            && enums_.find(type.structName()) != enums_.end()) {
            return Type::enumOf(type.structName());
        }
        if (type.isArray()) {
            return Type::arrayOf(resolveType(type.elementType()), type.arrayLength());
        }
        if (type.isVec()) {
            return Type::vecOf(resolveType(type.elementType()));
        }
        return type;
    }

    Type typeFromName(const std::string& name) const {
        return resolveType(Type::fromName(name));
    }

    Type functionReturnType(const FunctionDecl& function) const {
        return function.returnType().empty() ? Type::voidType() : typeFromName(function.returnType());
    }

    Type parameterType(const Parameter& parameter) const {
        return typeFromName(parameter.type());
    }

    void declareStructs(const Program& program) {
        for (const auto& structPtr : program.structs()) {
            const StructDecl& structure = *structPtr;
            if (structs_.find(structure.name()) != structs_.end()) {
                throw std::runtime_error("struct '" + structure.name() + "' is already declared");
            }

            StructInfo info;
            info.type = llvm::StructType::create(context_, "miko." + structure.name());
            info.fields = structure.fields();
            for (std::size_t i = 0; i < info.fields.size(); ++i) {
                info.fieldIndices[info.fields[i].name()] = i;
            }
            structs_.emplace(structure.name(), std::move(info));
        }

        for (const auto& structPtr : program.structs()) {
            const StructDecl& structure = *structPtr;
            StructInfo& info = structs_.at(structure.name());
            std::vector<llvm::Type*> fields;
            fields.reserve(structure.fields().size());
            for (const Parameter& field : structure.fields()) {
                fields.push_back(llvmType(typeFromName(field.type())));
            }
            info.type->setBody(fields, false);
        }
    }

    void declareEnums(const Program& program) {
        for (const auto& enumPtr : program.enums()) {
            const EnumDecl& enumeration = *enumPtr;
            if (enums_.find(enumeration.name()) != enums_.end()) {
                throw std::runtime_error("enum '" + enumeration.name() + "' is already declared");
            }

            EnumInfo info;
            info.members = enumeration.members();
            for (std::size_t i = 0; i < info.members.size(); ++i) {
                info.memberIndices[info.members[i]] = i;
            }
            enums_.emplace(enumeration.name(), std::move(info));
        }
    }

    void declareFunctions(const Program& program) {
        for (const auto& functionPtr : program.functions()) {
            const FunctionDecl& function = *functionPtr;
            if (isBuiltinFunctionName(function.name())) {
                throw std::runtime_error("built-in function '" + function.name() + "' cannot be redefined");
            }

            Type sourceReturnType = functionReturnType(function);
            Type loweredReturnType = sourceReturnType;
            const bool isEntryMain = isEntryFunctionName(function.name());

            if (isEntryMain) {
                if (!function.parameters().empty()) {
                    throw std::runtime_error("native executable entry function 'main' cannot have parameters yet");
                }
                loweredReturnType = Type(TypeKind::I32);
            }

            std::vector<llvm::Type*> llvmParameters;
            std::vector<Type> parameterTypes;
            if (isEntryMain) {
                llvmParameters.push_back(llvm::Type::getInt32Ty(context_));
                llvmParameters.push_back(llvm::PointerType::get(context_, 0));
            }
            for (const Parameter& parameter : function.parameters()) {
                Type type = parameterType(parameter);
                parameterTypes.push_back(type);
                llvmParameters.push_back(llvmType(type));
            }

            auto* functionType = llvm::FunctionType::get(llvmType(loweredReturnType), llvmParameters, false);
            const std::string llvmFunctionName = canonicalEntryFunctionName(function.name());
            auto* llvmFunction = llvm::Function::Create(functionType, llvm::Function::ExternalLinkage, llvmFunctionName, module_.get());

            std::size_t index = 0;
            for (auto& argument : llvmFunction->args()) {
                if (isEntryMain) {
                    argument.setName(index++ == 0 ? "argc" : "argv");
                } else {
                    argument.setName(function.parameters()[index++].name());
                }
            }

            functions_.emplace(function.name(), FunctionInfo{llvmFunction, sourceReturnType, std::move(parameterTypes), isEntryMain});
        }
    }

    void defineFunctions(const Program& program) {
        for (const auto& functionPtr : program.functions()) {
            defineFunction(*functionPtr);
        }
    }

    void defineFunction(const FunctionDecl& function) {
        const auto found = functions_.find(function.name());
        if (found == functions_.end()) {
            throw std::runtime_error("internal error: function was not declared: " + function.name());
        }

        currentFunction_ = &found->second;
        scopes_.clear();

        llvm::Function* llvmFunction = currentFunction_->function;
        auto* entry = llvm::BasicBlock::Create(context_, "entry", llvmFunction);
        builder_.SetInsertPoint(entry);
        pushScope();

        if (currentFunction_->isEntryMain) {
            auto argument = llvmFunction->args().begin();
            builder_.CreateStore(&*argument, programArgc_);
            ++argument;
            builder_.CreateStore(&*argument, programArgv_);
            // Hand argc/argv to the runtime as well: arg_at is a runtime call
            // now, because an argv entry has no length header and the runtime
            // has to copy it into a managed string. The self-hosted backend
            // emits the same call.
            auto* int32Type = llvm::Type::getInt32Ty(context_);
            auto* ptrType = llvm::PointerType::get(context_, 0);
            auto* setArgsType = llvm::FunctionType::get(llvm::Type::getVoidTy(context_), {int32Type, ptrType}, false);
            llvm::Function* setArgs = llvm::Function::Create(
                setArgsType, llvm::Function::ExternalLinkage, "miko_set_args", module_.get());
            builder_.CreateCall(setArgs, {builder_.CreateLoad(int32Type, programArgc_, "argc.for.runtime"),
                                          builder_.CreateLoad(ptrType, programArgv_, "argv.for.runtime")});
        } else {
            std::size_t index = 0;
            for (auto& argument : llvmFunction->args()) {
                Type type = currentFunction_->parameterTypes[index];
                llvm::AllocaInst* alloca = createEntryAlloca(llvmFunction, argument.getName().str(), type);
                builder_.CreateStore(&argument, alloca);
                declareVariable(argument.getName().str(), alloca, type);
                ++index;
            }
        }

        emitBlock(function.body(), false);

        if (!builder_.GetInsertBlock()->getTerminator()) {
            emitDefaultReturn();
        }

        popScope();
        currentFunction_ = nullptr;
    }

    llvm::AllocaInst* createEntryAlloca(llvm::Function* function, const std::string& name, Type type) {
        llvm::IRBuilder<> temporaryBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
        return temporaryBuilder.CreateAlloca(llvmType(type), nullptr, name);
    }

    void emitDefaultReturn() {
        if (!currentFunction_) {
            throw std::runtime_error("internal error: no current function");
        }

        if (currentFunction_->isEntryMain) {
            builder_.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0, true));
            return;
        }

        if (currentFunction_->returnType.isVoid()) {
            builder_.CreateRetVoid();
            return;
        }

        builder_.CreateRet(defaultValue(currentFunction_->returnType));
    }

    void emitBlock(const BlockStmt& block, bool createScope = true) {
        if (createScope) {
            pushScope();
        }

        for (const auto& statement : block.statements()) {
            if (builder_.GetInsertBlock()->getTerminator()) {
                break;
            }
            emitStmt(*statement);
        }

        if (createScope) {
            popScope();
        }
    }

    void emitStmt(const Stmt& statement) {
        if (const auto* let = dynamic_cast<const LetStmt*>(&statement)) {
            Type type = let->explicitType().empty()
                ? (let->initializer() ? inferExprType(*let->initializer()) : Type::voidType())
                : typeFromName(let->explicitType());

            if (type.isVoid()) {
                throw std::runtime_error("variable '" + let->name() + "' cannot have type void");
            }

            llvm::AllocaInst* alloca = createEntryAlloca(builder_.GetInsertBlock()->getParent(), let->name(), type);
            if (let->initializer()) {
                ValueInfo value = emitExpr(*let->initializer());
                if (type.isArray() || type.kind() == TypeKind::Struct) {
                    copyAggregateValue(value, alloca, type);
                } else {
                    value = convertValue(value, type);
                    builder_.CreateStore(value.value, alloca);
                }
            } else if (type.isVec()) {
                builder_.CreateStore(emitVecNew(), alloca);
            } else {
                builder_.CreateStore(defaultValue(type), alloca);
            }
            declareVariable(let->name(), alloca, type, let->isConst());
            return;
        }

        if (const auto* assign = dynamic_cast<const AssignStmt*>(&statement)) {
            VariableInfo variable = lookupVariable(assign->name());
            if (variable.isConst) {
                throw std::runtime_error("cannot assign to const variable '" + assign->name() + "'");
            }
            ValueInfo value = emitExpr(assign->value());
            if (variable.type.isArray() || variable.type.kind() == TypeKind::Struct) {
                copyAggregateValue(value, variable.alloca, variable.type);
            } else {
                value = convertValue(value, variable.type);
                builder_.CreateStore(value.value, variable.alloca);
            }
            return;
        }

        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&statement)) {
            (void)emitExpr(exprStmt->expression());
            return;
        }

        if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&statement)) {
            emitReturn(*returnStmt);
            return;
        }

        if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
            emitBlock(*block);
            return;
        }

        if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
            emitIf(*ifStmt);
            return;
        }

        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
            emitWhile(*whileStmt);
            return;
        }

        if (const auto* assign = dynamic_cast<const IndexAssignStmt*>(&statement)) {
            VariableInfo variable = lookupVariable(assign->name());
            if (variable.isConst) {
                throw std::runtime_error("cannot assign to const array variable '" + assign->name() + "'");
            }
            if (variable.type.isVec()) {
                llvm::Value* header = builder_.CreateLoad(llvmType(variable.type), variable.alloca, "vec.header");
                llvm::Value* index = convertValue(emitExpr(assign->index()), Type(TypeKind::I64)).value;
                llvm::Value* length = vecFieldLoad(header, 0, "vec.len");
                llvm::Value* data = vecFieldLoad(header, 2, "vec.data");
                emitRuntimeCheck(vecIndexOk(index, length), "vec index out of bounds");
                llvm::Value* elementPtr = builder_.CreateInBoundsGEP(llvmType(variable.type.elementType()), data, index, "vec.elem");
                ValueInfo value = convertValue(emitExpr(assign->value()), variable.type.elementType());
                builder_.CreateStore(value.value, elementPtr);
                return;
            }
            if (!variable.type.isArray()) {
                throw std::runtime_error("cannot index-assign non-array variable '" + assign->name() + "'");
            }

            ValueInfo index = convertValue(emitExpr(assign->index()), Type(TypeKind::I64));
            emitArrayBoundsCheck(index.value, variable.type);
            llvm::Value* elementPtr = arrayElementPointer(variable.alloca, variable.type, index.value);
            ValueInfo value = convertValue(emitExpr(assign->value()), variable.type.elementType());
            builder_.CreateStore(value.value, elementPtr);
            return;
        }

        if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
            emitFor(*forStmt);
            return;
        }

        if (dynamic_cast<const BreakStmt*>(&statement)) {
            if (loopTargets_.empty()) {
                throw std::runtime_error("break can only be used inside a loop");
            }
            builder_.CreateBr(loopTargets_.back().breakTarget);
            return;
        }

        if (dynamic_cast<const ContinueStmt*>(&statement)) {
            if (loopTargets_.empty()) {
                throw std::runtime_error("continue can only be used inside a loop");
            }
            builder_.CreateBr(loopTargets_.back().continueTarget);
            return;
        }

        throw std::runtime_error("unknown statement node in LLVM codegen");
    }

    void emitReturn(const ReturnStmt& statement) {
        if (!currentFunction_) {
            throw std::runtime_error("internal error: return outside function");
        }

        if (currentFunction_->isEntryMain) {
            if (statement.value()) {
                builder_.CreateRet(convertValue(emitExpr(*statement.value()), Type(TypeKind::I32)).value);
            } else {
                builder_.CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0, true));
            }
            return;
        }

        if (currentFunction_->returnType.isVoid()) {
            builder_.CreateRetVoid();
            return;
        }

        if (!statement.value()) {
            throw std::runtime_error("non-void function must return a value");
        }

        ValueInfo value = emitExpr(*statement.value());
        if (currentFunction_->returnType.isArray() || currentFunction_->returnType.kind() == TypeKind::Struct) {
            if (value.type != currentFunction_->returnType) {
                throw std::runtime_error("cannot return '" + value.type.name() + "' from function returning '" + currentFunction_->returnType.name() + "'");
            }
            value = materializeAggregate(value);
            builder_.CreateRet(builder_.CreateLoad(llvmType(value.type), value.value, "aggregate.ret.load"));
            return;
        }

        builder_.CreateRet(convertValue(value, currentFunction_->returnType).value);
    }

    void emitIf(const IfStmt& statement) {
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        llvm::Value* condition = convertToBool(emitExpr(statement.condition())).value;

        auto* thenBlock = llvm::BasicBlock::Create(context_, "if.then", function);
        auto* elseBlock = llvm::BasicBlock::Create(context_, "if.else");
        auto* mergeBlock = llvm::BasicBlock::Create(context_, "if.end");

        builder_.CreateCondBr(condition, thenBlock, elseBlock);

        builder_.SetInsertPoint(thenBlock);
        emitBlock(statement.thenBlock());
        if (!builder_.GetInsertBlock()->getTerminator()) {
            builder_.CreateBr(mergeBlock);
        }

        function->insert(function->end(), elseBlock);
        builder_.SetInsertPoint(elseBlock);
        if (const BlockStmt* elseBody = statement.elseBlock()) {
            emitBlock(*elseBody);
        }
        if (!builder_.GetInsertBlock()->getTerminator()) {
            builder_.CreateBr(mergeBlock);
        }

        function->insert(function->end(), mergeBlock);
        builder_.SetInsertPoint(mergeBlock);
    }

    void emitWhile(const WhileStmt& statement) {
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        auto* conditionBlock = llvm::BasicBlock::Create(context_, "while.cond", function);
        auto* bodyBlock = llvm::BasicBlock::Create(context_, "while.body");
        auto* endBlock = llvm::BasicBlock::Create(context_, "while.end");

        builder_.CreateBr(conditionBlock);

        builder_.SetInsertPoint(conditionBlock);
        llvm::Value* condition = convertToBool(emitExpr(statement.condition())).value;
        builder_.CreateCondBr(condition, bodyBlock, endBlock);

        function->insert(function->end(), bodyBlock);
        builder_.SetInsertPoint(bodyBlock);
        loopTargets_.push_back(LoopTargets{conditionBlock, endBlock});
        emitBlock(statement.body());
        loopTargets_.pop_back();
        if (!builder_.GetInsertBlock()->getTerminator()) {
            builder_.CreateBr(conditionBlock);
        }

        function->insert(function->end(), endBlock);
        builder_.SetInsertPoint(endBlock);
    }

    void emitFor(const ForStmt& statement) {
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        pushScope();

        if (const Stmt* initializer = statement.initializer()) {
            emitStmt(*initializer);
        }

        auto* conditionBlock = llvm::BasicBlock::Create(context_, "for.cond", function);
        auto* bodyBlock = llvm::BasicBlock::Create(context_, "for.body");
        auto* incrementBlock = llvm::BasicBlock::Create(context_, "for.inc");
        auto* endBlock = llvm::BasicBlock::Create(context_, "for.end");

        builder_.CreateBr(conditionBlock);

        builder_.SetInsertPoint(conditionBlock);
        if (const Expr* conditionExpr = statement.condition()) {
            llvm::Value* condition = convertToBool(emitExpr(*conditionExpr)).value;
            builder_.CreateCondBr(condition, bodyBlock, endBlock);
        } else {
            builder_.CreateBr(bodyBlock);
        }

        function->insert(function->end(), bodyBlock);
        builder_.SetInsertPoint(bodyBlock);
        loopTargets_.push_back(LoopTargets{incrementBlock, endBlock});
        emitBlock(statement.body());
        loopTargets_.pop_back();
        if (!builder_.GetInsertBlock()->getTerminator()) {
            builder_.CreateBr(incrementBlock);
        }

        function->insert(function->end(), incrementBlock);
        builder_.SetInsertPoint(incrementBlock);
        if (const Stmt* increment = statement.increment()) {
            emitStmt(*increment);
        }
        if (!builder_.GetInsertBlock()->getTerminator()) {
            builder_.CreateBr(conditionBlock);
        }

        function->insert(function->end(), endBlock);
        builder_.SetInsertPoint(endBlock);
        popScope();
    }

    ValueInfo emitExpr(const Expr& expression) {
        if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expression)) {
            const long long value = parseIntegerLiteral(integer->value());
            if (integerLiteralFitsI32(value)) {
                return {llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), value, true), Type(TypeKind::I32)};
            }
            return {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), value, true), Type(TypeKind::I64)};
        }

        if (const auto* floating = dynamic_cast<const FloatExpr*>(&expression)) {
            return {llvm::ConstantFP::get(llvm::Type::getDoubleTy(context_), parseFloatLiteral(floating->value())), Type(TypeKind::F64)};
        }

        if (const auto* string = dynamic_cast<const StringExpr*>(&expression)) {
            return {createManagedString(unquoteString(string->value())), Type(TypeKind::String)};
        }

        if (const auto* character = dynamic_cast<const CharExpr*>(&expression)) {
            return {llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), static_cast<unsigned char>(unquoteChar(character->value())), false), Type(TypeKind::Char)};
        }

        if (const auto* boolean = dynamic_cast<const BoolExpr*>(&expression)) {
            return {llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), boolean->value()), Type(TypeKind::Bool)};
        }

        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
            return emitUnary(*unary);
        }

        if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
            const std::size_t separator = variable->name().find("::");
            if (separator != std::string::npos) {
                return emitEnumMember(variable->name());
            }
            VariableInfo info = lookupVariable(variable->name());
            if (info.type.isArray() || info.type.kind() == TypeKind::Struct) {
                return {info.alloca, info.type, true};
            }
            return {builder_.CreateLoad(llvmType(info.type), info.alloca, variable->name()), info.type};
        }

        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
            return emitBinary(*binary);
        }

        if (const auto* array = dynamic_cast<const ArrayExpr*>(&expression)) {
            return emitArray(*array);
        }

        if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
            return emitIndex(*index);
        }

        if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expression)) {
            return emitFieldAccess(*field);
        }

        if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expression)) {
            return emitStructLiteral(*literal);
        }

        if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
            return emitCall(*call);
        }

        throw std::runtime_error("unknown expression node in LLVM codegen");
    }

    ValueInfo emitArray(const ArrayExpr& expression) {
        Type type = inferExprType(expression);
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        llvm::AllocaInst* alloca = createEntryAlloca(function, "array.literal", type);

        llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0, true);
        for (std::size_t i = 0; i < expression.elements().size(); ++i) {
            llvm::Value* elementIndex = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), static_cast<long long>(i), true);
            llvm::Value* elementPtr = builder_.CreateInBoundsGEP(llvmType(type), alloca, {zero, elementIndex}, "array.elem");
            ValueInfo value = convertValue(emitExpr(*expression.elements()[i]), type.elementType());
            builder_.CreateStore(value.value, elementPtr);
        }

        return {alloca, type, true};
    }

    // Aggregate values flow through the compiler as addresses; a struct or
    // array rvalue (e.g. a function call result) is materialized into a
    // temporary alloca first.
    ValueInfo materializeAggregate(ValueInfo source) {
        if (source.isAddress) {
            return source;
        }
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        llvm::AllocaInst* slot = createEntryAlloca(function, "aggregate.rvalue", source.type);
        builder_.CreateStore(source.value, slot);
        return {slot, source.type, true};
    }

    void copyAggregateValue(ValueInfo source, llvm::Value* destination, Type destinationType) {
        if (source.type.kind() == TypeKind::Struct || destinationType.kind() == TypeKind::Struct) {
            if (source.type != destinationType) {
                throw std::runtime_error("cannot copy '" + source.type.name() + "' to '" + destinationType.name() + "'");
            }
            source = materializeAggregate(source);
            builder_.CreateMemCpy(destination, llvm::MaybeAlign(), source.value, llvm::MaybeAlign(), module_->getDataLayout().getTypeAllocSize(llvmType(destinationType)));
            return;
        }

        if (!source.type.isArray() || !destinationType.isArray()) {
            throw std::runtime_error("array copy requires array values");
        }
        if (source.type.arrayLength() != destinationType.arrayLength()) {
            throw std::runtime_error("cannot copy array of type '" + source.type.name() + "' to '" + destinationType.name() + "'");
        }

        if (source.type == destinationType) {
            source = materializeAggregate(source);
            builder_.CreateMemCpy(destination, llvm::MaybeAlign(), source.value, llvm::MaybeAlign(), module_->getDataLayout().getTypeAllocSize(llvmType(destinationType)));
            return;
        }

        for (std::size_t i = 0; i < destinationType.arrayLength(); ++i) {
            llvm::Value* elementIndex = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), static_cast<long long>(i), true);
            llvm::Value* sourcePtr = arrayElementPointer(source.value, source.type, elementIndex);
            llvm::Value* destinationPtr = arrayElementPointer(destination, destinationType, elementIndex);
            ValueInfo element{builder_.CreateLoad(llvmType(source.type.elementType()), sourcePtr, "array.copy.load"), source.type.elementType()};
            builder_.CreateStore(convertValue(element, destinationType.elementType()).value, destinationPtr);
        }
    }

    ValueInfo emitStructLiteral(const StructLiteralExpr& expression) {
        Type type = Type::structOf(expression.typeName());
        const StructInfo& info = lookupStructInfo(expression.typeName());
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        llvm::AllocaInst* alloca = createEntryAlloca(function, "struct.literal", type);
        builder_.CreateStore(defaultValue(type), alloca);

        for (const auto& field : expression.fields()) {
            const auto found = info.fieldIndices.find(field.first);
            if (found == info.fieldIndices.end()) {
                throw std::runtime_error("struct '" + expression.typeName() + "' has no field '" + field.first + "'");
            }
            Type fieldType = typeFromName(info.fields[found->second].type());
            llvm::Value* ptr = structFieldPointer(alloca, type, found->second);
            ValueInfo value = convertValue(emitExpr(*field.second), fieldType);
            if (fieldType.isArray() || fieldType.kind() == TypeKind::Struct) {
                copyAggregateValue(value, ptr, fieldType);
            } else {
                builder_.CreateStore(value.value, ptr);
            }
        }

        return {alloca, type, true};
    }

    ValueInfo emitFieldAccess(const FieldAccessExpr& expression) {
        ValueInfo target = emitExpr(expression.target());
        if (target.type.kind() != TypeKind::Struct || !target.isAddress) {
            throw std::runtime_error("cannot access field on non-struct value");
        }

        const StructInfo& info = lookupStructInfo(target.type.structName());
        const auto found = info.fieldIndices.find(expression.field());
        if (found == info.fieldIndices.end()) {
            throw std::runtime_error("struct '" + target.type.name() + "' has no field '" + expression.field() + "'");
        }

        Type fieldType = typeFromName(info.fields[found->second].type());
        llvm::Value* ptr = structFieldPointer(target.value, target.type, found->second);
        if (fieldType.isArray() || fieldType.kind() == TypeKind::Struct) {
            return {ptr, fieldType, true};
        }
        return {builder_.CreateLoad(llvmType(fieldType), ptr, "struct.field.load"), fieldType};
    }

    llvm::Value* structFieldPointer(llvm::Value* structAddress, Type structType, std::size_t fieldIndex) {
        llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0, true);
        llvm::Value* index = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), static_cast<long long>(fieldIndex), true);
        return builder_.CreateInBoundsGEP(llvmType(structType), structAddress, {zero, index}, "struct.field");
    }

    const StructInfo& lookupStructInfo(const std::string& name) const {
        const auto found = structs_.find(name);
        if (found == structs_.end()) {
            throw std::runtime_error("unknown struct type '" + name + "'");
        }
        return found->second;
    }

    ValueInfo emitIndex(const IndexExpr& expression) {
        ValueInfo target = emitExpr(expression.target());
        if (target.type.isVec()) {
            Type elementType = target.type.elementType();
            llvm::Value* index = convertValue(emitExpr(expression.index()), Type(TypeKind::I64)).value;
            llvm::Value* length = vecFieldLoad(target.value, 0, "vec.len");
            llvm::Value* data = vecFieldLoad(target.value, 2, "vec.data");
            emitRuntimeCheck(vecIndexOk(index, length), "vec index out of bounds");
            llvm::Value* elementPtr = builder_.CreateInBoundsGEP(llvmType(elementType), data, index, "vec.elem");
            if (elementType.kind() == TypeKind::Struct || elementType.isArray()) {
                return {elementPtr, elementType, true};
            }
            return {builder_.CreateLoad(llvmType(elementType), elementPtr, "vec.load"), elementType};
        }
        if (!target.type.isArray() || !target.isAddress) {
            throw std::runtime_error("cannot index non-array value");
        }

        ValueInfo index = convertValue(emitExpr(expression.index()), Type(TypeKind::I64));
        emitArrayBoundsCheck(index.value, target.type);
        llvm::Value* elementPtr = arrayElementPointer(target.value, target.type, index.value);
        Type elementType = target.type.elementType();
        if (elementType.kind() == TypeKind::Struct || elementType.isArray()) {
            return {elementPtr, elementType, true};
        }
        return {builder_.CreateLoad(llvmType(elementType), elementPtr, "array.load"), elementType};
    }

    // Bounds-check a dynamic index into a fixed-length array.
    //
    // Fixed arrays used to be indexed with no check at all, through an
    // `inbounds` GEP: out-of-range access was not even defined behaviour, so
    // `a[99]` on a [3]i32 could return 0, garbage, or be optimised away.
    void emitArrayBoundsCheck(llvm::Value* index, const Type& arrayType) {
        llvm::Value* length = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(context_),
            static_cast<long long>(arrayType.arrayLength()),
            true);
        emitRuntimeCheck(vecIndexOk(index, length), "array index out of bounds");
    }

    llvm::Value* arrayElementPointer(llvm::Value* arrayAddress, Type arrayType, llvm::Value* index) {
        llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0, true);
        return builder_.CreateGEP(llvmType(arrayType), arrayAddress, {zero, index}, "array.index");
    }

    llvm::StructType* vecHeaderType() {
        if (!vecHeaderType_) {
            vecHeaderType_ = llvm::StructType::create(context_, "miko.vec_header");
            vecHeaderType_->setBody({llvm::Type::getInt64Ty(context_), llvm::Type::getInt64Ty(context_),
                                     llvm::PointerType::get(context_, 0)}, false);
        }
        return vecHeaderType_;
    }

    llvm::Value* vecFieldPointer(llvm::Value* header, unsigned fieldIndex) {
        llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0, true);
        llvm::Value* index = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), fieldIndex, true);
        return builder_.CreateInBoundsGEP(vecHeaderType(), header, {zero, index}, "vec.field");
    }

    llvm::Value* vecFieldLoad(llvm::Value* header, unsigned fieldIndex, const std::string& name) {
        llvm::Type* field = nullptr;
        if (fieldIndex == 2) {
            field = llvm::PointerType::get(context_, 0);
        } else {
            field = llvm::Type::getInt64Ty(context_);
        }
        return builder_.CreateLoad(field, vecFieldPointer(header, fieldIndex), name);
    }

    void vecFieldStore(llvm::Value* header, unsigned fieldIndex, llvm::Value* value) {
        builder_.CreateStore(value, vecFieldPointer(header, fieldIndex));
    }

    llvm::Value* vecIndexOk(llvm::Value* index, llvm::Value* length) {
        auto* int64Type = llvm::Type::getInt64Ty(context_);
        llvm::Value* zero = llvm::ConstantInt::get(int64Type, 0, true);
        llvm::Value* lowerOk = builder_.CreateICmpSGE(index, zero, "vec.index.lower");
        llvm::Value* upperOk = builder_.CreateICmpSLT(index, length, "vec.index.upper");
        return builder_.CreateAnd(lowerOk, upperOk, "vec.index.ok");
    }

    uint64_t vecElementSize(Type vecType) {
        return module_->getDataLayout().getTypeAllocSize(llvmType(vecType.elementType()));
    }

    llvm::Value* emitVecNew() {
        auto* int64Type = llvm::Type::getInt64Ty(context_);
        const uint64_t headerSize = 16 + module_->getDataLayout().getPointerSize();
        llvm::Value* header = builder_.CreateCall(malloc_, {llvm::ConstantInt::get(int64Type, static_cast<long long>(headerSize), true)}, "vec.new.header");
        vecFieldStore(header, 0, llvm::ConstantInt::get(int64Type, 0, true));
        vecFieldStore(header, 1, llvm::ConstantInt::get(int64Type, 0, true));
        vecFieldStore(header, 2, llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)));
        return header;
    }

    void emitVecPush(llvm::Value* header, Type vecType, ValueInfo element) {
        auto* int64Type = llvm::Type::getInt64Ty(context_);
        auto* ptrType = llvm::PointerType::get(context_, 0);
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        auto* growBlock = llvm::BasicBlock::Create(context_, "vec.grow", function);
        auto* appendBlock = llvm::BasicBlock::Create(context_, "vec.append");
        auto* doneBlock = llvm::BasicBlock::Create(context_, "vec.done");
        llvm::BasicBlock* conditionBlock = builder_.GetInsertBlock();

        ValueInfo elementValue = convertValue(element, vecType.elementType());
        llvm::Value* length = vecFieldLoad(header, 0, "vec.push.len");
        llvm::Value* capacity = vecFieldLoad(header, 1, "vec.push.cap");
        llvm::Value* data = vecFieldLoad(header, 2, "vec.push.data");

        builder_.CreateCondBr(builder_.CreateICmpSLT(length, capacity, "vec.has.cap"), appendBlock, growBlock);

        builder_.SetInsertPoint(growBlock);
        llvm::Value* doubled = builder_.CreateMul(capacity, llvm::ConstantInt::get(int64Type, 2, true), "vec.cap.x2");
        llvm::Value* zeroCap = builder_.CreateICmpEQ(capacity, llvm::ConstantInt::get(int64Type, 0, true), "vec.cap.zero");
        llvm::Value* nextCapacity = builder_.CreateSelect(zeroCap, llvm::ConstantInt::get(int64Type, 4, true), doubled, "vec.cap.next");
        llvm::Value* byteCapacity = builder_.CreateMul(nextCapacity, llvm::ConstantInt::get(int64Type, static_cast<long long>(vecElementSize(vecType)), true), "vec.cap.bytes");
        llvm::Value* grownData = builder_.CreateCall(realloc_, {data, byteCapacity}, "vec.data.grown");
        emitRuntimeCheck(builder_.CreateICmpNE(grownData, llvm::ConstantPointerNull::get(ptrType), "vec.grow.ok"), "failed to grow vec");
        vecFieldStore(header, 1, nextCapacity);
        vecFieldStore(header, 2, grownData);
        llvm::BasicBlock* grownFrom = builder_.GetInsertBlock();
        builder_.CreateBr(appendBlock);

        function->insert(function->end(), appendBlock);
        builder_.SetInsertPoint(appendBlock);
        llvm::Value* effectiveData = builder_.CreatePHI(ptrType, 2, "vec.data.phi");
        static_cast<llvm::PHINode*>(effectiveData)->addIncoming(data, conditionBlock);
        static_cast<llvm::PHINode*>(effectiveData)->addIncoming(grownData, grownFrom);
        llvm::Value* elementPtr = builder_.CreateInBoundsGEP(llvmType(vecType.elementType()), effectiveData, length, "vec.push.slot");
        Type elementType = vecType.elementType();
        if (elementType.kind() == TypeKind::Struct || elementType.isArray()) {
            ValueInfo source = materializeAggregate(elementValue);
            builder_.CreateMemCpy(elementPtr, llvm::MaybeAlign(), source.value, llvm::MaybeAlign(), module_->getDataLayout().getTypeAllocSize(llvmType(elementType)));
        } else {
            builder_.CreateStore(elementValue.value, elementPtr);
        }
        vecFieldStore(header, 0, builder_.CreateAdd(length, llvm::ConstantInt::get(int64Type, 1, true), "vec.len.next"));
        builder_.CreateBr(doneBlock);

        function->insert(function->end(), doneBlock);
        builder_.SetInsertPoint(doneBlock);
    }

    ValueInfo emitVecPop(llvm::Value* header, Type vecType) {
        auto* int64Type = llvm::Type::getInt64Ty(context_);
        llvm::Value* length = vecFieldLoad(header, 0, "vec.pop.len");
        llvm::Value* data = vecFieldLoad(header, 2, "vec.pop.data");
        emitRuntimeCheck(builder_.CreateICmpSGT(length, llvm::ConstantInt::get(int64Type, 0, true), "vec.pop.nonempty"), "vec_pop from empty vec");
        llvm::Value* lastIndex = builder_.CreateSub(length, llvm::ConstantInt::get(int64Type, 1, true), "vec.pop.last");
        llvm::Value* elementPtr = builder_.CreateInBoundsGEP(llvmType(vecType.elementType()), data, lastIndex, "vec.pop.slot");
        Type elementType = vecType.elementType();
        vecFieldStore(header, 0, lastIndex);
        if (elementType.kind() == TypeKind::Struct || elementType.isArray()) {
            llvm::Function* function = builder_.GetInsertBlock()->getParent();
            llvm::AllocaInst* slot = createEntryAlloca(function, "vec.pop.struct", elementType);
            builder_.CreateMemCpy(slot, llvm::MaybeAlign(), elementPtr, llvm::MaybeAlign(), module_->getDataLayout().getTypeAllocSize(llvmType(elementType)));
            return {slot, elementType, true};
        }
        return {builder_.CreateLoad(llvmType(elementType), elementPtr, "vec.pop.value"), elementType};
    }

    void emitVecSet(llvm::Value* header, Type vecType, llvm::Value* index, ValueInfo value) {
        llvm::Value* length = vecFieldLoad(header, 0, "vec.set.len");
        llvm::Value* data = vecFieldLoad(header, 2, "vec.set.data");
        emitRuntimeCheck(vecIndexOk(index, length), "vec index out of bounds");
        llvm::Value* elementPtr = builder_.CreateInBoundsGEP(llvmType(vecType.elementType()), data, index, "vec.set.slot");
        Type elementType = vecType.elementType();
        ValueInfo elementValue = convertValue(value, elementType);
        if (elementType.kind() == TypeKind::Struct || elementType.isArray()) {
            ValueInfo source = materializeAggregate(elementValue);
            builder_.CreateMemCpy(elementPtr, llvm::MaybeAlign(), source.value, llvm::MaybeAlign(), module_->getDataLayout().getTypeAllocSize(llvmType(elementType)));
        } else {
            builder_.CreateStore(elementValue.value, elementPtr);
        }
    }

    void emitVecReserve(llvm::Value* header, Type vecType, llvm::Value* requested) {
        auto* int64Type = llvm::Type::getInt64Ty(context_);
        auto* ptrType = llvm::PointerType::get(context_, 0);
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        auto* growBlock = llvm::BasicBlock::Create(context_, "vec.reserve.grow", function);
        auto* doneBlock = llvm::BasicBlock::Create(context_, "vec.reserve.done");

        llvm::Value* capacity = vecFieldLoad(header, 1, "vec.reserve.cap");
        llvm::Value* data = vecFieldLoad(header, 2, "vec.reserve.data");
        builder_.CreateCondBr(builder_.CreateICmpSGT(requested, capacity, "vec.reserve.needed"), growBlock, doneBlock);

        builder_.SetInsertPoint(growBlock);
        llvm::Value* byteCapacity = builder_.CreateMul(requested, llvm::ConstantInt::get(int64Type, static_cast<long long>(vecElementSize(vecType)), true), "vec.reserve.bytes");
        llvm::Value* grownData = builder_.CreateCall(realloc_, {data, byteCapacity}, "vec.reserve.grown");
        emitRuntimeCheck(builder_.CreateICmpNE(grownData, llvm::ConstantPointerNull::get(ptrType), "vec.reserve.ok"), "failed to reserve vec capacity");
        vecFieldStore(header, 1, requested);
        vecFieldStore(header, 2, grownData);
        builder_.CreateBr(doneBlock);

        function->insert(function->end(), doneBlock);
        builder_.SetInsertPoint(doneBlock);
    }

    void emitSeed(llvm::Value* seed) {
        builder_.CreateStore(seed, rngState_);
    }

    ValueInfo emitRand() {
        auto* int64Type = llvm::Type::getInt64Ty(context_);
        // SplitMix64, identical to the interpreter implementation: the state
        // advances by the golden ratio constant on every call.
        llvm::Value* state = builder_.CreateLoad(int64Type, rngState_, "rand.state");
        llvm::Value* advanced = builder_.CreateAdd(state, llvm::ConstantInt::get(int64Type, 0x9E3779B97F4A7C15LL, true), "rand.state.next");
        builder_.CreateStore(advanced, rngState_);

        llvm::Value* z1 = builder_.CreateXor(advanced, builder_.CreateLShr(advanced, llvm::ConstantInt::get(int64Type, 30, true), "rand.shr30"), "rand.z1");
        llvm::Value* z2 = builder_.CreateMul(z1, llvm::ConstantInt::get(int64Type, 0xBF58476D1CE4E5B9LL, true), "rand.z2");
        llvm::Value* z3 = builder_.CreateXor(z2, builder_.CreateLShr(z2, llvm::ConstantInt::get(int64Type, 27, true), "rand.shr27"), "rand.z3");
        llvm::Value* z4 = builder_.CreateMul(z3, llvm::ConstantInt::get(int64Type, 0x94D049BB13511EF9LL, true), "rand.z4");
        llvm::Value* result = builder_.CreateXor(z4, builder_.CreateLShr(z4, llvm::ConstantInt::get(int64Type, 31, true), "rand.shr31"), "rand.result");
        return {result, Type(TypeKind::I64)};
    }

    ValueInfo emitEnumMember(const std::string& qualifiedName) {
        const std::size_t separator = qualifiedName.find("::");
        const std::string enumName = qualifiedName.substr(0, separator);
        const std::string memberName = qualifiedName.substr(separator + 2);
        const auto found = enums_.find(enumName);
        if (found == enums_.end()) {
            throw std::runtime_error("unknown enum '" + enumName + "'");
        }
        const auto member = found->second.memberIndices.find(memberName);
        if (member == found->second.memberIndices.end()) {
            throw std::runtime_error("enum '" + enumName + "' has no member '" + memberName + "'");
        }
        return {llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), static_cast<long long>(member->second), true), Type::enumOf(enumName)};
    }

    ValueInfo emitUnary(const UnaryExpr& expression) {
        ValueInfo operand = emitExpr(expression.operand());
        if (expression.op() == "!") {
            ValueInfo value = convertToBool(operand);
            return {builder_.CreateNot(value.value), Type(TypeKind::Bool)};
        }
        if (expression.op() == "-") {
            if (!operand.type.isNumeric()) {
                throw std::runtime_error("unary '-' requires a numeric operand");
            }
            if (operand.type.isFloat()) {
                return {builder_.CreateFNeg(operand.value), operand.type};
            }
            return {builder_.CreateNeg(operand.value), operand.type};
        }
        throw std::runtime_error("unsupported unary operator in native codegen: " + expression.op());
    }

    ValueInfo emitBinary(const BinaryExpr& expression) {
        const std::string& op = expression.op();

        // `&&` and `||` must not evaluate the right operand when the left one
        // already decides the result. Evaluating both eagerly made the guard
        // pattern `i < n && char_at(text, i) != ' '` read past the end of the
        // string and abort, and made `x != 0 && 10 / x > 1` divide by zero.
        if (op == "&&" || op == "||") {
            const bool isAnd = (op == "&&");
            llvm::Function* function = builder_.GetInsertBlock()->getParent();

            llvm::Value* left = convertToBool(emitExpr(expression.left())).value;
            llvm::BasicBlock* leftEnd = builder_.GetInsertBlock();
            if (leftEnd->getTerminator()) {
                // The left operand cannot fall through: the result is decided.
                return {left, Type(TypeKind::Bool)};
            }

            auto* rhsBlock = llvm::BasicBlock::Create(context_, isAnd ? "and.rhs" : "or.rhs");
            auto* endBlock = llvm::BasicBlock::Create(context_, isAnd ? "and.end" : "or.end");

            builder_.CreateCondBr(left, isAnd ? rhsBlock : endBlock, isAnd ? endBlock : rhsBlock);

            function->insert(function->end(), rhsBlock);
            builder_.SetInsertPoint(rhsBlock);
            llvm::Value* right = convertToBool(emitExpr(expression.right())).value;
            llvm::BasicBlock* rhsEnd = builder_.GetInsertBlock();
            if (!rhsEnd->getTerminator()) {
                builder_.CreateBr(endBlock);
            }

            function->insert(function->end(), endBlock);
            builder_.SetInsertPoint(endBlock);

            auto* result = builder_.CreatePHI(llvm::Type::getInt1Ty(context_), 2, isAnd ? "and.result" : "or.result");
            result->addIncoming(left, leftEnd);
            result->addIncoming(right, rhsEnd);
            return {result, Type(TypeKind::Bool)};
        }

        ValueInfo left = emitExpr(expression.left());
        ValueInfo right = emitExpr(expression.right());

        if (op == "+" && left.type.kind() == TypeKind::String && right.type.kind() == TypeKind::String) {
            return emitStringConcat(left.value, right.value);
        }

        if ((op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
            && left.type.kind() == TypeKind::String && right.type.kind() == TypeKind::String) {
            return emitStringCompare(op, left.value, right.value);
        }

        Type type = commonType(left.type, right.type);
        if (type.isUnknown()) {
            throw std::runtime_error("operator '" + op + "' cannot be lowered for '" + left.type.name() + "' and '" + right.type.name() + "'");
        }

        left = convertValue(left, type);
        right = convertValue(right, type);

        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            if (type.isFloat()) {
                if (op == "+") return {builder_.CreateFAdd(left.value, right.value), type};
                if (op == "-") return {builder_.CreateFSub(left.value, right.value), type};
                if (op == "*") return {builder_.CreateFMul(left.value, right.value), type};
                if (op == "/") return {builder_.CreateFDiv(left.value, right.value), type};
                if (op == "%") return {builder_.CreateFRem(left.value, right.value), type};
            }

            if (op == "+") return {builder_.CreateAdd(left.value, right.value), type};
            if (op == "-") return {builder_.CreateSub(left.value, right.value), type};
            if (op == "*") return {builder_.CreateMul(left.value, right.value), type};
            if (op == "/" || op == "%") {
                // Integer division by zero is undefined behaviour; without this
                // check a compiled program died with STATUS_INTEGER_DIVIDE_BY_ZERO
                // and no message at all.
                llvm::Value* zero = llvm::ConstantInt::get(llvmType(type), 0, true);
                emitRuntimeCheck(
                    builder_.CreateICmpNE(right.value, zero, "divisor.nonzero"),
                    op == "/" ? "division by zero" : "remainder by zero");
                if (op == "/") return {builder_.CreateSDiv(left.value, right.value), type};
                return {builder_.CreateSRem(left.value, right.value), type};
            }
        }

        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (type.isFloat()) {
                if (op == "==") return {builder_.CreateFCmpOEQ(left.value, right.value), Type(TypeKind::Bool)};
                if (op == "!=") return {builder_.CreateFCmpONE(left.value, right.value), Type(TypeKind::Bool)};
                if (op == "<") return {builder_.CreateFCmpOLT(left.value, right.value), Type(TypeKind::Bool)};
                if (op == "<=") return {builder_.CreateFCmpOLE(left.value, right.value), Type(TypeKind::Bool)};
                if (op == ">") return {builder_.CreateFCmpOGT(left.value, right.value), Type(TypeKind::Bool)};
                if (op == ">=") return {builder_.CreateFCmpOGE(left.value, right.value), Type(TypeKind::Bool)};
            }

            if (op == "==") return {builder_.CreateICmpEQ(left.value, right.value), Type(TypeKind::Bool)};
            if (op == "!=") return {builder_.CreateICmpNE(left.value, right.value), Type(TypeKind::Bool)};
            if (op == "<") return {builder_.CreateICmpSLT(left.value, right.value), Type(TypeKind::Bool)};
            if (op == "<=") return {builder_.CreateICmpSLE(left.value, right.value), Type(TypeKind::Bool)};
            if (op == ">") return {builder_.CreateICmpSGT(left.value, right.value), Type(TypeKind::Bool)};
            if (op == ">=") return {builder_.CreateICmpSGE(left.value, right.value), Type(TypeKind::Bool)};
        }

        throw std::runtime_error("unsupported binary operator in native codegen: " + op);
    }

    // A miko string constant, laid out exactly like the runtime allocator lays
    // out a dynamic one: a length word, a capacity word, then the text, then a
    // NUL. Capacity equals length here, which is what tells miko_str_append it
    // must copy instead of writing into read-only memory. The value returned
    // points at the text, so the C ABI still sees a `char*`.
    // llvm::CreateGlobalStringPtr cannot be used for this: it emits the bytes
    // with no header, and then len() would read whatever preceded the literal.
    llvm::Constant* createManagedString(const std::string& text) {
        std::vector<uint8_t> bytes;
        uint64_t length = static_cast<uint64_t>(text.size());
        // length, then capacity - both equal for a literal.
        for (int word = 0; word < 2; ++word) {
            for (int shift = 0; shift < 8; ++shift) {
                bytes.push_back(static_cast<uint8_t>((length >> (8 * shift)) & 0xFF));
            }
        }
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.push_back(0);

        auto* data = llvm::ConstantDataArray::get(context_, bytes);
        auto* global = new llvm::GlobalVariable(
            *module_, data->getType(), true, llvm::GlobalValue::PrivateLinkage, data, ".str.managed");
        global->setAlignment(llvm::Align(8));

        auto* int64Type = llvm::Type::getInt64Ty(context_);
        auto* zero = llvm::ConstantInt::get(int64Type, 0);
        auto* headerSize = llvm::ConstantInt::get(int64Type, 16);
        // A constant expression, not an instruction: defaultValue() needs a
        // Constant for zero-initialising a string, and a constant folds just as
        // well inside a function body.
        return llvm::ConstantExpr::getInBoundsGetElementPtr(
            data->getType(), global, llvm::ArrayRef<llvm::Constant*>{zero, headerSize});
    }

    ValueInfo emitStringConcat(llvm::Value* left, llvm::Value* right) {
        return {builder_.CreateCall(strConcat_, {left, right}, "string.concat"), Type(TypeKind::String)};
    }

    // append_str(dst, piece) - build a string in a loop without the quadratic
    // copy that `dst = dst + piece` costs. See miko_str_append in the runtime for
    // the ownership rule: the caller must not keep another reference to dst.
    ValueInfo emitStringAppend(llvm::Value* dst, llvm::Value* piece) {
        return {builder_.CreateCall(strAppend_, {dst, piece}, "string.append"), Type(TypeKind::String)};
    }

    ValueInfo emitStringCompare(const std::string& op, llvm::Value* left, llvm::Value* right) {
        llvm::Value* compared = builder_.CreateCall(strcmp_, {left, right}, "string.cmp");
        llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0, true);

        if (op == "==") return {builder_.CreateICmpEQ(compared, zero), Type(TypeKind::Bool)};
        if (op == "!=") return {builder_.CreateICmpNE(compared, zero), Type(TypeKind::Bool)};
        if (op == "<") return {builder_.CreateICmpSLT(compared, zero), Type(TypeKind::Bool)};
        if (op == "<=") return {builder_.CreateICmpSLE(compared, zero), Type(TypeKind::Bool)};
        if (op == ">") return {builder_.CreateICmpSGT(compared, zero), Type(TypeKind::Bool)};
        if (op == ">=") return {builder_.CreateICmpSGE(compared, zero), Type(TypeKind::Bool)};

        throw std::runtime_error("unsupported string comparison operator: " + op);
    }

    ValueInfo emitCall(const CallExpr& expression) {
        const std::string builtinName = canonicalBuiltinName(expression.callee());

        if (builtinName == "print") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("print expects exactly one argument");
            }

            ValueInfo argument = emitExpr(*expression.arguments().front());
            emitPrint(argument);
            return {nullptr, Type::voidType()};
        }

        if (builtinName == "len") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("len expects exactly one argument");
            }

            ValueInfo argument = emitExpr(*expression.arguments().front());
            if (argument.type.kind() == TypeKind::String) {
                return {builder_.CreateCall(strLen_, {argument.value}, "string.len"), Type(TypeKind::I64)};
            }
            if (argument.type.isVec()) {
                return {vecFieldLoad(argument.value, 0, "vec.len"), Type(TypeKind::I64)};
            }
            if (argument.type.isArray()) {
                return {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), static_cast<long long>(argument.type.arrayLength()), true), Type(TypeKind::I64)};
            }
            throw std::runtime_error("len expects a string, array, or vec");
        }

        if (builtinName == "input") {
            if (!expression.arguments().empty()) {
                throw std::runtime_error("input expects no arguments");
            }
            return emitInput();
        }

        if (builtinName == "arg_count") {
            if (!expression.arguments().empty()) {
                throw std::runtime_error("arg_count expects no arguments");
            }
            return emitArgCount();
        }

        if (builtinName == "arg_at") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("arg_at expects exactly one argument");
            }
            return emitArgAt(convertValue(emitExpr(*expression.arguments()[0]), Type(TypeKind::I64)).value);
        }

        if (builtinName == "char_at") {
            if (expression.arguments().size() != 2) {
                throw std::runtime_error("char_at expects exactly two arguments");
            }
            ValueInfo text = emitExpr(*expression.arguments()[0]);
            if (text.type.kind() != TypeKind::String) {
                throw std::runtime_error("char_at expects first argument to be string");
            }
            llvm::Value* index = convertValue(emitExpr(*expression.arguments()[1]), Type(TypeKind::I64)).value;
            return emitCharAt(text.value, index);
        }

        if (builtinName == "substring") {
            if (expression.arguments().size() != 3) {
                throw std::runtime_error("substring expects exactly three arguments");
            }
            ValueInfo text = emitExpr(*expression.arguments()[0]);
            if (text.type.kind() != TypeKind::String) {
                throw std::runtime_error("substring expects first argument to be string");
            }
            llvm::Value* start = convertValue(emitExpr(*expression.arguments()[1]), Type(TypeKind::I64)).value;
            llvm::Value* length = convertValue(emitExpr(*expression.arguments()[2]), Type(TypeKind::I64)).value;
            return emitSubstring(text.value, start, length);
        }

        if (builtinName == "read_file") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("read_file expects exactly one argument");
            }
            ValueInfo path = emitExpr(*expression.arguments()[0]);
            if (path.type.kind() != TypeKind::String) {
                throw std::runtime_error("read_file expects a string path");
            }
            return emitReadFile(path.value);
        }

        if (builtinName == "run_command") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("run_command expects exactly one argument");
            }
            ValueInfo command = emitExpr(*expression.arguments()[0]);
            if (command.type.kind() != TypeKind::String) {
                throw std::runtime_error("run_command expects a string command");
            }
            // Routed through the runtime like the self-hosted backend does, so
            // shelling out is one replaceable entry point (miko_run_command)
            // instead of a direct system() call baked into every program's IR.
            llvm::CallInst* status = builder_.CreateCall(runCommand_, {command.value}, "run_command.status");
            return {builder_.CreateSExt(status, builder_.getInt64Ty(), "run_command.wide"), Type(TypeKind::I64)};
        }

        if (builtinName == "write_file") {
            if (expression.arguments().size() != 2) {
                throw std::runtime_error("write_file expects exactly two arguments");
            }
            ValueInfo path = emitExpr(*expression.arguments()[0]);
            ValueInfo content = emitExpr(*expression.arguments()[1]);
            if (path.type.kind() != TypeKind::String || content.type.kind() != TypeKind::String) {
                throw std::runtime_error("write_file expects string path and content");
            }
            emitWriteFile(path.value, content.value);
            return {nullptr, Type::voidType()};
        }

        if (builtinName == "seed_i64") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("seed_i64 expects exactly one argument");
            }
            ValueInfo arg = emitExpr(*expression.arguments()[0]);
            emitSeed(convertValue(arg, Type(TypeKind::I64)).value);
            return {nullptr, Type::voidType()};
        }

        if (builtinName == "rand_i64") {
            if (!expression.arguments().empty()) {
                throw std::runtime_error("rand_i64 expects no arguments");
            }
            return emitRand();
        }

        if (builtinName == "vec_new") {
            if (!expression.arguments().empty()) {
                throw std::runtime_error("vec_new expects no arguments");
            }
            return {emitVecNew(), Type::unknown()};
        }

        if (builtinName == "vec_push") {
            if (expression.arguments().size() != 2) {
                throw std::runtime_error("vec_push expects exactly two arguments");
            }
            ValueInfo target = emitExpr(*expression.arguments()[0]);
            if (!target.type.isVec()) {
                throw std::runtime_error("vec_push expects a vec, got '" + target.type.name() + "'");
            }
            ValueInfo element = emitExpr(*expression.arguments()[1]);
            emitVecPush(target.value, target.type, element);
            return {nullptr, Type::voidType()};
        }

        if (builtinName == "vec_pop") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("vec_pop expects exactly one argument");
            }
            ValueInfo target = emitExpr(*expression.arguments()[0]);
            if (!target.type.isVec()) {
                throw std::runtime_error("vec_pop expects a vec, got '" + target.type.name() + "'");
            }
            return emitVecPop(target.value, target.type);
        }

        if (builtinName == "vec_set") {
            if (expression.arguments().size() != 3) {
                throw std::runtime_error("vec_set expects exactly three arguments");
            }
            ValueInfo target = emitExpr(*expression.arguments()[0]);
            if (!target.type.isVec()) {
                throw std::runtime_error("vec_set expects a vec, got '" + target.type.name() + "'");
            }
            llvm::Value* index = convertValue(emitExpr(*expression.arguments()[1]), Type(TypeKind::I64)).value;
            ValueInfo value = emitExpr(*expression.arguments()[2]);
            emitVecSet(target.value, target.type, index, value);
            return {nullptr, Type::voidType()};
        }

        if (builtinName == "vec_clear") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("vec_clear expects exactly one argument");
            }
            ValueInfo target = emitExpr(*expression.arguments()[0]);
            if (!target.type.isVec()) {
                throw std::runtime_error("vec_clear expects a vec, got '" + target.type.name() + "'");
            }
            vecFieldStore(target.value, 0, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0, true));
            return {nullptr, Type::voidType()};
        }

        if (builtinName == "vec_reserve") {
            if (expression.arguments().size() != 2) {
                throw std::runtime_error("vec_reserve expects exactly two arguments");
            }
            ValueInfo target = emitExpr(*expression.arguments()[0]);
            if (!target.type.isVec()) {
                throw std::runtime_error("vec_reserve expects a vec, got '" + target.type.name() + "'");
            }
            llvm::Value* capacity = convertValue(emitExpr(*expression.arguments()[1]), Type(TypeKind::I64)).value;
            emitVecReserve(target.value, target.type, capacity);
            return {nullptr, Type::voidType()};
        }

        if (builtinName == "to_string_i64") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("to_string_i64 expects exactly one argument");
            }
            llvm::Value* value = convertValue(emitExpr(*expression.arguments()[0]), Type(TypeKind::I64)).value;
            return {builder_.CreateCall(toStringI64_, {value}, "to_string_i64.text"), Type(TypeKind::String)};
        }

        if (builtinName == "to_string_f64") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("to_string_f64 expects exactly one argument");
            }
            llvm::Value* value = convertValue(emitExpr(*expression.arguments()[0]), Type(TypeKind::F64)).value;
            return {builder_.CreateCall(toStringF64_, {value}, "to_string_f64.text"), Type(TypeKind::String)};
        }

        if (builtinName == "to_string_bool") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("to_string_bool expects exactly one argument");
            }
            llvm::Value* value = convertToBool(emitExpr(*expression.arguments()[0])).value;
            llvm::Value* trueText = createManagedString("true");
            llvm::Value* falseText = createManagedString("false");
            return {builder_.CreateSelect(value, trueText, falseText, "bool.str"), Type(TypeKind::String)};
        }

        if (builtinName == "to_string_char") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("to_string_char expects exactly one argument");
            }
            llvm::Value* code = convertValue(emitExpr(*expression.arguments()[0]), Type(TypeKind::I64)).value;
            llvm::Value* buffer = builder_.CreateCall(malloc_, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 2, true)}, "char.str");
            builder_.CreateStore(builder_.CreateTrunc(code, llvm::Type::getInt8Ty(context_), "char.byte"), buffer);
            llvm::Value* terminator = builder_.CreateInBoundsGEP(llvm::Type::getInt8Ty(context_), buffer, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1, true), "char.str.term");
            builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(context_), 0), terminator);
            return {buffer, Type(TypeKind::String)};
        }

        if (builtinName == "str_to_i64") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("str_to_i64 expects exactly one argument");
            }
            ValueInfo text = emitExpr(*expression.arguments()[0]);
            if (text.type.kind() != TypeKind::String) {
                throw std::runtime_error("str_to_i64 expects a string");
            }
            auto* int64Type = llvm::Type::getInt64Ty(context_);
            auto* ptrType = llvm::PointerType::get(context_, 0);
            llvm::Function* function = builder_.GetInsertBlock()->getParent();
            llvm::AllocaInst* endSlot = createEntryAlloca(function, "str_to_i64.end", Type(TypeKind::String));
            llvm::Value* value = builder_.CreateCall(strtoll_, {text.value, endSlot, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 10, true)}, "str_to_i64.value");
            llvm::Value* end = builder_.CreateLoad(ptrType, endSlot, "str_to_i64.end.value");
            llvm::Value* advanced = builder_.CreateICmpNE(end, text.value, "str_to_i64.advanced");
            llvm::Value* terminatorByte = builder_.CreateLoad(llvm::Type::getInt8Ty(context_), end, "str_to_i64.term");
            llvm::Value* atEnd = builder_.CreateICmpEQ(terminatorByte, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context_), 0, true), "str_to_i64.atend");
            emitRuntimeCheck(builder_.CreateAnd(advanced, atEnd, "str_to_i64.ok"), "invalid integer");
            return {value, Type(TypeKind::I64)};
        }

        if (builtinName == "str_to_f64") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("str_to_f64 expects exactly one argument");
            }
            ValueInfo text = emitExpr(*expression.arguments()[0]);
            if (text.type.kind() != TypeKind::String) {
                throw std::runtime_error("str_to_f64 expects a string");
            }
            auto* ptrType = llvm::PointerType::get(context_, 0);
            llvm::Function* function = builder_.GetInsertBlock()->getParent();
            llvm::AllocaInst* endSlot = createEntryAlloca(function, "str_to_f64.end", Type(TypeKind::String));
            llvm::Value* value = builder_.CreateCall(strtod_, {text.value, endSlot}, "str_to_f64.value");
            llvm::Value* end = builder_.CreateLoad(ptrType, endSlot, "str_to_f64.end.value");
            llvm::Value* advanced = builder_.CreateICmpNE(end, text.value, "str_to_f64.advanced");
            llvm::Value* terminatorByte = builder_.CreateLoad(llvm::Type::getInt8Ty(context_), end, "str_to_f64.term");
            llvm::Value* atEnd = builder_.CreateICmpEQ(terminatorByte, llvm::ConstantInt::get(llvm::Type::getInt8Ty(context_), 0, true), "str_to_f64.atend");
            emitRuntimeCheck(builder_.CreateAnd(advanced, atEnd, "str_to_f64.ok"), "invalid number");
            return {value, Type(TypeKind::F64)};
        }

        if (builtinName == "append_str") {
            if (expression.arguments().size() != 2) {
                throw std::runtime_error("append_str expects exactly two arguments");
            }
            ValueInfo target = emitExpr(*expression.arguments()[0]);
            ValueInfo piece = emitExpr(*expression.arguments()[1]);
            if (target.type.kind() != TypeKind::String || piece.type.kind() != TypeKind::String) {
                throw std::runtime_error("append_str expects string arguments");
            }
            return emitStringAppend(target.value, piece.value);
        }

        if (builtinName == "str_index_of") {
            if (expression.arguments().size() != 2) {
                throw std::runtime_error("str_index_of expects exactly two arguments");
            }
            ValueInfo text = emitExpr(*expression.arguments()[0]);
            ValueInfo needle = emitExpr(*expression.arguments()[1]);
            if (text.type.kind() != TypeKind::String || needle.type.kind() != TypeKind::String) {
                throw std::runtime_error("str_index_of expects string arguments");
            }
            return {builder_.CreateCall(strIndexOf_, {text.value, needle.value}, "str_index_of.result"), Type(TypeKind::I64)};
        }

        if (builtinName == "str_contains") {
            if (expression.arguments().size() != 2) {
                throw std::runtime_error("str_contains expects exactly two arguments");
            }
            ValueInfo text = emitExpr(*expression.arguments()[0]);
            ValueInfo needle = emitExpr(*expression.arguments()[1]);
            if (text.type.kind() != TypeKind::String || needle.type.kind() != TypeKind::String) {
                throw std::runtime_error("str_contains expects string arguments");
            }
            // The runtime returns a C int; miko's bool is i1, so narrow it here
            // rather than handing an i32 to every boolean context (select needs
            // an i1 condition).
            llvm::Value* raw = builder_.CreateCall(strContains_, {text.value, needle.value}, "str_contains.raw");
            llvm::Value* isSet = builder_.CreateICmpNE(
                raw, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0), "str_contains.result");
            return {isSet, Type(TypeKind::Bool)};
        }

        if (builtinName == "sqrt_f64" || builtinName == "floor_f64" || builtinName == "ceil_f64" || builtinName == "round_f64") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error(builtinName + " expects exactly one argument");
            }
            llvm::Value* value = convertValue(emitExpr(*expression.arguments()[0]), Type(TypeKind::F64)).value;
            llvm::Intrinsic::ID id = builtinName == "sqrt_f64" ? llvm::Intrinsic::sqrt
                : builtinName == "floor_f64" ? llvm::Intrinsic::floor
                : builtinName == "ceil_f64" ? llvm::Intrinsic::ceil
                : llvm::Intrinsic::round;
            return {builder_.CreateUnaryIntrinsic(id, value), Type(TypeKind::F64)};
        }

        if (builtinName == "f64_to_i64") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("f64_to_i64 expects exactly one argument");
            }
            llvm::Value* value = convertValue(emitExpr(*expression.arguments()[0]), Type(TypeKind::F64)).value;
            return {builder_.CreateFPToSI(value, llvm::Type::getInt64Ty(context_), "f64_to_i64"), Type(TypeKind::I64)};
        }

        if (builtinName == "exit") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("exit expects exactly one argument");
            }
            llvm::Value* code = convertValue(emitExpr(*expression.arguments()[0]), Type(TypeKind::I32)).value;
            builder_.CreateCall(exit_, {code});
            builder_.CreateUnreachable();
            return {nullptr, Type::voidType()};
        }

        if (builtinName == "eprint") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("eprint expects exactly one argument");
            }
            ValueInfo text = emitExpr(*expression.arguments()[0]);
            if (text.type.kind() != TypeKind::String) {
                throw std::runtime_error("eprint expects a string");
            }
            // Routed through the runtime rather than emitting fprintf against
            // __acrt_iob_func(2): that symbol only exists in the MSVC CRT, so
            // writing stderr directly in the IR pinned every generated program
            // to Windows.
            builder_.CreateCall(eprintStr_, {text.value});
            return {nullptr, Type::voidType()};
        }

        if (builtinName == "clock_ms") {
            if (!expression.arguments().empty()) {
                throw std::runtime_error("clock_ms expects no arguments");
            }
            // Wall-clock milliseconds come from the runtime, which picks
            // GetSystemTimeAsFileTime on Windows and clock_gettime elsewhere.
            llvm::Value* epochMs = builder_.CreateCall(clockMs_, {}, "clock.ms");
            return {epochMs, Type(TypeKind::I64)};
        }

        if (builtinName == "mono_ms") {
            if (!expression.arguments().empty()) {
                throw std::runtime_error("mono_ms expects no arguments");
            }
            // Monotonic milliseconds: QueryPerformanceCounter on Windows,
            // clock_gettime(CLOCK_MONOTONIC) elsewhere - both inside the runtime.
            llvm::Value* mono = builder_.CreateCall(monoMs_, {}, "mono.ms");
            return {mono, Type(TypeKind::I64)};
        }

        if (builtinName == "file_exists") {
            if (expression.arguments().size() != 1) {
                throw std::runtime_error("file_exists expects exactly one argument");
            }
            ValueInfo path = emitExpr(*expression.arguments()[0]);
            if (path.type.kind() != TypeKind::String) {
                throw std::runtime_error("file_exists expects a string path");
            }
            llvm::Value* mode = builder_.CreateGlobalStringPtr("rb");
            llvm::Value* file = builder_.CreateCall(fopen_, {path.value, mode}, "file_exists.handle");
            llvm::Value* exists = builder_.CreateICmpNE(file, llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)), "file_exists.result");
            auto* existsBlock = llvm::BasicBlock::Create(context_, "file_exists.close", builder_.GetInsertBlock()->getParent());
            auto* doneBlock = llvm::BasicBlock::Create(context_, "file_exists.done");
            builder_.CreateCondBr(exists, existsBlock, doneBlock);

            builder_.SetInsertPoint(existsBlock);
            builder_.CreateCall(fclose_, {file});
            builder_.CreateBr(doneBlock);

            builder_.GetInsertBlock()->getParent()->insert(builder_.GetInsertBlock()->getParent()->end(), doneBlock);
            builder_.SetInsertPoint(doneBlock);
            return {exists, Type(TypeKind::Bool)};
        }

        const auto found = functions_.find(expression.callee());
        if (found == functions_.end()) {
            throw std::runtime_error("unknown function '" + expression.callee() + "'");
        }

        const FunctionInfo& function = found->second;
        std::vector<llvm::Value*> arguments;
        for (std::size_t i = 0; i < expression.arguments().size(); ++i) {
            ValueInfo argument = emitExpr(*expression.arguments()[i]);
            if (i < function.parameterTypes.size()) {
                Type parameterType = function.parameterTypes[i];
                if (parameterType.isArray() || parameterType.kind() == TypeKind::Struct) {
                    if (argument.type != parameterType) {
                        throw std::runtime_error("cannot pass '" + argument.type.name() + "' to parameter of type '" + parameterType.name() + "'");
                    }
                    argument = materializeAggregate(argument);
                    argument = {builder_.CreateLoad(llvmType(argument.type), argument.value, "aggregate.arg.load"), argument.type};
                } else {
                    argument = convertValue(argument, parameterType);
                }
            }
            arguments.push_back(argument.value);
        }

        llvm::Value* call = builder_.CreateCall(function.function, arguments);
        return {call, function.returnType};
    }

    ValueInfo emitInput() {
        // The runtime owns reading stdin; doing it in the IR meant naming
        // __acrt_iob_func(0), which is MSVC-only.
        llvm::Value* text = builder_.CreateCall(input_, {}, "input.text");
        return {text, Type(TypeKind::String)};
    }

    void emitRuntimeAbort(const std::string& message) {
        // Diagnostics belong on stderr: printing them to stdout mixed a fatal
        // error into the program's own output, so a caller capturing stdout
        // could not tell a partial result from a complete one. The runtime
        // function keeps the platform symbol (MSVC's __acrt_iob_func) out of
        // the generated IR.
        auto* text = builder_.CreateGlobalStringPtr(message);
        builder_.CreateCall(runtimeError_, {text});
        builder_.CreateUnreachable();
    }

    void emitRuntimeCheck(llvm::Value* condition, const std::string& message) {
        llvm::Function* function = builder_.GetInsertBlock()->getParent();
        auto* okBlock = llvm::BasicBlock::Create(context_, "runtime.ok", function);
        auto* failBlock = llvm::BasicBlock::Create(context_, "runtime.fail", function);
        builder_.CreateCondBr(condition, okBlock, failBlock);

        builder_.SetInsertPoint(failBlock);
        emitRuntimeAbort(message);

        builder_.SetInsertPoint(okBlock);
    }

    ValueInfo emitArgCount() {
        llvm::Value* argc = builder_.CreateLoad(llvm::Type::getInt32Ty(context_), programArgc_, "argc");
        llvm::Value* count32 = builder_.CreateSub(argc, llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 1, true), "program.argc");
        llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0, true);
        llvm::Value* nonNegative = builder_.CreateICmpSGE(count32, zero, "program.argc.nonnegative");
        llvm::Value* clamped = builder_.CreateSelect(nonNegative, count32, zero, "program.argc.clamped");
        return {builder_.CreateSExt(clamped, llvm::Type::getInt64Ty(context_), "program.argc.i64"), Type(TypeKind::I64)};
    }

    ValueInfo emitArgAt(llvm::Value* index) {
        // Routed through the runtime: argv entries are bare OS strings with no
        // length header, and the runtime copies them into a managed string.
        // Emitting the argv load inline here handed back a headerless pointer,
        // so len(arg_at(0)) read whatever preceded the argument in memory.
        return {builder_.CreateCall(argAt_, {index}, "arg_at.text"), Type(TypeKind::String)};
    }

    ValueInfo emitCharAt(llvm::Value* text, llvm::Value* index) {
        // The runtime performs the bounds check against the header length and
        // reports through miko_runtime_error, so the diagnostic is identical to
        // the self-hosted backend's.
        return {builder_.CreateCall(charAt_, {text, index}, "char_at.char"), Type(TypeKind::Char)};
    }

    ValueInfo emitSubstring(llvm::Value* text, llvm::Value* start, llvm::Value* length) {
        return {builder_.CreateCall(substring_, {text, start, length}, "substring.text"), Type(TypeKind::String)};
    }

    ValueInfo emitReadFile(llvm::Value* path) {
        // One call: runtime/miko_runtime.c reads the whole file and returns a
        // managed string. This used to be a 68-line byte-at-a-time loop that
        // grew a raw malloc buffer, which both buried the I/O in the IR and
        // produced a string with no length header.
        return {builder_.CreateCall(readFile_, {path}, "read_file.text"), Type(TypeKind::String)};
    }


    void emitWriteFile(llvm::Value* path, llvm::Value* content) {
        // miko_write_file takes the length from the string header, so binary
        // content survives - the fopen/fwrite sequence this replaces measured with
        // miko_str_len as well, but it duplicated the runtime's logic and drifted
        // from it on parent directories.
        builder_.CreateCall(writeFile_, {path, content});
    }

    void emitPrint(ValueInfo argument) {
        if (argument.type.kind() == TypeKind::String) {
            auto* format = builder_.CreateGlobalStringPtr("%s\n");
            builder_.CreateCall(printf_, {format, argument.value});
            return;
        }

        if (argument.type.kind() == TypeKind::Bool) {
            auto* trueText = builder_.CreateGlobalStringPtr("true");
            auto* falseText = builder_.CreateGlobalStringPtr("false");
            auto* text = builder_.CreateSelect(argument.value, trueText, falseText);
            auto* format = builder_.CreateGlobalStringPtr("%s\n");
            builder_.CreateCall(printf_, {format, text});
            return;
        }

        if (argument.type.kind() == TypeKind::I32) {
            auto* format = builder_.CreateGlobalStringPtr("%d\n");
            builder_.CreateCall(printf_, {format, argument.value});
            return;
        }

        if (argument.type.kind() == TypeKind::Byte) {
            auto* format = builder_.CreateGlobalStringPtr("%u\n");
            ValueInfo promoted = convertValue(argument, Type(TypeKind::I32));
            builder_.CreateCall(printf_, {format, promoted.value});
            return;
        }

        if (argument.type.kind() == TypeKind::Char) {
            auto* format = builder_.CreateGlobalStringPtr("%c\n");
            builder_.CreateCall(printf_, {format, argument.value});
            return;
        }

        if (argument.type.kind() == TypeKind::I64) {
            auto* format = builder_.CreateGlobalStringPtr("%lld\n");
            builder_.CreateCall(printf_, {format, argument.value});
            return;
        }

        if (argument.type.kind() == TypeKind::F32) {
            auto* format = builder_.CreateGlobalStringPtr("%g\n");
            ValueInfo promoted = convertValue(argument, Type(TypeKind::F64));
            builder_.CreateCall(printf_, {format, promoted.value});
            return;
        }

        if (argument.type.kind() == TypeKind::F64) {
            auto* format = builder_.CreateGlobalStringPtr("%g\n");
            builder_.CreateCall(printf_, {format, argument.value});
            return;
        }

        throw std::runtime_error("print does not support type '" + argument.type.name() + "' in native codegen");
    }

    Type inferExprType(const Expr& expression) {
        if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expression)) {
            return integerLiteralFitsI32(parseIntegerLiteral(integer->value())) ? Type(TypeKind::I32) : Type(TypeKind::I64);
        }
        if (dynamic_cast<const FloatExpr*>(&expression)) return Type(TypeKind::F64);
        if (dynamic_cast<const StringExpr*>(&expression)) return Type(TypeKind::String);
        if (dynamic_cast<const CharExpr*>(&expression)) return Type(TypeKind::Char);
        if (dynamic_cast<const BoolExpr*>(&expression)) return Type(TypeKind::Bool);

        if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
            const std::size_t separator = variable->name().find("::");
            if (separator != std::string::npos) {
                return Type::enumOf(variable->name().substr(0, separator));
            }
            return lookupVariable(variable->name()).type;
        }

        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
            const std::string& op = binary->op();
            if (op == "&&" || op == "||" || op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
                return Type(TypeKind::Bool);
            }
            return commonType(inferExprType(binary->left()), inferExprType(binary->right()));
        }

        if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
            if (unary->op() == "!") {
                return Type(TypeKind::Bool);
            }
            if (unary->op() == "-") {
                return inferExprType(unary->operand());
            }
        }

        if (const auto* array = dynamic_cast<const ArrayExpr*>(&expression)) {
            if (array->elements().empty()) {
                throw std::runtime_error("cannot infer type for empty array literal");
            }

            Type elementType = inferExprType(*array->elements().front());
            for (std::size_t i = 1; i < array->elements().size(); ++i) {
                Type current = inferExprType(*array->elements()[i]);
                Type common = commonType(elementType, current);
                if (common.isUnknown()) {
                    if (!canImplicitlyConvert(current, elementType)) {
                        throw std::runtime_error("array literal has incompatible element types");
                    }
                } else {
                    elementType = common;
                }
            }

            return Type::arrayOf(elementType, array->elements().size());
        }

        if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
            Type target = inferExprType(index->target());
            if (!target.isArray() && !target.isVec()) {
                throw std::runtime_error("cannot index non-array value");
            }
            return target.elementType();
        }

        if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expression)) {
            Type target = inferExprType(field->target());
            if (target.kind() != TypeKind::Struct) {
                throw std::runtime_error("cannot access field on non-struct value");
            }
            const StructInfo& info = lookupStructInfo(target.structName());
            const auto found = info.fieldIndices.find(field->field());
            if (found == info.fieldIndices.end()) {
                throw std::runtime_error("struct '" + target.name() + "' has no field '" + field->field() + "'");
            }
            return Type::fromName(info.fields[found->second].type());
        }

        if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expression)) {
            return Type::structOf(literal->typeName());
        }

        if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
            const std::string builtinName = canonicalBuiltinName(call->callee());
            if (builtinName == "print") {
                return Type::voidType();
            }
            if (builtinName == "len") {
                return Type(TypeKind::I64);
            }
            if (builtinName == "input") {
                return Type(TypeKind::String);
            }
            if (builtinName == "arg_count") {
                return Type(TypeKind::I64);
            }
            if (builtinName == "arg_at") {
                return Type(TypeKind::String);
            }
            if (builtinName == "char_at") {
                return Type(TypeKind::Char);
            }
            if (builtinName == "substring") {
                return Type(TypeKind::String);
            }
            if (builtinName == "read_file") {
                return Type(TypeKind::String);
            }
            if (builtinName == "write_file") {
                return Type::voidType();
            }
            if (builtinName == "seed_i64") {
                return Type::voidType();
            }
            if (builtinName == "rand_i64") {
                return Type(TypeKind::I64);
            }
            if (builtinName == "vec_new") {
                return Type::unknown();
            }
            if (builtinName == "vec_push" || builtinName == "vec_clear" || builtinName == "vec_reserve") {
                if (!call->arguments().empty()) {
                    inferExprType(*call->arguments()[0]);
                }
                return Type::voidType();
            }
            if (builtinName == "vec_pop") {
                if (call->arguments().size() == 1) {
                    Type target = inferExprType(*call->arguments()[0]);
                    if (target.isVec()) {
                        return target.elementType();
                    }
                }
                return Type::unknown();
            }
            if (builtinName == "vec_set") {
                for (const auto& argument : call->arguments()) {
                    inferExprType(*argument);
                }
                return Type::voidType();
            }
            if (builtinName == "to_string_i64" || builtinName == "to_string_f64" || builtinName == "to_string_bool"
                || builtinName == "to_string_char") {
                for (const auto& argument : call->arguments()) {
                    inferExprType(*argument);
                }
                return Type(TypeKind::String);
            }
            if (builtinName == "append_str") {
                for (const auto& argument : call->arguments()) {
                    inferExprType(*argument);
                }
                return Type(TypeKind::String);
            }
            if (builtinName == "str_to_i64" || builtinName == "str_index_of" || builtinName == "f64_to_i64") {
                for (const auto& argument : call->arguments()) {
                    inferExprType(*argument);
                }
                return Type(TypeKind::I64);
            }
            if (builtinName == "str_to_f64" || builtinName == "sqrt_f64" || builtinName == "floor_f64"
                || builtinName == "ceil_f64" || builtinName == "round_f64") {
                for (const auto& argument : call->arguments()) {
                    inferExprType(*argument);
                }
                return Type(TypeKind::F64);
            }
            if (builtinName == "str_contains" || builtinName == "file_exists") {
                for (const auto& argument : call->arguments()) {
                    inferExprType(*argument);
                }
                return Type(TypeKind::Bool);
            }
            if (builtinName == "clock_ms" || builtinName == "mono_ms") {
                return Type(TypeKind::I64);
            }
            if (builtinName == "exit" || builtinName == "eprint") {
                for (const auto& argument : call->arguments()) {
                    inferExprType(*argument);
                }
                return Type::voidType();
            }
            const auto found = functions_.find(call->callee());
            if (found == functions_.end()) {
                throw std::runtime_error("unknown function '" + call->callee() + "'");
            }
            return found->second.returnType;
        }

        throw std::runtime_error("cannot infer expression type for native codegen");
    }

    ValueInfo convertToBool(ValueInfo value) {
        if (value.type.kind() == TypeKind::Bool) {
            return value;
        }
        if (value.type.isInteger()) {
            return {builder_.CreateICmpNE(value.value, llvm::ConstantInt::get(llvmType(value.type), 0, true)), Type(TypeKind::Bool)};
        }
        if (value.type.isFloat()) {
            return {builder_.CreateFCmpONE(value.value, llvm::ConstantFP::get(llvmType(value.type), 0.0)), Type(TypeKind::Bool)};
        }
        throw std::runtime_error("cannot convert '" + value.type.name() + "' to bool");
    }

    ValueInfo convertValue(ValueInfo value, Type target) {
        if (value.type == target) {
            return value;
        }

        // An unknown source type means inference was deferred (e.g.
        // `vec_new()`); Sema already validated the surrounding annotation.
        if (value.type.isUnknown()) {
            return {value.value, target};
        }

        if (value.type.kind() == TypeKind::Struct || target.kind() == TypeKind::Struct) {
            if (value.type == target) {
                return value;
            }
            throw std::runtime_error("cannot convert '" + value.type.name() + "' to '" + target.name() + "' in native codegen");
        }

        if (value.type.isArray() || target.isArray()) {
            throw std::runtime_error("cannot convert '" + value.type.name() + "' to '" + target.name() + "' in native codegen");
        }

        if (value.type.kind() == TypeKind::Bool && target.kind() == TypeKind::I32) {
            return {builder_.CreateZExt(value.value, llvmType(target)), target};
        }

        if (value.type.kind() == TypeKind::Bool && (target.kind() == TypeKind::Byte || target.kind() == TypeKind::Char)) {
            return {builder_.CreateZExt(value.value, llvmType(target)), target};
        }

        if (value.type.kind() == TypeKind::Byte && target.kind() == TypeKind::Char) {
            return {builder_.CreateZExt(value.value, llvmType(target)), target};
        }

        if (value.type.kind() == TypeKind::Char && target.kind() == TypeKind::Byte) {
            return {builder_.CreateTrunc(value.value, llvmType(target)), target};
        }

        if ((value.type.kind() == TypeKind::Byte || value.type.kind() == TypeKind::Char) && target.kind() == TypeKind::I32) {
            return {builder_.CreateZExt(value.value, llvmType(target)), target};
        }

        if ((value.type.kind() == TypeKind::Byte || value.type.kind() == TypeKind::Char) && target.kind() == TypeKind::I64) {
            return {builder_.CreateZExt(value.value, llvmType(target)), target};
        }

        if ((value.type.kind() == TypeKind::Byte || value.type.kind() == TypeKind::Char) && (target.kind() == TypeKind::F32 || target.kind() == TypeKind::F64)) {
            return {builder_.CreateUIToFP(value.value, llvmType(target)), target};
        }

        if (value.type.kind() == TypeKind::I32 && target.kind() == TypeKind::I64) {
            return {builder_.CreateSExt(value.value, llvmType(target)), target};
        }

        if (value.type.kind() == TypeKind::I32 && (target.kind() == TypeKind::F32 || target.kind() == TypeKind::F64)) {
            return {builder_.CreateSIToFP(value.value, llvmType(target)), target};
        }

        if (value.type.kind() == TypeKind::I64 && target.kind() == TypeKind::F64) {
            return {builder_.CreateSIToFP(value.value, llvmType(target)), target};
        }

        if (value.type.kind() == TypeKind::F32 && target.kind() == TypeKind::F64) {
            return {builder_.CreateFPExt(value.value, llvmType(target)), target};
        }

        if (value.type.isNumeric() && target.kind() == TypeKind::Bool) {
            return convertToBool(value);
        }

        throw std::runtime_error("cannot convert '" + value.type.name() + "' to '" + target.name() + "' in native codegen");
    }

    void pushScope() {
        scopes_.emplace_back();
    }

    void popScope() {
        scopes_.pop_back();
    }

    void declareVariable(const std::string& name, llvm::AllocaInst* alloca, Type type, bool isConst = false) {
        scopes_.back()[name] = VariableInfo{alloca, type, isConst};
    }

    VariableInfo lookupVariable(const std::string& name) const {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->find(name);
            if (found != scope->end()) {
                return found->second;
            }
        }
        throw std::runtime_error("undefined variable '" + name + "'");
    }

    void verifyModule() {
        std::string message;
        llvm::raw_string_ostream out(message);
        if (llvm::verifyModule(*module_, &out)) {
            throw std::runtime_error("LLVM module verification failed:\n" + out.str());
        }
    }

    void optimizeModule() {
        if (options_.optLevel <= 0) {
            return;
        }

        llvm::LoopAnalysisManager loopAnalyses;
        llvm::FunctionAnalysisManager functionAnalyses;
        llvm::CGSCCAnalysisManager cgsccAnalyses;
        llvm::ModuleAnalysisManager moduleAnalyses;
        llvm::PassBuilder passBuilder;

        passBuilder.registerModuleAnalyses(moduleAnalyses);
        passBuilder.registerCGSCCAnalyses(cgsccAnalyses);
        passBuilder.registerFunctionAnalyses(functionAnalyses);
        passBuilder.registerLoopAnalyses(loopAnalyses);
        passBuilder.crossRegisterProxies(loopAnalyses, functionAnalyses, cgsccAnalyses, moduleAnalyses);

        llvm::OptimizationLevel level = llvm::OptimizationLevel::O2;
        if (options_.optLevel == 1) {
            level = llvm::OptimizationLevel::O1;
        } else if (options_.optLevel >= 3) {
            level = llvm::OptimizationLevel::O3;
        }

        llvm::ModulePassManager modulePasses = passBuilder.buildPerModuleDefaultPipeline(level);
        modulePasses.run(*module_, moduleAnalyses);
    }

    std::filesystem::path irPath() const {
        if (options_.emitIr) {
            return std::filesystem::path(options_.outputPath);
        }

        if (options_.keepIr) {
            std::filesystem::path path = options_.outputPath;
            if (options_.emitObj || options_.emitAsm) {
                return path.replace_extension(".ll");
            }
            return path.replace_extension(".ll");
        }

        std::filesystem::path temp = std::filesystem::temp_directory_path();
        std::string base = std::filesystem::path(options_.outputPath).filename().replace_extension(".ll").string();
        return temp / base;
    }

    void writeIr() {
        const std::filesystem::path path = irPath();
        result_.irPath = path.string();

        std::error_code error;
        llvm::raw_fd_ostream out(path.string(), error, llvm::sys::fs::OF_Text);
        if (error) {
            throw std::runtime_error("failed to write LLVM IR '" + path.string() + "': " + error.message());
        }

        module_->print(out, nullptr);
    }

    bool runClang(const std::vector<std::string>& argumentsStorage, const std::string& failureContext) {
        llvm::ErrorOr<std::string> clang = llvm::sys::findProgramByName(options_.clangPath);
        if (!clang) {
            report("failed to find clang executable '" + options_.clangPath + "': " + clang.getError().message());
            return false;
        }

        std::vector<llvm::StringRef> arguments;
        arguments.reserve(argumentsStorage.size());
        for (const std::string& argument : argumentsStorage) {
            arguments.push_back(argument);
        }

        std::string errorMessage;
        bool executionFailed = false;
        const int exitCode = llvm::sys::ExecuteAndWait(
            *clang,
            arguments,
            std::nullopt,
            {},
            0,
            0,
            &errorMessage,
            &executionFailed);
        if (exitCode != 0) {
            std::ostringstream message;
            if (executionFailed) {
                message << "failed to execute clang";
            } else {
                message << "clang failed while " << failureContext << ", exit code " << exitCode;
            }
            if (!errorMessage.empty()) {
                message << ": " << errorMessage;
            }
            report(message.str());
            return false;
        }

        return true;
    }

    bool emitNativeArtifact() {
        llvm::ErrorOr<std::string> clang = llvm::sys::findProgramByName(options_.clangPath);
        if (!clang) {
            report("failed to find clang executable '" + options_.clangPath + "': " + clang.getError().message());
            return false;
        }

        std::vector<std::string> argumentsStorage;
        argumentsStorage.push_back(*clang);
        if (!options_.targetTriple.empty()) {
            argumentsStorage.push_back("--target=" + options_.targetTriple);
        }
        if (options_.optLevel > 0) {
            argumentsStorage.push_back("-O" + std::to_string(options_.optLevel));
        }
        argumentsStorage.push_back("-Wno-override-module");
        if (options_.emitObj) {
            argumentsStorage.push_back("-c");
        } else {
            argumentsStorage.push_back("-S");
        }
        argumentsStorage.push_back(result_.irPath);
        argumentsStorage.push_back("-o");
        argumentsStorage.push_back(options_.outputPath);

        if (options_.emitObj) {
            result_.objPath = options_.outputPath;
            return runClang(argumentsStorage, "emitting object file");
        }

        result_.asmPath = options_.outputPath;
        return runClang(argumentsStorage, "emitting assembly");
    }

    bool linkExecutable() {
        llvm::ErrorOr<std::string> clang = llvm::sys::findProgramByName(options_.clangPath);
        if (!clang) {
            report("failed to find clang executable '" + options_.clangPath + "': " + clang.getError().message());
            return false;
        }

        std::vector<std::string> argumentsStorage;
        argumentsStorage.push_back(*clang);
        if (!options_.targetTriple.empty()) {
            argumentsStorage.push_back("--target=" + options_.targetTriple);
        }
        if (options_.optLevel > 0) {
            argumentsStorage.push_back("-O" + std::to_string(options_.optLevel));
        }
        argumentsStorage.push_back("-Wno-override-module");
        argumentsStorage.push_back(result_.irPath);
        argumentsStorage.push_back("-o");
        argumentsStorage.push_back(options_.outputPath);

        return runClang(argumentsStorage, "linking native executable");
    }

    void cleanupIntermediateIr() {
        if (options_.keepIr || result_.irPath.empty()) {
            return;
        }

        std::error_code ignored;
        std::filesystem::remove(result_.irPath, ignored);
        result_.irPath.clear();
    }

    void report(const std::string& message) {
        result_.errors.push_back(message);
    }

    NativeCompileOptions options_;
    NativeCompileResult result_;
    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    llvm::Function* printf_ = nullptr;
    llvm::Function* strLen_ = nullptr;
    llvm::Function* strConcat_ = nullptr;
    llvm::Function* strAppend_ = nullptr;
    llvm::Function* charAt_ = nullptr;
    llvm::Function* substring_ = nullptr;
    llvm::Function* toStringI64_ = nullptr;
    llvm::Function* toStringF64_ = nullptr;
    llvm::Function* readFile_ = nullptr;
    llvm::Function* writeFile_ = nullptr;
    llvm::Function* argAt_ = nullptr;
    llvm::Function* strcmp_ = nullptr;
    llvm::Function* runCommand_ = nullptr;
    llvm::Function* malloc_ = nullptr;
    llvm::Function* realloc_ = nullptr;
    llvm::Function* memcpy_ = nullptr;
    llvm::Function* eprintStr_ = nullptr;
    llvm::Function* input_ = nullptr;
    llvm::Function* runtimeError_ = nullptr;
    llvm::Function* fopen_ = nullptr;
    llvm::Function* fgetc_ = nullptr;
    llvm::Function* fwrite_ = nullptr;
    llvm::Function* fclose_ = nullptr;
    llvm::Function* exit_ = nullptr;
    llvm::Function* snprintf_ = nullptr;
    llvm::Function* fprintf_ = nullptr;
    llvm::Function* strtoll_ = nullptr;
    llvm::Function* strtod_ = nullptr;
    llvm::Function* strIndexOf_ = nullptr;
    llvm::Function* strContains_ = nullptr;
    llvm::Function* clockMs_ = nullptr;
    llvm::Function* monoMs_ = nullptr;
    llvm::GlobalVariable* programArgc_ = nullptr;
    llvm::GlobalVariable* programArgv_ = nullptr;
    llvm::GlobalVariable* rngState_ = nullptr;
    std::unordered_map<std::string, FunctionInfo> functions_;
    std::unordered_map<std::string, StructInfo> structs_;
    std::unordered_map<std::string, EnumInfo> enums_;
    llvm::StructType* vecHeaderType_ = nullptr;
    std::vector<std::unordered_map<std::string, VariableInfo>> scopes_;
    std::vector<LoopTargets> loopTargets_;
    FunctionInfo* currentFunction_ = nullptr;
};

} // namespace

NativeCompileResult compileNativeExecutable(const Program& program, const NativeCompileOptions& options) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    LlvmNativeCompiler compiler(options);
    return compiler.compile(program);
}

} // namespace miko

#endif // MIKO_ENABLE_LLVM
