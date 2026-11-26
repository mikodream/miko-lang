#pragma once

#include "miko/ast/Ast.h"
#include "miko/runtime/Value.h"

#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace miko {

class Interpreter {
public:
    explicit Interpreter(std::ostream& out, std::vector<std::string> programArgs = {});

    bool run(const Program& program);
    const std::vector<std::string>& errors() const;

private:
    struct VariableInfo {
        Value value;
        bool isConst = false;
    };

    struct ReturnSignal {
        Value value;
    };
    struct BreakSignal {
    };
    struct ContinueSignal {
    };

    void registerFunctions(const Program& program);
    Value callFunction(const std::string& name, const std::vector<Value>& arguments);
    void executeBlock(const BlockStmt& block, bool createScope = true);
    void executeStmt(const Stmt& statement);
    Value evalExpr(const Expr& expression);

    void pushScope();
    void popScope();
    void declareVariable(const std::string& name, Value value, bool isConst = false);
    bool assignVariable(const std::string& name, Value value);
    bool assignArrayElement(const std::string& name, long long index, Value value);
    Value lookupVariable(const std::string& name) const;
    void report(const std::string& message);

    static Value applyBinary(const std::string& op, const Value& left, const Value& right);
    static std::string unquoteString(const std::string& value);
    static char unquoteChar(const std::string& value);

    std::ostream& out_;
    std::vector<std::string> programArgs_;
    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, std::unordered_map<std::string, long long>> enums_;
    std::vector<std::unordered_map<std::string, VariableInfo>> scopes_;
    std::vector<std::string> errors_;
    long long rngSeed_;
};

} // namespace miko
