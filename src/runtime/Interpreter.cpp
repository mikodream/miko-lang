#include "miko/runtime/Interpreter.h"

#include "miko/Builtins.h"
#include "miko/StringLiterals.h"
#include "miko/NumericLiterals.h"

#include <filesystem>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace miko {

Interpreter::Interpreter(std::ostream& out, std::vector<std::string> programArgs)
    : out_(out), programArgs_(std::move(programArgs)), rngSeed_(0x9E3779B97F4A7C15LL) {
}

bool Interpreter::run(const Program& program) {
    errors_.clear();
    functions_.clear();
    scopes_.clear();
    enums_.clear();

    for (const auto& enumeration : program.enums()) {
        std::unordered_map<std::string, long long> members;
        for (std::size_t i = 0; i < enumeration->members().size(); ++i) {
            members.emplace(enumeration->members()[i], static_cast<long long>(i));
        }
        enums_.emplace(enumeration->name(), std::move(members));
    }

    registerFunctions(program);
    if (!errors_.empty()) {
        return false;
    }

    std::string entryName;
    for (const auto& function : functions_) {
        if (isEntryFunctionName(function.first)) {
            entryName = function.first;
            break;
        }
    }

    if (entryName.empty()) {
        report("entry function 'main' was not found");
        return false;
    }

    try {
        callFunction(entryName, {});
    } catch (const std::runtime_error& error) {
        report(error.what());
    }

    return errors_.empty();
}

const std::vector<std::string>& Interpreter::errors() const {
    return errors_;
}

void Interpreter::registerFunctions(const Program& program) {
    for (const auto& function : program.functions()) {
        if (functions_.find(function->name()) != functions_.end()) {
            report("function '" + function->name() + "' is already declared");
            continue;
        }
        functions_.emplace(function->name(), function.get());
    }
}

Value Interpreter::callFunction(const std::string& name, const std::vector<Value>& arguments) {
    const std::string builtinName = canonicalBuiltinName(name);

    if (builtinName == "print") {
        if (arguments.size() != 1) {
            throw std::runtime_error("print expects exactly one argument");
        }
        out_ << arguments.front().toString() << '\n';
        return Value::voidValue();
    }

    if (builtinName == "len") {
        if (arguments.size() != 1) {
            throw std::runtime_error("len expects exactly one argument");
        }
        if (arguments.front().kind() == ValueKind::String) {
            return Value::integer(static_cast<long long>(arguments.front().asString().size()));
        }
        if (arguments.front().kind() == ValueKind::Array) {
            return Value::integer(static_cast<long long>(arguments.front().asArray().size()));
        }
        if (arguments.front().kind() == ValueKind::Vec) {
            return Value::integer(static_cast<long long>(arguments.front().asVec().size()));
        }
        throw std::runtime_error("len expects a string, array, or vec");
    }

    if (builtinName == "input") {
        if (!arguments.empty()) {
            throw std::runtime_error("input expects no arguments");
        }
        std::string line;
        std::getline(std::cin, line);
        return Value::string(std::move(line));
    }

    if (builtinName == "arg_count") {
        if (!arguments.empty()) {
            throw std::runtime_error("arg_count expects no arguments");
        }
        return Value::integer(static_cast<long long>(programArgs_.size()));
    }

    if (builtinName == "arg_at") {
        if (arguments.size() != 1) {
            throw std::runtime_error("arg_at expects exactly one argument");
        }
        const long long index = arguments[0].asInteger();
        if (index < 0 || static_cast<std::size_t>(index) >= programArgs_.size()) {
            throw std::runtime_error("arg_at index out of range");
        }
        return Value::string(programArgs_[static_cast<std::size_t>(index)]);
    }

    if (builtinName == "char_at") {
        if (arguments.size() != 2) {
            throw std::runtime_error("char_at expects exactly two arguments");
        }
        const std::string& text = arguments[0].asString();
        const long long index = arguments[1].asInteger();
        if (index < 0 || static_cast<std::size_t>(index) >= text.size()) {
            throw std::runtime_error("char_at index out of range");
        }
        // A byte in 0..255, matching the compiled runtime. Storing the raw
        // `char` would sign-extend on targets where char is signed, turning
        // UTF-8 continuation bytes negative and breaking byte arithmetic.
        return Value::character(static_cast<unsigned char>(text[static_cast<std::size_t>(index)]));
    }

    if (builtinName == "substring") {
        if (arguments.size() != 3) {
            throw std::runtime_error("substring expects exactly three arguments");
        }
        const std::string& text = arguments[0].asString();
        const long long start = arguments[1].asInteger();
        const long long length = arguments[2].asInteger();
        if (start < 0 || length < 0) {
            throw std::runtime_error("substring start/length must be non-negative");
        }
        const std::size_t begin = static_cast<std::size_t>(start);
        const std::size_t count = static_cast<std::size_t>(length);
        if (begin > text.size()) {
            throw std::runtime_error("substring start out of range");
        }
        return Value::string(text.substr(begin, count));
    }

    if (builtinName == "read_file") {
        if (arguments.size() != 1) {
            throw std::runtime_error("read_file expects exactly one argument");
        }
        std::ifstream file(arguments[0].asString(), std::ios::binary);
        if (!file) {
            throw std::runtime_error("failed to open file '" + arguments[0].asString() + "'");
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return Value::string(buffer.str());
    }

    if (builtinName == "run_command") {
        // Runs a shell command and yields its exit status. This is what lets a
        // miko program drive a toolchain - the self-hosted compiler uses it to
        // assemble and link the IR it emits, so `miko prog.miko out\p` produces
        // an executable rather than stopping at intermediate files.
        //
        // stdio is inherited, so the child's diagnostics reach the terminal
        // instead of being swallowed by the parent.
        if (arguments.size() != 1) {
            throw std::runtime_error("run_command expects exactly one argument");
        }
        const std::string command = arguments[0].asString();
        const int status = std::system(command.c_str());
        return Value::integer(static_cast<long long>(status));
    }

    if (builtinName == "write_file") {
        if (arguments.size() != 2) {
            throw std::runtime_error("write_file expects exactly two arguments");
        }
        const std::filesystem::path outputPath(arguments[0].asString());
        if (outputPath.has_parent_path()) {
            std::filesystem::create_directories(outputPath.parent_path());
        }
        std::ofstream file(outputPath, std::ios::binary);
        if (!file) {
            throw std::runtime_error("failed to open file '" + arguments[0].asString() + "' for writing");
        }
        file << arguments[1].asString();
        if (!file) {
            throw std::runtime_error("failed to write file '" + arguments[0].asString() + "'");
        }
        return Value::voidValue();
    }

    if (builtinName == "seed_i64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("seed_i64 expects exactly one argument");
        }
        rngSeed_ = arguments[0].asInteger();
        return Value::voidValue();
    }

    if (builtinName == "rand_i64") {
        // SplitMix64: high-quality, dependency-free PRNG. The seed advances
        // on every call, so repeated calls within a single run produce a
        // different value (no fixed point).
        //
        // The state word is unsigned on purpose: the mixing shifts have to be
        // logical, matching the LLVM `lshr` that NativeCompiler emits and the
        // runtime's miko_rand_i64. Shifting a signed long long would be an
        // arithmetic shift for negative states, which is why this engine used
        // to disagree with the two compiled backends.
        unsigned long long state = static_cast<unsigned long long>(rngSeed_);
        state += 0x9E3779B97F4A7C15ULL;
        rngSeed_ = static_cast<long long>(state);
        unsigned long long z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB13511EF9ULL;
        z = z ^ (z >> 31);
        return Value::integer(static_cast<long long>(z));
    }

    if (builtinName == "vec_new") {
        if (!arguments.empty()) {
            throw std::runtime_error("vec_new expects no arguments");
        }
        return Value::vec();
    }

    if (builtinName == "vec_push") {
        if (arguments.size() != 2) {
            throw std::runtime_error("vec_push expects exactly two arguments");
        }
        // Copy the Value handle (shares storage); mutate through the copy.
        Value target = arguments[0];
        target.asVec().push_back(arguments[1]);
        return Value::voidValue();
    }

    if (builtinName == "vec_pop") {
        if (arguments.size() != 1) {
            throw std::runtime_error("vec_pop expects exactly one argument");
        }
        Value target = arguments[0];
        auto& elements = target.asVec();
        if (elements.empty()) {
            throw std::runtime_error("vec_pop from empty vec");
        }
        Value last = elements.back();
        elements.pop_back();
        return last;
    }

    if (builtinName == "vec_set") {
        if (arguments.size() != 3) {
            throw std::runtime_error("vec_set expects exactly three arguments");
        }
        Value target = arguments[0];
        auto& elements = target.asVec();
        const long long index = arguments[1].asInteger();
        if (index < 0 || static_cast<std::size_t>(index) >= elements.size()) {
            throw std::runtime_error("vec index out of bounds");
        }
        elements[static_cast<std::size_t>(index)] = arguments[2];
        return Value::voidValue();
    }

    if (builtinName == "vec_clear") {
        if (arguments.size() != 1) {
            throw std::runtime_error("vec_clear expects exactly one argument");
        }
        Value target = arguments[0];
        target.asVec().clear();
        return Value::voidValue();
    }

    if (builtinName == "vec_reserve") {
        if (arguments.size() != 2) {
            throw std::runtime_error("vec_reserve expects exactly two arguments");
        }
        const long long capacity = arguments[1].asInteger();
        if (capacity < 0) {
            throw std::runtime_error("vec_reserve capacity must be non-negative");
        }
        Value target = arguments[0];
        target.asVec().reserve(static_cast<std::size_t>(capacity));
        return Value::voidValue();
    }

    if (builtinName == "to_string_i64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("to_string_i64 expects exactly one argument");
        }
        return Value::string(std::to_string(arguments[0].asInteger()));
    }

    if (builtinName == "to_string_f64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("to_string_f64 expects exactly one argument");
        }
        std::ostringstream out;
        out << std::setprecision(12) << arguments[0].asFloat();
        return Value::string(out.str());
    }

    if (builtinName == "to_string_bool") {
        if (arguments.size() != 1) {
            throw std::runtime_error("to_string_bool expects exactly one argument");
        }
        return Value::string(arguments[0].asBool() ? "true" : "false");
    }

    if (builtinName == "to_string_char") {
        if (arguments.size() != 1) {
            throw std::runtime_error("to_string_char expects exactly one argument");
        }
        const long long code = arguments[0].asInteger();
        if (code < 0 || code > 255) {
            throw std::runtime_error("to_string_char code out of range");
        }
        return Value::string(std::string(1, static_cast<char>(code)));
    }

    if (builtinName == "str_to_i64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("str_to_i64 expects exactly one argument");
        }
        const std::string& text = arguments[0].asString();
        try {
            std::size_t consumed = 0;
            const long long value = std::stoll(text, &consumed);
            if (consumed != text.size()) {
                throw std::invalid_argument("trailing characters");
            }
            return Value::integer(value);
        } catch (const std::exception&) {
            throw std::runtime_error("invalid integer '" + text + "'");
        }
    }

    if (builtinName == "str_to_f64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("str_to_f64 expects exactly one argument");
        }
        const std::string& text = arguments[0].asString();
        try {
            std::size_t consumed = 0;
            const double value = std::stod(text, &consumed);
            if (consumed != text.size()) {
                throw std::invalid_argument("trailing characters");
            }
            return Value::floating(value);
        } catch (const std::exception&) {
            throw std::runtime_error("invalid number '" + text + "'");
        }
    }

    if (builtinName == "str_index_of") {
        if (arguments.size() != 2) {
            throw std::runtime_error("str_index_of expects exactly two arguments");
        }
        const std::string& text = arguments[0].asString();
        const std::string& needle = arguments[1].asString();
        const std::size_t found = text.find(needle);
        if (found == std::string::npos) {
            return Value::integer(-1);
        }
        return Value::integer(static_cast<long long>(found));
    }

    if (builtinName == "str_contains") {
        if (arguments.size() != 2) {
            throw std::runtime_error("str_contains expects exactly two arguments");
        }
        return Value::boolean(arguments[0].asString().find(arguments[1].asString()) != std::string::npos);
    }

    if (builtinName == "append_str") {
        if (arguments.size() != 2) {
            throw std::runtime_error("append_str expects exactly two arguments");
        }
        // Same observable result as `a + b`. The compiled backends may reuse the
        // left buffer in place, which the interpreter does not need to imitate:
        // that is an allocation strategy, not part of the meaning.
        return Value::string(arguments[0].asString() + arguments[1].asString());
    }

    if (builtinName == "sqrt_f64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("sqrt_f64 expects exactly one argument");
        }
        return Value::floating(std::sqrt(arguments[0].asFloat()));
    }

    if (builtinName == "floor_f64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("floor_f64 expects exactly one argument");
        }
        return Value::floating(std::floor(arguments[0].asFloat()));
    }

    if (builtinName == "ceil_f64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("ceil_f64 expects exactly one argument");
        }
        return Value::floating(std::ceil(arguments[0].asFloat()));
    }

    if (builtinName == "round_f64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("round_f64 expects exactly one argument");
        }
        return Value::floating(std::round(arguments[0].asFloat()));
    }

    if (builtinName == "f64_to_i64") {
        if (arguments.size() != 1) {
            throw std::runtime_error("f64_to_i64 expects exactly one argument");
        }
        return Value::integer(static_cast<long long>(arguments[0].asFloat()));
    }

    if (builtinName == "exit") {
        if (arguments.size() != 1) {
            throw std::runtime_error("exit expects exactly one argument");
        }
        out_.flush();
        std::exit(static_cast<int>(arguments[0].asInteger()));
    }

    if (builtinName == "eprint") {
        if (arguments.size() != 1) {
            throw std::runtime_error("eprint expects exactly one argument");
        }
        std::cerr << arguments[0].toString() << '\n';
        return Value::voidValue();
    }

    if (builtinName == "file_exists") {
        if (arguments.size() != 1) {
            throw std::runtime_error("file_exists expects exactly one argument");
        }
        std::error_code error;
        const bool exists = std::filesystem::exists(arguments[0].asString(), error);
        return Value::boolean(exists);
    }

    if (builtinName == "clock_ms") {
        // Milliseconds since the Unix epoch (wall clock, the analog of
        // System.currentTimeMillis()).
        if (!arguments.empty()) {
            throw std::runtime_error("clock_ms expects no arguments");
        }
        const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
        return Value::integer(static_cast<long long>(now.time_since_epoch().count()));
    }

    if (builtinName == "mono_ms") {
        // Monotonic clock in milliseconds; immune to system clock
        // adjustments, so it is the right primitive for measuring elapsed
        // time (e.g. how long a self-compile took).
        if (!arguments.empty()) {
            throw std::runtime_error("mono_ms expects no arguments");
        }
        const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now());
        return Value::integer(static_cast<long long>(now.time_since_epoch().count()));
    }

    const auto it = functions_.find(name);
    if (it == functions_.end()) {
        throw std::runtime_error("unknown function '" + name + "'");
    }

    const FunctionDecl& function = *it->second;
    if (arguments.size() != function.parameters().size()) {
        throw std::runtime_error("function '" + name + "' called with wrong number of arguments");
    }

    pushScope();
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        declareVariable(function.parameters()[i].name(), arguments[i]);
    }

    try {
        executeBlock(function.body(), false);
    } catch (const ReturnSignal& signal) {
        popScope();
        return signal.value;
    }

    popScope();
    return Value::voidValue();
}

void Interpreter::executeBlock(const BlockStmt& block, bool createScope) {
    if (createScope) {
        pushScope();
    }

    try {
        for (const auto& statement : block.statements()) {
            executeStmt(*statement);
        }
    } catch (...) {
        if (createScope) {
            popScope();
        }
        throw;
    }

    if (createScope) {
        popScope();
    }
}

void Interpreter::executeStmt(const Stmt& statement) {
    if (const auto* let = dynamic_cast<const LetStmt*>(&statement)) {
        if (let->initializer()) {
            Value value = evalExpr(*let->initializer());
            declareVariable(let->name(), value, let->isConst());
        } else if (let->explicitType().rfind("vec<", 0) == 0) {
            declareVariable(let->name(), Value::vec(), let->isConst());
        } else {
            declareVariable(let->name(), Value::voidValue(), let->isConst());
        }
        return;
    }

    if (const auto* assign = dynamic_cast<const AssignStmt*>(&statement)) {
        if (!assignVariable(assign->name(), evalExpr(assign->value()))) {
            throw std::runtime_error("undefined variable '" + assign->name() + "'");
        }
        return;
    }

    if (const auto* assign = dynamic_cast<const IndexAssignStmt*>(&statement)) {
        if (!assignArrayElement(assign->name(), evalExpr(assign->index()).asInteger(), evalExpr(assign->value()))) {
            throw std::runtime_error("undefined variable '" + assign->name() + "'");
        }
        return;
    }

    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&statement)) {
        (void)evalExpr(exprStmt->expression());
        return;
    }

    if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&statement)) {
        throw ReturnSignal{returnStmt->value() ? evalExpr(*returnStmt->value()) : Value::voidValue()};
    }

    if (const auto* block = dynamic_cast<const BlockStmt*>(&statement)) {
        executeBlock(*block);
        return;
    }

    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&statement)) {
        if (evalExpr(ifStmt->condition()).asBool()) {
            executeBlock(ifStmt->thenBlock());
        } else if (const BlockStmt* elseBlock = ifStmt->elseBlock()) {
            executeBlock(*elseBlock);
        }
        return;
    }

    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&statement)) {
        while (evalExpr(whileStmt->condition()).asBool()) {
            try {
                executeBlock(whileStmt->body());
            } catch (const ContinueSignal&) {
                continue;
            } catch (const BreakSignal&) {
                break;
            }
        }
        return;
    }

    if (const auto* forStmt = dynamic_cast<const ForStmt*>(&statement)) {
        pushScope();
        try {
            if (const Stmt* initializer = forStmt->initializer()) {
                executeStmt(*initializer);
            }

            while (!forStmt->condition() || evalExpr(*forStmt->condition()).asBool()) {
                try {
                    executeBlock(forStmt->body());
                } catch (const ContinueSignal&) {
                } catch (const BreakSignal&) {
                    break;
                }

                if (const Stmt* increment = forStmt->increment()) {
                    executeStmt(*increment);
                }
            }
        } catch (...) {
            popScope();
            throw;
        }
        popScope();
        return;
    }

    if (dynamic_cast<const BreakStmt*>(&statement)) {
        throw BreakSignal{};
    }

    if (dynamic_cast<const ContinueStmt*>(&statement)) {
        throw ContinueSignal{};
    }

    throw std::runtime_error("unknown statement node");
}

Value Interpreter::evalExpr(const Expr& expression) {
    if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expression)) {
        return Value::integer(parseIntegerLiteral(integer->value()));
    }

    if (const auto* floating = dynamic_cast<const FloatExpr*>(&expression)) {
        return Value::floating(parseFloatLiteral(floating->value()));
    }

    if (const auto* string = dynamic_cast<const StringExpr*>(&expression)) {
        return Value::string(unquoteString(string->value()));
    }

    if (const auto* character = dynamic_cast<const CharExpr*>(&expression)) {
        return Value::character(unquoteChar(character->value()));
    }

    if (const auto* boolean = dynamic_cast<const BoolExpr*>(&expression)) {
        return Value::boolean(boolean->value());
    }

    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        Value operand = evalExpr(unary->operand());
        if (unary->op() == "!") {
            return Value::boolean(!operand.asBool());
        }
        if (unary->op() == "-") {
            if (operand.isFloat()) {
                return Value::floating(-operand.asFloat());
            }
            return Value::integer(-operand.asInteger());
        }
        throw std::runtime_error("unsupported unary operator '" + unary->op() + "'");
    }

    if (const auto* array = dynamic_cast<const ArrayExpr*>(&expression)) {
        std::vector<Value> elements;
        elements.reserve(array->elements().size());
        for (const auto& element : array->elements()) {
            elements.push_back(evalExpr(*element));
        }
        return Value::array(std::move(elements));
    }

    if (const auto* index = dynamic_cast<const IndexExpr*>(&expression)) {
        const Value target = evalExpr(index->target());
        const long long elementIndex = evalExpr(index->index()).asInteger();
        if (target.kind() == ValueKind::Vec) {
            const auto& elements = target.asVec();
            if (elementIndex < 0 || static_cast<std::size_t>(elementIndex) >= elements.size()) {
                throw std::runtime_error("vec index out of bounds");
            }
            return elements[static_cast<std::size_t>(elementIndex)];
        }
        const auto& elements = target.asArray();
        if (elementIndex < 0 || static_cast<std::size_t>(elementIndex) >= elements.size()) {
            throw std::runtime_error("array index out of bounds");
        }
        return elements[static_cast<std::size_t>(elementIndex)];
    }

    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expression)) {
        Value target = evalExpr(field->target());
        const auto& fields = target.asStructFields();
        const auto found = fields.find(field->field());
        if (found == fields.end()) {
            throw std::runtime_error("struct '" + target.structTypeName() + "' has no field '" + field->field() + "'");
        }
        return found->second;
    }

    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expression)) {
        std::unordered_map<std::string, Value> fields;
        for (const auto& field : literal->fields()) {
            fields[field.first] = evalExpr(*field.second);
        }
        return Value::structure(literal->typeName(), std::move(fields));
    }

    if (const auto* variable = dynamic_cast<const VariableExpr*>(&expression)) {
        const std::string& name = variable->name();
        const std::size_t separator = name.find("::");
        if (separator != std::string::npos) {
            const std::string enumName = name.substr(0, separator);
            const std::string memberName = name.substr(separator + 2);
            const auto foundEnum = enums_.find(enumName);
            if (foundEnum == enums_.end()) {
                throw std::runtime_error("unknown enum '" + enumName + "'");
            }
            const auto foundMember = foundEnum->second.find(memberName);
            if (foundMember == foundEnum->second.end()) {
                throw std::runtime_error("enum '" + enumName + "' has no member '" + memberName + "'");
            }
            return Value::integer(foundMember->second);
        }
        return lookupVariable(name);
    }

    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expression)) {
        if (binary->op() == "&&") {
            Value left = evalExpr(binary->left());
            return Value::boolean(left.asBool() && evalExpr(binary->right()).asBool());
        }
        if (binary->op() == "||") {
            Value left = evalExpr(binary->left());
            return Value::boolean(left.asBool() || evalExpr(binary->right()).asBool());
        }
        return applyBinary(binary->op(), evalExpr(binary->left()), evalExpr(binary->right()));
    }

    if (const auto* call = dynamic_cast<const CallExpr*>(&expression)) {
        std::vector<Value> arguments;
        arguments.reserve(call->arguments().size());
        for (const auto& argument : call->arguments()) {
            arguments.push_back(evalExpr(*argument));
        }
        return callFunction(call->callee(), arguments);
    }

    throw std::runtime_error("unknown expression node");
}

void Interpreter::pushScope() {
    scopes_.emplace_back();
}

void Interpreter::popScope() {
    scopes_.pop_back();
}

void Interpreter::declareVariable(const std::string& name, Value value, bool isConst) {
    scopes_.back()[name] = VariableInfo{std::move(value), isConst};
}

bool Interpreter::assignVariable(const std::string& name, Value value) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto it = scope->find(name);
        if (it != scope->end()) {
            if (it->second.isConst) {
                throw std::runtime_error("cannot assign to const variable '" + name + "'");
            }
            it->second.value = std::move(value);
            return true;
        }
    }
    return false;
}

bool Interpreter::assignArrayElement(const std::string& name, long long index, Value value) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto it = scope->find(name);
        if (it != scope->end()) {
            if (it->second.isConst) {
                throw std::runtime_error("cannot assign to const array variable '" + name + "'");
            }
            if (it->second.value.kind() == ValueKind::Vec) {
                auto& elements = it->second.value.asVec();
                if (index < 0 || static_cast<std::size_t>(index) >= elements.size()) {
                    throw std::runtime_error("vec index out of bounds");
                }
                elements[static_cast<std::size_t>(index)] = std::move(value);
                return true;
            }
            auto& elements = it->second.value.asArray();
            if (index < 0 || static_cast<std::size_t>(index) >= elements.size()) {
                throw std::runtime_error("array index out of bounds");
            }
            elements[static_cast<std::size_t>(index)] = std::move(value);
            return true;
        }
    }
    return false;
}

Value Interpreter::lookupVariable(const std::string& name) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto it = scope->find(name);
        if (it != scope->end()) {
            return it->second.value;
        }
    }
    throw std::runtime_error("undefined variable '" + name + "'");
}

void Interpreter::report(const std::string& message) {
    errors_.push_back(message);
}

Value Interpreter::applyBinary(const std::string& op, const Value& left, const Value& right) {
    if (op == "+" && (left.kind() == ValueKind::String || right.kind() == ValueKind::String)) {
        return Value::string(left.toString() + right.toString());
    }

    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        if (left.isFloat() || right.isFloat()) {
            const double l = left.asFloat();
            const double r = right.asFloat();
            if (op == "+") return Value::floating(l + r);
            if (op == "-") return Value::floating(l - r);
            if (op == "*") return Value::floating(l * r);
            if (op == "/") return Value::floating(l / r);
            if (op == "%") return Value::floating(std::fmod(l, r));
        }

        const long long l = left.asInteger();
        const long long r = right.asInteger();
        if (op == "+") return Value::integer(l + r);
        if (op == "-") return Value::integer(l - r);
        if (op == "*") return Value::integer(l * r);
        if (op == "/" || op == "%") {
            // Dividing by zero is undefined behaviour in C++, so the process
            // died with a hardware exception (0xC0000094) and the user saw a
            // crash instead of an error message naming the operator. main()
            // turns the exception into "error: <message>" on stderr and exit 1.
            if (r == 0) {
                throw std::runtime_error(op == "/" ? "division by zero" : "remainder by zero");
            }
            // The single pair that overflows a 64-bit division.
            if (l == std::numeric_limits<long long>::min() && r == -1) {
                throw std::runtime_error("integer overflow in " + op);
            }
            if (op == "/") return Value::integer(l / r);
            return Value::integer(l % r);
        }
    }

    if (left.kind() == ValueKind::String && right.kind() == ValueKind::String) {
        const int compared = left.asString().compare(right.asString());
        if (op == "==") return Value::boolean(compared == 0);
        if (op == "!=") return Value::boolean(compared != 0);
        if (op == "<") return Value::boolean(compared < 0);
        if (op == "<=") return Value::boolean(compared <= 0);
        if (op == ">") return Value::boolean(compared > 0);
        if (op == ">=") return Value::boolean(compared >= 0);
    }

    // Numeric comparisons compare by value (chars are their code points), so
    // `' ' == 32` holds the same way it does in the native backends.
    if (left.isNumeric() && right.isNumeric()) {
        if (op == "==") return Value::boolean(left.asInteger() == right.asInteger());
        if (op == "!=") return Value::boolean(left.asInteger() != right.asInteger());
        if (op == "<") return Value::boolean(left.asFloat() < right.asFloat());
        if (op == "<=") return Value::boolean(left.asFloat() <= right.asFloat());
        if (op == ">") return Value::boolean(left.asFloat() > right.asFloat());
        if (op == ">=") return Value::boolean(left.asFloat() >= right.asFloat());
    }

    if (op == "==") return Value::boolean(left.toString() == right.toString());
    if (op == "!=") return Value::boolean(left.toString() != right.toString());
    if (op == "<") return Value::boolean(left.asFloat() < right.asFloat());
    if (op == "<=") return Value::boolean(left.asFloat() <= right.asFloat());
    if (op == ">") return Value::boolean(left.asFloat() > right.asFloat());
    if (op == ">=") return Value::boolean(left.asFloat() >= right.asFloat());

    throw std::runtime_error("unsupported binary operator '" + op + "'");
}

std::string Interpreter::unquoteString(const std::string& value) {
    return decodeStringLiteral(value);
}

char Interpreter::unquoteChar(const std::string& value) {
    return decodeCharLiteral(value);
}

} // namespace miko
