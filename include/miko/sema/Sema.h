#pragma once

#include "miko/ast/Ast.h"
#include "miko/sema/Type.h"

#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace miko {

struct SymbolInfo {
    std::string name;
    Type type;
    bool isConst = false;
};

struct FunctionSignature {
    std::string name;
    std::vector<Type> parameters;
    Type returnType;
};

struct StructInfo {
    std::string name;
    std::vector<Parameter> fields;
    std::unordered_map<std::string, std::size_t> fieldIndices;
};

struct EnumInfo {
    std::string name;
    std::vector<std::string> members;
    std::unordered_map<std::string, std::size_t> memberIndices;
};

class Sema {
public:
    bool analyze(const Program& program);
    const std::vector<std::string>& errors() const;
    void dumpTypes(std::ostream& out) const;

private:
    void analyzeFunction(const FunctionDecl& function);
    void registerStruct(const StructDecl& structure);
    void registerEnum(const EnumDecl& enumeration);
    void registerFunction(const FunctionDecl& function);
    const StructInfo* lookupStruct(const std::string& name) const;
    const EnumInfo* lookupEnum(const std::string& name) const;
    Type resolveType(Type type) const;
    void analyzeBlock(const BlockStmt& block);
    void analyzeStmt(const Stmt& statement);
    Type inferExpr(const Expr& expression);

    void pushScope();
    void popScope();
    bool declareSymbol(const std::string& name, Type type, bool isConst = false);
    const SymbolInfo* lookupSymbol(const std::string& name) const;
    void report(const std::string& message);
    void report(const AstNode& node, const std::string& message);
    void record(const std::string& message);

    std::vector<std::unordered_map<std::string, SymbolInfo>> scopes_;
    std::unordered_map<std::string, StructInfo> structs_;
    std::unordered_map<std::string, EnumInfo> enums_;
    std::unordered_map<std::string, FunctionSignature> functions_;
    Type currentReturnType_ = Type::voidType();
    bool entryFunctionSeen_ = false;
    int loopDepth_ = 0;
    std::vector<std::string> errors_;
    std::vector<std::string> typeLog_;
};

} // namespace miko
