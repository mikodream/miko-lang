#include "miko/sema/Sema.h"

#include "miko/Builtins.h"

#include <sstream>
#include <unordered_set>

namespace miko {

bool Sema::analyze(const Program& program) {
    errors_.clear();
    typeLog_.clear();
    scopes_.clear();
    structs_.clear();
    enums_.clear();
    functions_.clear();
    entryFunctionSeen_ = false;
    loopDepth_ = 0;

    for (const auto& structure : program.structs()) {
        registerStruct(*structure);
    }

    for (const auto& enumeration : program.enums()) {
        registerEnum(*enumeration);
    }

    for (const auto& function : program.functions()) {
        registerFunction(*function);
    }

    for (const auto& function : program.functions()) {
        analyzeFunction(*function);
    }

    return errors_.empty();
}

const std::vector<std::string>& Sema::errors() const {
    return errors_;
}

void Sema::dumpTypes(std::ostream& out) const {
    for (const std::string& line : typeLog_) {
        out << line << '\n';
    }
}

void Sema::analyzeFunction(const FunctionDecl& function) {
    currentReturnType_ = function.returnType().empty() ? Type::voidType() : resolveType(Type::fromName(function.returnType()));
    if (currentReturnType_.kind() == TypeKind::Struct && !lookupStruct(currentReturnType_.structName())) {
        report(function, "unknown return type '" + function.returnType() + "' for function '" + function.name() + "'");
    }
    record("function " + function.name() + " -> " + currentReturnType_.name());
    pushScope();

    for (const Parameter& parameter : function.parameters()) {
        Type type = resolveType(Type::fromName(parameter.type()));
        if (type.isUnknown() || (type.kind() == TypeKind::Struct && !lookupStruct(type.structName()))) {
            report(function, "unknown parameter type '" + parameter.type() + "' for '" + parameter.name() + "'");
        }
        declareSymbol(parameter.name(), type);
        record("  param " + parameter.name() + ": " + type.name());
    }

    analyzeBlock(function.body());
    popScope();
}

void Sema::registerStruct(const StructDecl& structure) {
    if (structs_.find(structure.name()) != structs_.end()) {
        report(structure, "struct '" + structure.name() + "' is already declared");
        return;
    }

    if (enums_.find(structure.name()) != enums_.end()) {
        report(structure, "struct '" + structure.name() + "' conflicts with an enum of the same name");
        return;
    }

    StructInfo info;
    info.name = structure.name();
    info.fields = structure.fields();
    for (std::size_t i = 0; i < info.fields.size(); ++i) {
        const Parameter& field = info.fields[i];
        if (info.fieldIndices.find(field.name()) != info.fieldIndices.end()) {
            report(structure, "field '" + field.name() + "' is already declared in struct '" + structure.name() + "'");
            continue;
        }
        Type fieldType = resolveType(Type::fromName(field.type()));
        if (fieldType.isUnknown() || (fieldType.kind() == TypeKind::Struct && fieldType.structName() != structure.name() && !lookupStruct(fieldType.structName()))) {
            report(structure, "unknown field type '" + field.type() + "' for '" + field.name() + "'");
        }
        info.fieldIndices[field.name()] = i;
    }

    structs_.emplace(structure.name(), std::move(info));
    record("struct " + structure.name());
}

void Sema::registerEnum(const EnumDecl& enumeration) {
    if (enums_.find(enumeration.name()) != enums_.end()) {
        report(enumeration, "enum '" + enumeration.name() + "' is already declared");
        return;
    }

    if (structs_.find(enumeration.name()) != structs_.end()) {
        report(enumeration, "enum '" + enumeration.name() + "' conflicts with a struct of the same name");
        return;
    }

    if (enumeration.members().empty()) {
        report(enumeration, "enum '" + enumeration.name() + "' must declare at least one member");
        return;
    }

    EnumInfo info;
    info.name = enumeration.name();
    info.members = enumeration.members();
    for (std::size_t i = 0; i < info.members.size(); ++i) {
        info.memberIndices[info.members[i]] = i;
    }

    enums_.emplace(enumeration.name(), std::move(info));
    record("enum " + enumeration.name());
}

void Sema::registerFunction(const FunctionDecl& function) {
    if (isBuiltinFunctionName(function.name())) {
        report(function, "built-in function '" + function.name() + "' cannot be redefined");
        return;
    }

    if (isEntryFunctionName(function.name())) {
        if (entryFunctionSeen_) {
            report(function, "entry function '" + function.name() + "' conflicts with another entry function");
            return;
        }
        entryFunctionSeen_ = true;
    }

    if (functions_.find(function.name()) != functions_.end()) {
        report(function, "function '" + function.name() + "' is already declared");
        return;
    }

    FunctionSignature signature;
    signature.name = function.name();
    signature.returnType = function.returnType().empty() ? Type::voidType() : resolveType(Type::fromName(function.returnType()));

    if (!function.returnType().empty() && (signature.returnType.isUnknown()
        || (signature.returnType.kind() == TypeKind::Struct && !lookupStruct(signature.returnType.structName())))) {
        report(function, "unknown return type '" + function.returnType() + "' for function '" + function.name() + "'");
    }

    for (const Parameter& parameter : function.parameters()) {
        Type type = resolveType(Type::fromName(parameter.type()));
        if (type.isUnknown() || (type.kind() == TypeKind::Struct && !lookupStruct(type.structName()))) {
            report(function, "unknown parameter type '" + parameter.type() + "' for '" + parameter.name() + "'");
        }
        signature.parameters.push_back(type);
    }

    functions_.emplace(function.name(), std::move(signature));
}

void Sema::analyzeBlock(const BlockStmt& block) {
    pushScope();
    for (const auto& statement : block.statements()) {
        analyzeStmt(*statement);
    }
    popScope();
}

void Sema::analyzeStmt(const Stmt& statement) {
    if (const auto* let = dynamic_cast<const LetStmt*>(&statement)) {
        Type inferred = Type::unknown();
        if (const Expr* initializer = let->initializer()) {
            inferred = inferExpr(*initializer);
        }

        Type declared = let->explicitType().empty() ? inferred : resolveType(Type::fromName(let->explicitType()));
        if (!let->explicitType().empty() && (declared.isUnknown() || (declared.kind() == TypeKind::Struct && !lookupStruct(declared.structName())))) {
            report(*let, "unknown type '" + let->explicitType() + "' for variable '" + let->name() + "'");
        }

        if (let->initializer() && inferred.isArray() && declared.isArray()) {
            if (inferred.arrayLength() != declared.arrayLength()) {
                report(*let, "cannot initialize variable '" + let->name() + "' of type '" + declared.name()
                       + "' with array of type '" + inferred.name() + "'");
            } else if (!canImplicitlyConvert(inferred.elementType(), declared.elementType())) {
                report(*let, "cannot initialize array '" + let->name() + "' with element type '" + inferred.elementType().name()
                       + "', expected '" + declared.elementType().name() + "'");
            }
        } else if (let->initializer() && !canImplicitlyConvert(inferred, declared)) {
            report(*let, "cannot initialize variable '" + let->name() + "' of type '" + declared.name() + "' with '" + inferred.name() + "'");
        }

        if (declared.isUnknown()) {
            report(*let, "cannot infer type for variable '" + let->name() + "'");
        }

        if (let->isConst() && !let->initializer()) {
            report(*let, "const variable '" + let->name() + "' requires an initializer");
        }

        declareSymbol(let->name(), declared, let->isConst());
        record(std::string("  ") + (let->isConst() ? "const " : "let ") + let->name() + ": " + declared.name());
        return;
    }

    if (const auto* assign = dynamic_cast<const IndexAssignStmt*>(&statement)) {
        const SymbolInfo* targetSymbol = lookupSymbol(assign->name());
        Type target = targetSymbol ? targetSymbol->type : Type::unknown();
        if (target.isUnknown()) {
            report(*assign, "undefined variable '" + assign->name() + "'");
        } else if (!target.isArray() && !target.isVec()) {
            report(*assign, "cannot index-assign non-array variable '" + assign->name() + "' of type '" + target.name() + "'");
        } else if (targetSymbol->isConst) {
            report(*assign, "cannot assign to const array variable '" + assign->name() + "'");
        }

        Type indexType = inferExpr(assign->index());
        if (!indexType.isInteger()) {
            report(assign->index(), "array index must be an integer, got '" + indexType.name() + "'");
        }

        Type value = inferExpr(assign->value());
        if (target.isArray() && !canImplicitlyConvert(value, target.elementType())) {
            report(assign->value(), "cannot assign '" + value.name() + "' to array element of type '" + target.elementType().name() + "'");
        }

        record("  index assign " + assign->name() + ": " + ((target.isArray() || target.isVec()) ? target.elementType().name() : target.name()));
        return;
    }

    if (const auto* assign = dynamic_cast<const AssignStmt*>(&statement)) {
        const SymbolInfo* targetSymbol = lookupSymbol(assign->name());
        Type target = targetSymbol ? targetSymbol->type : Type::unknown();
        if (target.isUnknown()) {
            report(*assign, "undefined variable '" + assign->name() + "'");
        } else if (targetSymbol->isConst) {
            report(*assign, "cannot assign to const variable '" + assign->name() + "'");
        }

        Type value = inferExpr(assign->value());
        if (!canImplicitlyConvert(value, target)) {
            report(assign->value(), "cannot assign '" + value.name() + "' to variable '" + assign->name() + "' of type '" + target.name() + "'");
        }

        record("  assign " + assign->name() + ": " + target.name());
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&statement)) {
        Type type = inferExpr(exprStmt->expression());
        record("  expr: " + type.name());
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&statement)) {
        Type type = returnStmt->value() ? inferExpr(*returnStmt->value()) : Type::voidType();
        if (!canImplicitlyConvert(type, currentReturnType_)) {
            report(*returnStmt, "cannot return '" + type.name() + "' from function returning '" + currentReturnType_.name() + "'");
        }
        record("  return: " + type.name());
        return;
    }

    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
        analyzeBlock(*block);
        return;
    }

    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
        Type condition = inferExpr(ifStmt->condition());
        if (!canImplicitlyConvert(condition, Type(TypeKind::Bool))) {
            report(ifStmt->condition(), "if condition must be bool-compatible, got '" + condition.name() + "'");
        }
        record("  if condition: " + condition.name());
        analyzeBlock(ifStmt->thenBlock());
        if (const BlockStmt* elseBlock = ifStmt->elseBlock()) {
            analyzeBlock(*elseBlock);
        }
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
        Type condition = inferExpr(whileStmt->condition());
        if (!canImplicitlyConvert(condition, Type(TypeKind::Bool))) {
            report(whileStmt->condition(), "while condition must be bool-compatible, got '" + condition.name() + "'");
        }
        record("  while condition: " + condition.name());
        ++loopDepth_;
        analyzeBlock(whileStmt->body());
        --loopDepth_;
        return;
    }

    if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
        pushScope();
        if (const Stmt* initializer = forStmt->initializer()) {
            analyzeStmt(*initializer);
        }
        if (const Expr* conditionExpr = forStmt->condition()) {
            Type condition = inferExpr(*conditionExpr);
            if (!canImplicitlyConvert(condition, Type(TypeKind::Bool))) {
                report(*conditionExpr, "for condition must be bool-compatible, got '" + condition.name() + "'");
            }
            record("  for condition: " + condition.name());
        }
        if (const Stmt* increment = forStmt->increment()) {
            analyzeStmt(*increment);
        }
        ++loopDepth_;
        analyzeBlock(forStmt->body());
        --loopDepth_;
        popScope();
        return;
    }

    if (dynamic_cast<const BreakStmt*>(&statement)) {
        if (loopDepth_ == 0) {
            report(statement, "break can only be used inside a loop");
        }
        record("  break");
        return;
    }

    if (dynamic_cast<const ContinueStmt*>(&statement)) {
        if (loopDepth_ == 0) {
            report(statement, "continue can only be used inside a loop");
        }
        record("  continue");
        return;
    }

    report(statement, "unknown statement node");
}

Type Sema::inferExpr(const Expr& expression) {
    if (dynamic_cast<const IntegerExpr*>(&expression)) {
        return Type(TypeKind::I32);
    }

    if (dynamic_cast<const FloatExpr*>(&expression)) {
        return Type(TypeKind::F64);
    }

    if (dynamic_cast<const StringExpr*>(&expression)) {
        return Type(TypeKind::String);
    }

    if (dynamic_cast<const CharExpr*>(&expression)) {
        return Type(TypeKind::Char);
    }

    if (dynamic_cast<const BoolExpr*>(&expression)) {
        return Type(TypeKind::Bool);
    }

    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
        const std::string& name = variable->name();
        const std::size_t separator = name.find("::");
        if (separator != std::string::npos) {
            const std::string enumName = name.substr(0, separator);
            const std::string memberName = name.substr(separator + 2);
            const EnumInfo* info = lookupEnum(enumName);
            if (!info) {
                report(*variable, "unknown enum '" + enumName + "'");
                return Type::unknown();
            }
            if (info->memberIndices.find(memberName) == info->memberIndices.end()) {
                report(*variable, "enum '" + enumName + "' has no member '" + memberName + "'");
                return Type::unknown();
            }
            return Type::enumOf(enumName);
        }

        const SymbolInfo* symbol = lookupSymbol(name);
        Type type = symbol ? symbol->type : Type::unknown();
        if (type.isUnknown()) {
            report(*variable, "undefined variable '" + name + "'");
        }
        return type;
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        Type left = inferExpr(binary->left());
        Type right = inferExpr(binary->right());

        const std::string& op = binary->op();
        if (op == "+" && left.kind() == TypeKind::String && right.kind() == TypeKind::String) {
            return Type(TypeKind::String);
        }

        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            Type result = commonType(left, right);
            if (result.isUnknown()) {
                report(*binary, "operator '" + op + "' cannot be applied to '" + left.name() + "' and '" + right.name() + "'");
            }
            return result;
        }

        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (left.kind() == TypeKind::String && right.kind() == TypeKind::String) {
                return Type(TypeKind::Bool);
            }

            Type result = commonType(left, right);
            if (result.isUnknown() && left != right) {
                report(*binary, "comparison '" + op + "' cannot be applied to '" + left.name() + "' and '" + right.name() + "'");
            }
            return Type(TypeKind::Bool);
        }

        if (op == "&&" || op == "||") {
            if (!canImplicitlyConvert(left, Type(TypeKind::Bool)) || !canImplicitlyConvert(right, Type(TypeKind::Bool))) {
                report(*binary, "logical operator '" + op + "' requires bool-compatible operands");
            }
            return Type(TypeKind::Bool);
        }

        report(*binary, "unknown binary operator '" + op + "'");
        return Type::unknown();
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        Type operand = inferExpr(unary->operand());
        if (unary->op() == "!") {
            if (!canImplicitlyConvert(operand, Type(TypeKind::Bool))) {
                report(*unary, "unary '!' requires a bool-compatible operand, got '" + operand.name() + "'");
            }
            return Type(TypeKind::Bool);
        }
        if (unary->op() == "-") {
            if (!operand.isNumeric()) {
                report(*unary, "unary '-' requires a numeric operand, got '" + operand.name() + "'");
            }
            return operand;
        }
        report(*unary, "unknown unary operator '" + unary->op() + "'");
        return Type::unknown();
    }

    if (const auto* array = dynamic_cast<const ArrayExpr*>(&expression)) {
        if (array->elements().empty()) {
            report(*array, "cannot infer type for empty array literal");
            return Type::unknown();
        }

        Type elementType = inferExpr(*array->elements().front());
        for (std::size_t i = 1; i < array->elements().size(); ++i) {
            Type current = inferExpr(*array->elements()[i]);
            Type common = commonType(elementType, current);
            if (common.isUnknown()) {
                if (!canImplicitlyConvert(current, elementType)) {
                    report(*array->elements()[i], "array element " + std::to_string(i + 1) + " has type '" + current.name()
                           + "', expected compatible with '" + elementType.name() + "'");
                }
            } else {
                elementType = common;
            }
        }

        return Type::arrayOf(elementType, array->elements().size());
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
        Type target = inferExpr(index->target());
        Type indexType = inferExpr(index->index());

        if (!indexType.isInteger()) {
            report(index->index(), "array index must be an integer, got '" + indexType.name() + "'");
        }

        if (!target.isArray() && !target.isVec()) {
            report(index->target(), "cannot index non-array value of type '" + target.name() + "'");
            return Type::unknown();
        }

        return target.elementType();
    }

    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expression)) {
        Type target = inferExpr(field->target());
        if (target.kind() != TypeKind::Struct) {
            report(field->target(), "cannot access field '" + field->field() + "' on non-struct value of type '" + target.name() + "'");
            return Type::unknown();
        }

        const StructInfo* info = lookupStruct(target.structName());
        if (!info) {
            report(field->target(), "unknown struct type '" + target.structName() + "'");
            return Type::unknown();
        }

        const auto found = info->fieldIndices.find(field->field());
        if (found == info->fieldIndices.end()) {
            report(*field, "struct '" + target.name() + "' has no field '" + field->field() + "'");
            return Type::unknown();
        }

        return Type::fromName(info->fields[found->second].type());
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expression)) {
        const StructInfo* info = lookupStruct(literal->typeName());
        if (!info) {
            report(*literal, "unknown struct type '" + literal->typeName() + "'");
            for (const auto& field : literal->fields()) {
                inferExpr(*field.second);
            }
            return Type::structOf(literal->typeName());
        }

        std::unordered_set<std::string> seen;
        for (const auto& field : literal->fields()) {
            if (!seen.insert(field.first).second) {
                report(*field.second, "field '" + field.first + "' is initialized more than once");
            }

            const auto found = info->fieldIndices.find(field.first);
            Type valueType = inferExpr(*field.second);
            if (found == info->fieldIndices.end()) {
                report(*field.second, "struct '" + literal->typeName() + "' has no field '" + field.first + "'");
                continue;
            }

            Type expected = Type::fromName(info->fields[found->second].type());
            if (!canImplicitlyConvert(valueType, expected)) {
                report(*field.second, "field '" + field.first + "' expects '" + expected.name() + "', got '" + valueType.name() + "'");
            }
        }

        for (const Parameter& field : info->fields) {
            if (seen.find(field.name()) == seen.end()) {
                report(*literal, "missing field '" + field.name() + "' for struct '" + literal->typeName() + "'");
            }
        }

        return Type::structOf(literal->typeName());
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
        const std::string builtinName = canonicalBuiltinName(call->callee());

        if (builtinName == "print") {
            if (call->arguments().size() != 1) {
                report(*call, "print expects exactly one argument");
            }
            for (const auto& argument : call->arguments()) {
                inferExpr(*argument);
            }
            return Type::voidType();
        }

        if (builtinName == "len") {
            if (call->arguments().size() != 1) {
                report(*call, "len expects exactly one argument");
                return Type(TypeKind::I64);
            }

            Type argumentType = inferExpr(*call->arguments().front());
            if (argumentType.kind() != TypeKind::String && !argumentType.isArray() && !argumentType.isVec()) {
                report(*call->arguments().front(), "len expects a string, array, or vec, got '" + argumentType.name() + "'");
            }
            return Type(TypeKind::I64);
        }

        if (builtinName == "input") {
            if (!call->arguments().empty()) {
                report(*call, "input expects no arguments");
            }
            return Type(TypeKind::String);
        }

        if (builtinName == "arg_count") {
            if (!call->arguments().empty()) {
                report(*call, "arg_count expects no arguments");
            }
            return Type(TypeKind::I64);
        }

        if (builtinName == "arg_at") {
            if (call->arguments().size() != 1) {
                report(*call, "arg_at expects exactly one argument");
                return Type(TypeKind::String);
            }
            Type indexType = inferExpr(*call->arguments()[0]);
            if (!indexType.isInteger()) {
                report(*call->arguments()[0], "arg_at expects an integer index, got '" + indexType.name() + "'");
            }
            return Type(TypeKind::String);
        }

        if (builtinName == "char_at") {
            if (call->arguments().size() != 2) {
                report(*call, "char_at expects exactly two arguments");
                return Type(TypeKind::Char);
            }
            Type textType = inferExpr(*call->arguments()[0]);
            Type indexType = inferExpr(*call->arguments()[1]);
            if (textType.kind() != TypeKind::String) {
                report(*call->arguments()[0], "char_at expects first argument to be string, got '" + textType.name() + "'");
            }
            if (!indexType.isInteger()) {
                report(*call->arguments()[1], "char_at expects second argument to be integer, got '" + indexType.name() + "'");
            }
            return Type(TypeKind::Char);
        }

        if (builtinName == "substring") {
            if (call->arguments().size() != 3) {
                report(*call, "substring expects exactly three arguments");
                return Type(TypeKind::String);
            }
            Type textType = inferExpr(*call->arguments()[0]);
            Type startType = inferExpr(*call->arguments()[1]);
            Type lengthType = inferExpr(*call->arguments()[2]);
            if (textType.kind() != TypeKind::String) {
                report(*call->arguments()[0], "substring expects first argument to be string, got '" + textType.name() + "'");
            }
            if (!startType.isInteger()) {
                report(*call->arguments()[1], "substring expects second argument to be integer, got '" + startType.name() + "'");
            }
            if (!lengthType.isInteger()) {
                report(*call->arguments()[2], "substring expects third argument to be integer, got '" + lengthType.name() + "'");
            }
            return Type(TypeKind::String);
        }

        if (builtinName == "read_file") {
            if (call->arguments().size() != 1) {
                report(*call, "read_file expects exactly one argument");
                return Type(TypeKind::String);
            }
            Type pathType = inferExpr(*call->arguments()[0]);
            if (pathType.kind() != TypeKind::String) {
                report(*call->arguments()[0], "read_file expects a string path, got '" + pathType.name() + "'");
            }
            return Type(TypeKind::String);
        }

        if (builtinName == "run_command") {
            if (call->arguments().size() != 1) {
                report(*call, "run_command expects exactly one argument");
                return Type(TypeKind::I64);
            }
            Type commandType = inferExpr(*call->arguments()[0]);
            if (commandType.kind() != TypeKind::String) {
                report(*call->arguments()[0], "run_command expects a string command, got '" + commandType.name() + "'");
            }
            return Type(TypeKind::I64);
        }

        if (builtinName == "write_file") {
            if (call->arguments().size() != 2) {
                report(*call, "write_file expects exactly two arguments");
                return Type::voidType();
            }
            Type pathType = inferExpr(*call->arguments()[0]);
            Type contentType = inferExpr(*call->arguments()[1]);
            if (pathType.kind() != TypeKind::String) {
                report(*call->arguments()[0], "write_file expects first argument to be string, got '" + pathType.name() + "'");
            }
            if (contentType.kind() != TypeKind::String) {
                report(*call->arguments()[1], "write_file expects second argument to be string, got '" + contentType.name() + "'");
            }
            return Type::voidType();
        }

        if (builtinName == "seed_i64") {
            if (call->arguments().size() != 1) {
                report(*call, "seed_i64 expects exactly one argument");
                return Type::voidType();
            }
            Type argType = inferExpr(*call->arguments()[0]);
            if (!argType.isInteger()) {
                report(*call->arguments()[0], "seed_i64 expects an integer, got '" + argType.name() + "'");
            }
            return Type::voidType();
        }

        if (builtinName == "rand_i64") {
            if (!call->arguments().empty()) {
                report(*call, "rand_i64 expects no arguments");
                return Type(TypeKind::I64);
            }
            return Type(TypeKind::I64);
        }

        if (builtinName == "vec_new") {
            if (!call->arguments().empty()) {
                report(*call, "vec_new expects no arguments");
            }
            // Element type is only known from the surrounding annotation;
            // require `let v: vec<T> = vec_new();`.
            return Type::unknown();
        }

        if (builtinName == "vec_push") {
            if (call->arguments().size() != 2) {
                report(*call, "vec_push expects exactly two arguments");
                for (const auto& argument : call->arguments()) {
                    inferExpr(*argument);
                }
                return Type::voidType();
            }
            Type target = inferExpr(*call->arguments()[0]);
            Type value = inferExpr(*call->arguments()[1]);
            if (target.isUnknown()) {
                report(*call->arguments()[0], "vec_push expects a vec with a known element type");
            } else if (!target.isVec()) {
                report(*call->arguments()[0], "vec_push expects a vec, got '" + target.name() + "'");
            } else if (!canImplicitlyConvert(value, target.elementType())) {
                report(*call->arguments()[1], "cannot push '" + value.name() + "' into vec of type '" + target.name() + "'");
            }
            return Type::voidType();
        }

        if (builtinName == "vec_pop") {
            if (call->arguments().size() != 1) {
                report(*call, "vec_pop expects exactly one argument");
                return Type::unknown();
            }
            Type target = inferExpr(*call->arguments()[0]);
            if (target.isUnknown()) {
                report(*call->arguments()[0], "vec_pop expects a vec with a known element type");
                return Type::unknown();
            }
            if (!target.isVec()) {
                report(*call->arguments()[0], "vec_pop expects a vec, got '" + target.name() + "'");
                return Type::unknown();
            }
            return target.elementType();
        }

        if (builtinName == "vec_set") {
            if (call->arguments().size() != 3) {
                report(*call, "vec_set expects exactly three arguments");
                for (const auto& argument : call->arguments()) {
                    inferExpr(*argument);
                }
                return Type::voidType();
            }
            Type target = inferExpr(*call->arguments()[0]);
            Type indexType = inferExpr(*call->arguments()[1]);
            Type value = inferExpr(*call->arguments()[2]);
            if (!indexType.isInteger()) {
                report(*call->arguments()[1], "vec index must be an integer, got '" + indexType.name() + "'");
            }
            if (target.isUnknown()) {
                report(*call->arguments()[0], "vec_set expects a vec with a known element type");
            } else if (!target.isVec()) {
                report(*call->arguments()[0], "vec_set expects a vec, got '" + target.name() + "'");
            } else if (!canImplicitlyConvert(value, target.elementType())) {
                report(*call->arguments()[2], "cannot assign '" + value.name() + "' to vec element of type '" + target.elementType().name() + "'");
            }
            return Type::voidType();
        }

        if (builtinName == "vec_clear" || builtinName == "vec_reserve") {
            if (call->arguments().empty() || call->arguments().size() > 2) {
                report(*call, builtinName + " expects a vec argument" + std::string(builtinName == "vec_reserve" ? " and a capacity" : ""));
                for (const auto& argument : call->arguments()) {
                    inferExpr(*argument);
                }
                return Type::voidType();
            }
            Type target = inferExpr(*call->arguments()[0]);
            if (target.isUnknown()) {
                report(*call->arguments()[0], builtinName + " expects a vec with a known element type");
            } else if (!target.isVec()) {
                report(*call->arguments()[0], builtinName + " expects a vec, got '" + target.name() + "'");
            }
            if (call->arguments().size() == 2) {
                Type capacityType = inferExpr(*call->arguments()[1]);
                if (!capacityType.isInteger()) {
                    report(*call->arguments()[1], builtinName + " expects an integer capacity, got '" + capacityType.name() + "'");
                }
            }
            return Type::voidType();
        }

        if (builtinName == "to_string_i64" || builtinName == "to_string_char") {
            if (call->arguments().size() != 1) {
                report(*call, builtinName + " expects exactly one argument");
                return Type(TypeKind::String);
            }
            Type argumentType = inferExpr(*call->arguments()[0]);
            if (!argumentType.isInteger()) {
                report(*call->arguments()[0], builtinName + " expects an integer, got '" + argumentType.name() + "'");
            }
            return Type(TypeKind::String);
        }

        if (builtinName == "to_string_f64" || builtinName == "sqrt_f64" || builtinName == "floor_f64"
            || builtinName == "ceil_f64" || builtinName == "round_f64") {
            if (call->arguments().size() != 1) {
                report(*call, builtinName + " expects exactly one argument");
                return Type(TypeKind::F64);
            }
            Type argumentType = inferExpr(*call->arguments()[0]);
            if (!argumentType.isNumeric()) {
                report(*call->arguments()[0], builtinName + " expects a number, got '" + argumentType.name() + "'");
            }
            return Type(TypeKind::F64);
        }

        if (builtinName == "f64_to_i64") {
            if (call->arguments().size() != 1) {
                report(*call, "f64_to_i64 expects exactly one argument");
                return Type(TypeKind::I64);
            }
            Type argumentType = inferExpr(*call->arguments()[0]);
            if (!argumentType.isNumeric()) {
                report(*call->arguments()[0], "f64_to_i64 expects a number, got '" + argumentType.name() + "'");
            }
            return Type(TypeKind::I64);
        }

        if (builtinName == "to_string_bool") {
            if (call->arguments().size() != 1) {
                report(*call, "to_string_bool expects exactly one argument");
                return Type(TypeKind::String);
            }
            Type argumentType = inferExpr(*call->arguments()[0]);
            if (!canImplicitlyConvert(argumentType, Type(TypeKind::Bool))) {
                report(*call->arguments()[0], "to_string_bool expects a bool, got '" + argumentType.name() + "'");
            }
            return Type(TypeKind::String);
        }

        if (builtinName == "str_to_i64") {
            if (call->arguments().size() != 1) {
                report(*call, "str_to_i64 expects exactly one argument");
                return Type(TypeKind::I64);
            }
            Type argumentType = inferExpr(*call->arguments()[0]);
            if (argumentType.kind() != TypeKind::String) {
                report(*call->arguments()[0], "str_to_i64 expects a string, got '" + argumentType.name() + "'");
            }
            return Type(TypeKind::I64);
        }

        if (builtinName == "str_to_f64") {
            if (call->arguments().size() != 1) {
                report(*call, "str_to_f64 expects exactly one argument");
                return Type(TypeKind::F64);
            }
            Type argumentType = inferExpr(*call->arguments()[0]);
            if (argumentType.kind() != TypeKind::String) {
                report(*call->arguments()[0], "str_to_f64 expects a string, got '" + argumentType.name() + "'");
            }
            return Type(TypeKind::F64);
        }

        if (builtinName == "str_index_of") {
            if (call->arguments().size() != 2) {
                report(*call, "str_index_of expects exactly two arguments");
                return Type(TypeKind::I64);
            }
            for (const auto& argument : call->arguments()) {
                Type argumentType = inferExpr(*argument);
                if (argumentType.kind() != TypeKind::String) {
                    report(*argument, "str_index_of expects string arguments, got '" + argumentType.name() + "'");
                }
            }
            return Type(TypeKind::I64);
        }

        if (builtinName == "str_contains") {
            if (call->arguments().size() != 2) {
                report(*call, "str_contains expects exactly two arguments");
                return Type(TypeKind::Bool);
            }
            for (const auto& argument : call->arguments()) {
                Type argumentType = inferExpr(*argument);
                if (argumentType.kind() != TypeKind::String) {
                    report(*argument, "str_contains expects string arguments, got '" + argumentType.name() + "'");
                }
            }
            return Type(TypeKind::Bool);
        }

        if (builtinName == "append_str") {
            if (call->arguments().size() != 2) {
                report(*call, "append_str expects exactly two arguments");
                return Type(TypeKind::String);
            }
            for (const auto& argument : call->arguments()) {
                Type argumentType = inferExpr(*argument);
                if (argumentType.kind() != TypeKind::String) {
                    report(*argument, "append_str expects string arguments, got '" + argumentType.name() + "'");
                }
            }
            return Type(TypeKind::String);
        }

        if (builtinName == "exit") {
            if (call->arguments().size() != 1) {
                report(*call, "exit expects exactly one argument");
                return Type::voidType();
            }
            Type argumentType = inferExpr(*call->arguments()[0]);
            if (!argumentType.isInteger()) {
                report(*call->arguments()[0], "exit expects an integer code, got '" + argumentType.name() + "'");
            }
            return Type::voidType();
        }

        if (builtinName == "eprint") {
            if (call->arguments().size() != 1) {
                report(*call, "eprint expects exactly one argument");
                return Type::voidType();
            }
            Type argumentType = inferExpr(*call->arguments()[0]);
            if (argumentType.kind() != TypeKind::String) {
                report(*call->arguments()[0], "eprint expects a string, got '" + argumentType.name() + "'");
            }
            return Type::voidType();
        }

        if (builtinName == "file_exists") {
            if (call->arguments().size() != 1) {
                report(*call, "file_exists expects exactly one argument");
                return Type(TypeKind::Bool);
            }
            Type argumentType = inferExpr(*call->arguments()[0]);
            if (argumentType.kind() != TypeKind::String) {
                report(*call->arguments()[0], "file_exists expects a string path, got '" + argumentType.name() + "'");
            }
            return Type(TypeKind::Bool);
        }

        if (builtinName == "clock_ms" || builtinName == "mono_ms") {
            if (!call->arguments().empty()) {
                report(*call, builtinName + " expects no arguments");
            }
            return Type(TypeKind::I64);
        }

        const auto function = functions_.find(call->callee());
        if (function == functions_.end()) {
            report(*call, "unknown function '" + call->callee() + "'");
            for (const auto& argument : call->arguments()) {
                inferExpr(*argument);
            }
            return Type::unknown();
        }

        const FunctionSignature& signature = function->second;
        if (call->arguments().size() != signature.parameters.size()) {
            report(*call, "function '" + call->callee() + "' expects " + std::to_string(signature.parameters.size())
                   + " argument(s), got " + std::to_string(call->arguments().size()));
        }

        for (std::size_t i = 0; i < call->arguments().size(); ++i) {
            Type argumentType = inferExpr(*call->arguments()[i]);
            if (i < signature.parameters.size()) {
                Type parameterType = signature.parameters[i];
                if (!canImplicitlyConvert(argumentType, parameterType)) {
                    report(*call->arguments()[i], "argument " + std::to_string(i + 1) + " of function '" + call->callee()
                           + "' expects '" + parameterType.name() + "', got '" + argumentType.name() + "'");
                }
            }
        }
        return signature.returnType;
    }

    report(expression, "unknown expression node");
    return Type::unknown();
}

void Sema::pushScope() {
    scopes_.emplace_back();
}

void Sema::popScope() {
    scopes_.pop_back();
}

bool Sema::declareSymbol(const std::string& name, Type type, bool isConst) {
    auto& scope = scopes_.back();
    if (scope.find(name) != scope.end()) {
        report("symbol '" + name + "' is already declared in this scope");
        return false;
    }

    scope.emplace(name, SymbolInfo{name, type, isConst});
    return true;
}

const SymbolInfo* Sema::lookupSymbol(const std::string& name) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto it = scope->find(name);
        if (it != scope->end()) {
            return &it->second;
        }
    }

    return nullptr;
}

const StructInfo* Sema::lookupStruct(const std::string& name) const {
    const auto found = structs_.find(name);
    if (found == structs_.end()) {
        return nullptr;
    }
    return &found->second;
}

const EnumInfo* Sema::lookupEnum(const std::string& name) const {
    const auto found = enums_.find(name);
    if (found == enums_.end()) {
        return nullptr;
    }
    return &found->second;
}

Type Sema::resolveType(Type type) const {
    if (type.kind() == TypeKind::Struct && structs_.find(type.structName()) == structs_.end()) {
        if (enums_.find(type.structName()) != enums_.end()) {
            return Type::enumOf(type.structName());
        }
    }
    if (type.isArray() || type.isVec()) {
        return type.isVec() ? Type::vecOf(resolveType(type.elementType()))
                            : Type::arrayOf(resolveType(type.elementType()), type.arrayLength());
    }
    return type;
}

void Sema::report(const std::string& message) {
    errors_.push_back(message);
}

void Sema::report(const AstNode& node, const std::string& message) {
    const SourceLocation& location = node.location();
    if (!location.file.empty()) {
        errors_.push_back(location.file + ":" + std::to_string(location.line) + ":" + std::to_string(location.column) + ": " + message);
        return;
    }

    report(message);
}

void Sema::record(const std::string& message) {
    typeLog_.push_back(message);
}

} // namespace miko
