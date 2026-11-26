#include "miko/lexer/Lexer.h"
#include "miko/lexer/Token.h"
#include "miko/parser/Parser.h"
#include "miko/runtime/Interpreter.h"
#include "miko/sema/Sema.h"

#if MIKO_ENABLE_LLVM
#include "miko/codegen/NativeCompiler.h"
#endif

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char* kUsage = R"(miko - lightweight static programming language

Usage:
  miko --version
  miko <input.miko> [options]

Options:
  --compile           Compile to native executable (requires LLVM)
  -o <path>           Output file path (default: <input>.exe on Windows, <input> on Unix)
  --target <triple>   Cross-compile target triple (e.g. x86_64-linux-gnu, aarch64-apple-darwin)
  --opt <0|1|2|3>     Optimization level (default: 0)
  --emit-ir           Emit LLVM IR (.ll) instead of executable
  --emit-obj          Emit object file (.o/.obj) instead of executable
  --emit-asm          Emit assembly (.s) instead of executable
  --keep-ir           Keep intermediate .ll file after linking
  --clang <path>      Path to clang for linking (default: clang)
  --debug-tokens      Print lexer tokens
  --debug-ast         Print parsed AST
  --debug-types       Print semantic type inference
  --run               Run the program with the tree-walk interpreter
  -- <args...>        Pass remaining arguments to the running Miko program
  --help              Print this help
  --version           Print compiler version

Examples:
  miko examples/hello.miko --debug-tokens
  miko examples/hello.miko --compile -o hello.exe
  miko examples/hello.miko --compile --target aarch64-linux-gnu -o hello_arm
  miko examples/hello.miko --compile --emit-ir --keep-ir -o hello.ll
  miko examples/hello.miko --compile --opt 2 -o hello.exe
)";

struct Options {
    bool showHelp = false;
    bool showVersion = false;
    bool debugTokens = false;
    bool debugAst = false;
    bool debugTypes = false;
    bool runProgram = false;
    bool compile = false;
    bool emitIr = false;
    bool emitObj = false;
    bool emitAsm = false;
    bool keepIr = false;
    int optLevel = 0;
    std::string outputFile;
    std::string targetTriple;
    std::string clangPath;
    std::string inputFile;
    std::vector<std::string> programArgs;
};

struct LoadedProgram {
    std::unique_ptr<miko::Program> program;
    std::unordered_map<std::string, std::string> sources;
};

struct ModuleInfo {
    std::string path;
    std::string moduleName;
    std::unordered_map<std::string, std::string> exports;
    std::unordered_map<std::string, std::string> structExports;
    std::unordered_map<std::string, std::string> enumExports;
};

struct ImportNamespace {
    std::string moduleName;
    std::unordered_map<std::string, std::string> exports;
    std::unordered_map<std::string, std::string> structExports;
    std::unordered_map<std::string, std::string> enumExports;
};

struct ParsedDiagnostic {
    bool hasLocation = false;
    std::string file;
    std::size_t line = 0;
    std::size_t column = 0;
    std::string message;
};

Options parseArgs(const std::vector<std::string>& args) {
    Options options;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "--") {
            for (++i; i < args.size(); ++i) {
                options.programArgs.push_back(args[i]);
            }
            break;
        } else if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        } else if (arg == "--version") {
            options.showVersion = true;
        } else if (arg == "--debug-tokens") {
            options.debugTokens = true;
        } else if (arg == "--debug-ast") {
            options.debugAst = true;
        } else if (arg == "--debug-types") {
            options.debugTypes = true;
        } else if (arg == "--run") {
            options.runProgram = true;
        } else if (arg == "--compile") {
            options.compile = true;
        } else if (arg == "-o") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("-o requires a path argument");
            }
            options.outputFile = args[++i];
        } else if (arg == "--target") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("--target requires a triple argument");
            }
            options.targetTriple = args[++i];
        } else if (arg == "--opt") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("--opt requires a level (0-3)");
            }
            options.optLevel = std::stoi(args[++i]);
            if (options.optLevel < 0 || options.optLevel > 3) {
                throw std::runtime_error("--opt level must be 0, 1, 2, or 3");
            }
        } else if (arg == "--emit-ir") {
            options.emitIr = true;
        } else if (arg == "--emit-obj") {
            options.emitObj = true;
        } else if (arg == "--emit-asm") {
            options.emitAsm = true;
        } else if (arg == "--keep-ir") {
            options.keepIr = true;
        } else if (arg == "--clang") {
            if (i + 1 >= args.size()) {
                throw std::runtime_error("--clang requires a path argument");
            }
            options.clangPath = args[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::runtime_error("unknown option: " + arg);
        } else if (options.inputFile.empty()) {
            options.inputFile = arg;
        } else {
            throw std::runtime_error("unexpected argument: " + arg);
        }
    }

    return options;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open input file: " + path);
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::vector<std::string> splitLines(const std::string& source) {
    std::vector<std::string> lines;
    std::string current;

    for (char ch : source) {
        if (ch == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    lines.push_back(current);
    return lines;
}

bool parseSize(const std::string& text, std::size_t start, std::size_t end, std::size_t& value) {
    if (start >= end) {
        return false;
    }

    std::size_t result = 0;
    for (std::size_t i = start; i < end; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        result = result * 10 + static_cast<std::size_t>(text[i] - '0');
    }

    value = result;
    return true;
}

ParsedDiagnostic parseDiagnostic(const std::string& diagnostic) {
    for (std::size_t firstColon = 0; firstColon < diagnostic.size(); ++firstColon) {
        if (diagnostic[firstColon] != ':') {
            continue;
        }

        const std::size_t secondColon = diagnostic.find(':', firstColon + 1);
        if (secondColon == std::string::npos) {
            break;
        }

        const std::size_t thirdColon = diagnostic.find(':', secondColon + 1);
        if (thirdColon == std::string::npos) {
            break;
        }

        std::size_t line = 0;
        std::size_t column = 0;
        if (!parseSize(diagnostic, firstColon + 1, secondColon, line)
            || !parseSize(diagnostic, secondColon + 1, thirdColon, column)) {
            continue;
        }

        ParsedDiagnostic parsed;
        parsed.hasLocation = true;
        parsed.file = diagnostic.substr(0, firstColon);
        parsed.line = line;
        parsed.column = column;
        parsed.message = diagnostic.substr(thirdColon + 1);
        if (!parsed.message.empty() && parsed.message.front() == ' ') {
            parsed.message.erase(parsed.message.begin());
        }
        return parsed;
    }

    ParsedDiagnostic parsed;
    parsed.message = diagnostic;
    return parsed;
}

void printDiagnostic(std::ostream& out, const std::string& diagnostic, const std::string& source) {
    const ParsedDiagnostic parsed = parseDiagnostic(diagnostic);
    if (!parsed.hasLocation) {
        out << "error: " << diagnostic << '\n';
        return;
    }

    out << "error: " << parsed.message << '\n';
    out << " --> " << parsed.file << ':' << parsed.line << ':' << parsed.column << '\n';

    const std::vector<std::string> lines = splitLines(source);
    if (parsed.line == 0 || parsed.line > lines.size()) {
        return;
    }

    const std::string& line = lines[parsed.line - 1];
    const std::size_t width = std::to_string(parsed.line).size();
    out << std::setw(static_cast<int>(width)) << "" << " |\n";
    out << parsed.line << " | " << line << '\n';
    out << std::setw(static_cast<int>(width)) << "" << " | ";

    const std::size_t caretColumn = parsed.column == 0 ? 1 : parsed.column;
    for (std::size_t i = 1; i < caretColumn; ++i) {
        out << (i <= line.size() && line[i - 1] == '\t' ? '\t' : ' ');
    }
    out << "^\n";
}

void printDiagnostic(std::ostream& out, const std::string& diagnostic, const std::unordered_map<std::string, std::string>& sources) {
    const ParsedDiagnostic parsed = parseDiagnostic(diagnostic);
    if (!parsed.hasLocation) {
        out << "error: " << diagnostic << '\n';
        return;
    }

    const auto found = sources.find(parsed.file);
    if (found == sources.end()) {
        out << "error: " << parsed.message << '\n';
        out << " --> " << parsed.file << ':' << parsed.line << ':' << parsed.column << '\n';
        return;
    }

    printDiagnostic(out, diagnostic, found->second);
}

void printDiagnostics(std::ostream& out, const std::vector<std::string>& diagnostics, const std::string& source) {
    for (const std::string& diagnostic : diagnostics) {
        printDiagnostic(out, diagnostic, source);
    }
}

void printDiagnostics(std::ostream& out, const std::vector<std::string>& diagnostics, const std::unordered_map<std::string, std::string>& sources) {
    for (const std::string& diagnostic : diagnostics) {
        printDiagnostic(out, diagnostic, sources);
    }
}

std::unique_ptr<miko::Program> parseProgramOrThrow(const std::string& path, const std::string& source) {
    miko::Lexer lexer(path, source);
    std::vector<miko::Token> tokens = lexer.lexAll();

    for (const miko::Token& token : tokens) {
        if (token.kind == miko::TokenKind::Invalid) {
            throw std::runtime_error("invalid token at " + miko::formatToken(token));
        }
    }

    miko::Parser parser(std::move(tokens));
    std::unique_ptr<miko::Program> program = parser.parseProgram();

    if (!parser.errors().empty()) {
        std::ostringstream error;
        for (const std::string& message : parser.errors()) {
            const ParsedDiagnostic parsed = parseDiagnostic(message);
            error << "error: " << (parsed.hasLocation ? parsed.message : message) << '\n';
            if (parsed.hasLocation) {
                error << " --> " << parsed.file << ':' << parsed.line << ':' << parsed.column << '\n';
                const std::vector<std::string> lines = splitLines(source);
                if (parsed.line > 0 && parsed.line <= lines.size()) {
                    const std::size_t width = std::to_string(parsed.line).size();
                    error << std::setw(static_cast<int>(width)) << "" << " |\n";
                    error << parsed.line << " | " << lines[parsed.line - 1] << '\n';
                    error << std::setw(static_cast<int>(width)) << "" << " | ";
                    for (std::size_t i = 1; i < parsed.column; ++i) {
                        error << ' ';
                    }
                    error << "^\n";
                }
            }
        }
        throw std::runtime_error(error.str());
    }

    return program;
}

std::string canonicalPath(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }

    std::filesystem::path normalized = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        normalized = absolute.lexically_normal();
    }

    return normalized.string();
}

std::string sanitizeModuleName(std::string value) {
    for (char& ch : value) {
        if (ch == '\\' || ch == '/' || ch == '-' || ch == ' ' || ch == ':') {
            ch = '.';
        }
    }
    return value;
}

std::string moduleNameFromImport(const miko::ImportDecl& import, const std::string& resolvedPath) {
    if (!import.isStringPath()) {
        return import.path();
    }

    std::filesystem::path path(resolvedPath);
    path.replace_extension("");
    return sanitizeModuleName(path.string());
}

std::string defaultAliasForImport(const miko::ImportDecl& import, const std::string& moduleName) {
    if (!import.alias().empty()) {
        return import.alias();
    }

    if (!import.isStringPath()) {
        const std::size_t dot = moduleName.rfind('.');
        return dot == std::string::npos ? moduleName : moduleName.substr(dot + 1);
    }

    std::filesystem::path path(import.path());
    path.replace_extension("");
    return path.filename().string();
}

std::filesystem::path findStdlibRoot(const std::filesystem::path& start) {
    std::filesystem::path cursor = start;
    if (!std::filesystem::is_directory(cursor)) {
        cursor = cursor.parent_path();
    }

    while (!cursor.empty()) {
        std::error_code ignored;
        if (std::filesystem::is_directory(cursor / "stdlib", ignored)) {
            return cursor / "stdlib";
        }

        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }

    return std::filesystem::current_path() / "stdlib";
}

std::string resolveImportPath(const miko::ImportDecl& import, const std::filesystem::path& importerPath) {
    if (import.isStringPath()) {
        return canonicalPath(importerPath.parent_path() / import.path());
    }

    auto dottedPath = [](const std::filesystem::path& root, const std::string& dotted) {
        std::filesystem::path path = root;
        std::stringstream segments(dotted);
        std::string segment;
        while (std::getline(segments, segment, '.')) {
            path /= segment;
        }
        path.replace_extension(".miko");
        return path;
    };

    std::filesystem::path path = dottedPath(findStdlibRoot(importerPath), import.path());
    std::error_code ignored;
    if (std::filesystem::exists(path, ignored)) {
        return canonicalPath(path);
    }

    std::stringstream segments(import.path());
    std::string packageName;
    std::getline(segments, packageName, '.');
    std::string rest;
    std::getline(segments, rest, '\0');

    std::filesystem::path packageRoot = findStdlibRoot(importerPath).parent_path() / "packages" / packageName;
    if (!rest.empty() && rest.front() == '.') {
        rest.erase(rest.begin());
    }

    if (!rest.empty()) {
        path = dottedPath(packageRoot / "src", rest);
    } else {
        path = packageRoot / "src" / "main.miko";
    }
    if (std::filesystem::exists(path, ignored)) {
        return canonicalPath(path);
    }

    if (!rest.empty()) {
        path = dottedPath(packageRoot, rest);
    } else {
        path = packageRoot / "main.miko";
    }
    if (std::filesystem::exists(path, ignored)) {
        return canonicalPath(path);
    }

    path = dottedPath(findStdlibRoot(importerPath), import.path());
    return canonicalPath(path);
}

std::string physicalFunctionName(const std::string& moduleName, const std::string& localName, bool isEntryModule) {
    if (isEntryModule) {
        return localName;
    }

    std::string prefix = moduleName;
    for (char& ch : prefix) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }
    return "__miko_" + prefix + "__" + localName;
}

std::string physicalStructName(const std::string& moduleName, const std::string& localName, bool isEntryModule) {
    if (isEntryModule) {
        return localName;
    }

    std::string prefix = moduleName;
    for (char& ch : prefix) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }
    return "__miko_" + prefix + "__" + localName;
}

std::string trimCopy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::string rewriteTypeName(
    const std::string& typeName,
    const std::unordered_map<std::string, std::string>& localStructs,
    const std::unordered_map<std::string, ImportNamespace>& namespaces,
    const miko::SourceLocation& location) {
    if (typeName.empty()) {
        return typeName;
    }

    if (typeName.size() >= 5 && typeName.front() == '[' && typeName.back() == ']') {
        const std::string inner = typeName.substr(1, typeName.size() - 2);
        const std::size_t separator = inner.rfind(';');
        if (separator == std::string::npos) {
            return typeName;
        }

        const std::string element = trimCopy(inner.substr(0, separator));
        const std::string length = trimCopy(inner.substr(separator + 1));
        return "[" + rewriteTypeName(element, localStructs, namespaces, location) + "; " + length + "]";
    }

    if (typeName.size() >= 5 && typeName.compare(0, 4, "vec<") == 0 && typeName.back() == '>') {
        const std::string element = typeName.substr(4, typeName.size() - 5);
        return "vec<" + rewriteTypeName(element, localStructs, namespaces, location) + ">";
    }

    const auto localIt = localStructs.find(typeName);
    if (localIt != localStructs.end()) {
        return localIt->second;
    }

    const std::size_t dot = typeName.find('.');
    if (dot == std::string::npos) {
        return typeName;
    }

    const std::string alias = typeName.substr(0, dot);
    const std::string exportedName = typeName.substr(dot + 1);
    const auto namespaceIt = namespaces.find(alias);
    if (namespaceIt == namespaces.end()) {
        return typeName;
    }

    const auto exportIt = namespaceIt->second.structExports.find(exportedName);
    if (exportIt != namespaceIt->second.structExports.end()) {
        return exportIt->second;
    }

    const auto enumExportIt = namespaceIt->second.enumExports.find(exportedName);
    if (enumExportIt != namespaceIt->second.enumExports.end()) {
        return enumExportIt->second;
    }

    throw std::runtime_error(location.file + ":" + std::to_string(location.line) + ":" + std::to_string(location.column)
                             + ": module alias '" + alias + "' does not export struct '" + exportedName + "'");
}

std::vector<miko::StmtPtr>& mutableStatements(miko::BlockStmt& block) {
    return const_cast<std::vector<miko::StmtPtr>&>(block.statements());
}

miko::Expr& mutableExpr(const miko::Expr& expression) {
    return const_cast<miko::Expr&>(expression);
}

miko::Stmt& mutableStmt(const miko::Stmt& statement) {
    return const_cast<miko::Stmt&>(statement);
}

using LocalEnumMap = std::unordered_map<std::string, std::string>;

void rewriteExpr(
    miko::Expr& expression,
    const std::unordered_map<std::string, std::string>& localFunctions,
    const std::unordered_map<std::string, std::string>& localStructs,
    const LocalEnumMap& localEnums,
    const std::unordered_map<std::string, ImportNamespace>& namespaces);

void rewriteStmt(
    miko::Stmt& statement,
    const std::unordered_map<std::string, std::string>& localFunctions,
    const std::unordered_map<std::string, std::string>& localStructs,
    const LocalEnumMap& localEnums,
    const std::unordered_map<std::string, ImportNamespace>& namespaces);

void rewriteBlock(
    miko::BlockStmt& block,
    const std::unordered_map<std::string, std::string>& localFunctions,
    const std::unordered_map<std::string, std::string>& localStructs,
    const LocalEnumMap& localEnums,
    const std::unordered_map<std::string, ImportNamespace>& namespaces) {
    for (auto& statement : mutableStatements(block)) {
        rewriteStmt(*statement, localFunctions, localStructs, localEnums, namespaces);
    }
}

void rewriteQualifiedCall(
    miko::CallExpr& call,
    const std::unordered_map<std::string, ImportNamespace>& namespaces) {
    const std::string& callee = call.callee();
    const std::size_t dot = callee.find('.');
    if (dot == std::string::npos) {
        return;
    }

    const std::string alias = callee.substr(0, dot);
    const std::string exportedName = callee.substr(dot + 1);
    const auto namespaceIt = namespaces.find(alias);
    if (namespaceIt == namespaces.end()) {
        return;
    }

    const auto exportIt = namespaceIt->second.exports.find(exportedName);
    if (exportIt == namespaceIt->second.exports.end()) {
        const miko::SourceLocation& location = call.location();
        throw std::runtime_error(location.file + ":" + std::to_string(location.line) + ":" + std::to_string(location.column)
                                 + ": module alias '" + alias + "' does not export function '" + exportedName + "'");
    }

    call.setCallee(exportIt->second);
}

void rewriteExpr(
    miko::Expr& expression,
    const std::unordered_map<std::string, std::string>& localFunctions,
    const std::unordered_map<std::string, std::string>& localStructs,
    const LocalEnumMap& localEnums,
    const std::unordered_map<std::string, ImportNamespace>& namespaces) {
    if (auto* variable = dynamic_cast<miko::VariableExpr*>(&expression)) {
        const std::string& name = variable->name();
        const std::size_t separator = name.find("::");
        if (separator != std::string::npos) {
            const std::string enumName = name.substr(0, separator);
            const auto localIt = localEnums.find(enumName);
            if (localIt != localEnums.end()) {
                variable->setName(localIt->second + name.substr(separator));
                return;
            }
            // Module-qualified member reference: `alias.Enum::Member`.
            const std::size_t dot = enumName.find('.');
            if (dot != std::string::npos) {
                const std::string alias = enumName.substr(0, dot);
                const std::string exportedName = enumName.substr(dot + 1);
                const auto namespaceIt = namespaces.find(alias);
                if (namespaceIt != namespaces.end()) {
                    const auto enumExportIt = namespaceIt->second.enumExports.find(exportedName);
                    if (enumExportIt != namespaceIt->second.enumExports.end()) {
                        variable->setName(enumExportIt->second + name.substr(separator));
                    }
                }
            }
        }
        return;
    }

    if (auto* call = dynamic_cast<miko::CallExpr*>(&expression)) {
        rewriteQualifiedCall(*call, namespaces);
        if (call->callee().find('.') == std::string::npos) {
            const auto localIt = localFunctions.find(call->callee());
            if (localIt != localFunctions.end()) {
                call->setCallee(localIt->second);
            }
        }
        for (auto& argument : call->arguments()) {
            rewriteExpr(*argument, localFunctions, localStructs, localEnums, namespaces);
        }
        return;
    }

    if (auto* binary = dynamic_cast<miko::BinaryExpr*>(&expression)) {
        rewriteExpr(mutableExpr(binary->left()), localFunctions, localStructs, localEnums, namespaces);
        rewriteExpr(mutableExpr(binary->right()), localFunctions, localStructs, localEnums, namespaces);
        return;
    }

    if (auto* unary = dynamic_cast<miko::UnaryExpr*>(&expression)) {
        rewriteExpr(mutableExpr(unary->operand()), localFunctions, localStructs, localEnums, namespaces);
        return;
    }

    if (auto* array = dynamic_cast<miko::ArrayExpr*>(&expression)) {
        for (const auto& element : array->elements()) {
            rewriteExpr(*element, localFunctions, localStructs, localEnums, namespaces);
        }
        return;
    }

    if (auto* index = dynamic_cast<miko::IndexExpr*>(&expression)) {
        rewriteExpr(mutableExpr(index->target()), localFunctions, localStructs, localEnums, namespaces);
        rewriteExpr(mutableExpr(index->index()), localFunctions, localStructs, localEnums, namespaces);
        return;
    }

    if (auto* field = dynamic_cast<miko::FieldAccessExpr*>(&expression)) {
        rewriteExpr(mutableExpr(field->target()), localFunctions, localStructs, localEnums, namespaces);
        return;
    }

    if (auto* literal = dynamic_cast<miko::StructLiteralExpr*>(&expression)) {
        auto localIt = localStructs.find(literal->typeName());
        if (localIt != localStructs.end()) {
            literal->setTypeName(localIt->second);
        } else {
            const std::string& typeName = literal->typeName();
            const std::size_t dot = typeName.find('.');
            if (dot != std::string::npos) {
                const std::string alias = typeName.substr(0, dot);
                const std::string exportedName = typeName.substr(dot + 1);
                const auto namespaceIt = namespaces.find(alias);
                if (namespaceIt != namespaces.end()) {
                    const auto exportIt = namespaceIt->second.structExports.find(exportedName);
                    if (exportIt == namespaceIt->second.structExports.end()) {
                        const miko::SourceLocation& location = literal->location();
                        throw std::runtime_error(location.file + ":" + std::to_string(location.line) + ":" + std::to_string(location.column)
                                                 + ": module alias '" + alias + "' does not export struct '" + exportedName + "'");
                    }
                    literal->setTypeName(exportIt->second);
                }
            }
        }

        for (auto& field : literal->fields()) {
            rewriteExpr(*field.second, localFunctions, localStructs, localEnums, namespaces);
        }
    }
}

void rewriteStmt(
    miko::Stmt& statement,
    const std::unordered_map<std::string, std::string>& localFunctions,
    const std::unordered_map<std::string, std::string>& localStructs,
    const LocalEnumMap& localEnums,
    const std::unordered_map<std::string, ImportNamespace>& namespaces) {
    if (auto* let = dynamic_cast<miko::LetStmt*>(&statement)) {
        if (!let->explicitType().empty()) {
            let->setExplicitType(rewriteTypeName(let->explicitType(), localStructs, namespaces, let->location()));
        }
        if (const miko::Expr* initializer = let->initializer()) {
            rewriteExpr(mutableExpr(*initializer), localFunctions, localStructs, localEnums, namespaces);
        }
        return;
    }

    if (auto* assign = dynamic_cast<miko::AssignStmt*>(&statement)) {
        rewriteExpr(mutableExpr(assign->value()), localFunctions, localStructs, localEnums, namespaces);
        return;
    }

    if (auto* assign = dynamic_cast<miko::IndexAssignStmt*>(&statement)) {
        rewriteExpr(mutableExpr(assign->index()), localFunctions, localStructs, localEnums, namespaces);
        rewriteExpr(mutableExpr(assign->value()), localFunctions, localStructs, localEnums, namespaces);
        return;
    }

    if (auto* returnStmt = dynamic_cast<miko::ReturnStmt*>(&statement)) {
        if (const miko::Expr* value = returnStmt->value()) {
            rewriteExpr(mutableExpr(*value), localFunctions, localStructs, localEnums, namespaces);
        }
        return;
    }

    if (auto* exprStmt = dynamic_cast<miko::ExprStmt*>(&statement)) {
        rewriteExpr(mutableExpr(exprStmt->expression()), localFunctions, localStructs, localEnums, namespaces);
        return;
    }

    if (auto* block = dynamic_cast<miko::BlockStmt*>(&statement)) {
        rewriteBlock(*block, localFunctions, localStructs, localEnums, namespaces);
        return;
    }

    if (auto* ifStmt = dynamic_cast<miko::IfStmt*>(&statement)) {
        rewriteExpr(mutableExpr(ifStmt->condition()), localFunctions, localStructs, localEnums, namespaces);
        rewriteBlock(const_cast<miko::BlockStmt&>(ifStmt->thenBlock()), localFunctions, localStructs, localEnums, namespaces);
        if (const miko::BlockStmt* elseBlock = ifStmt->elseBlock()) {
            rewriteBlock(const_cast<miko::BlockStmt&>(*elseBlock), localFunctions, localStructs, localEnums, namespaces);
        }
        return;
    }

    if (auto* whileStmt = dynamic_cast<miko::WhileStmt*>(&statement)) {
        rewriteExpr(mutableExpr(whileStmt->condition()), localFunctions, localStructs, localEnums, namespaces);
        rewriteBlock(const_cast<miko::BlockStmt&>(whileStmt->body()), localFunctions, localStructs, localEnums, namespaces);
        return;
    }

    if (auto* forStmt = dynamic_cast<miko::ForStmt*>(&statement)) {
        if (const miko::Stmt* initializer = forStmt->initializer()) {
            rewriteStmt(mutableStmt(*initializer), localFunctions, localStructs, localEnums, namespaces);
        }
        if (const miko::Expr* condition = forStmt->condition()) {
            rewriteExpr(mutableExpr(*condition), localFunctions, localStructs, localEnums, namespaces);
        }
        if (const miko::Stmt* increment = forStmt->increment()) {
            rewriteStmt(mutableStmt(*increment), localFunctions, localStructs, localEnums, namespaces);
        }
        rewriteBlock(const_cast<miko::BlockStmt&>(forStmt->body()), localFunctions, localStructs, localEnums, namespaces);
    }
}

void rewriteModuleDeclarations(
    std::vector<std::unique_ptr<miko::StructDecl>>& structs,
    std::vector<std::unique_ptr<miko::EnumDecl>>& enums,
    std::vector<std::unique_ptr<miko::FunctionDecl>>& functions,
    const std::string& moduleName,
    bool isEntryModule,
    const std::unordered_map<std::string, ImportNamespace>& namespaces) {
    std::unordered_map<std::string, std::string> localStructs;
    for (const auto& structure : structs) {
        localStructs[structure->name()] = physicalStructName(moduleName, structure->name(), isEntryModule);
    }

    LocalEnumMap localEnums;
    for (const auto& enumeration : enums) {
        localEnums[enumeration->name()] = physicalStructName(moduleName, enumeration->name(), isEntryModule);
    }

    for (auto& structure : structs) {
        for (miko::Parameter& field : structure->fields()) {
            field.setType(rewriteTypeName(field.type(), localStructs, namespaces, structure->location()));
        }
        structure->setName(localStructs[structure->name()]);
    }

    for (auto& enumeration : enums) {
        enumeration->setName(localEnums[enumeration->name()]);
    }

    std::unordered_map<std::string, std::string> localFunctions;
    for (const auto& function : functions) {
        localFunctions[function->name()] = physicalFunctionName(moduleName, function->name(), isEntryModule);
    }

    for (auto& function : functions) {
        for (miko::Parameter& parameter : function->parameters()) {
            parameter.setType(rewriteTypeName(parameter.type(), localStructs, namespaces, function->location()));
        }
        if (!function->returnType().empty()) {
            function->setReturnType(rewriteTypeName(function->returnType(), localStructs, namespaces, function->location()));
        }
    }

    for (auto& function : functions) {
        rewriteBlock(function->body(), localFunctions, localStructs, localEnums, namespaces);
        function->setName(localFunctions[function->name()]);
    }
}

std::string moduleNameForEntry(const std::string& path) {
    std::filesystem::path modulePath(path);
    modulePath.replace_extension("");
    return sanitizeModuleName(modulePath.filename().string());
}

ModuleInfo loadProgramRecursive(
    const std::string& path,
    std::vector<std::unique_ptr<miko::StructDecl>>& structs,
    std::vector<std::unique_ptr<miko::EnumDecl>>& enums,
    std::vector<std::unique_ptr<miko::FunctionDecl>>& functions,
    std::unordered_map<std::string, std::string>& sources,
    std::unordered_set<std::string>& loading,
    std::unordered_map<std::string, ModuleInfo>& loadedModules,
    bool isEntryModule,
    const std::string& requestedModuleName);

ImportNamespace loadImportNamespace(
    const miko::ImportDecl& import,
    const std::string& importerPath,
    std::vector<std::unique_ptr<miko::StructDecl>>& structs,
    std::vector<std::unique_ptr<miko::EnumDecl>>& enums,
    std::vector<std::unique_ptr<miko::FunctionDecl>>& functions,
    std::unordered_map<std::string, std::string>& sources,
    std::unordered_set<std::string>& loading,
    std::unordered_map<std::string, ModuleInfo>& loadedModules) {
    const std::string resolvedPath = resolveImportPath(import, importerPath);
    const std::string moduleName = moduleNameFromImport(import, resolvedPath);
    ModuleInfo module = loadProgramRecursive(resolvedPath, structs, enums, functions, sources, loading, loadedModules, false, moduleName);
    return ImportNamespace{module.moduleName, module.exports, module.structExports, module.enumExports};
}

ModuleInfo loadProgramRecursive(
    const std::string& path,
    std::vector<std::unique_ptr<miko::StructDecl>>& structs,
    std::vector<std::unique_ptr<miko::EnumDecl>>& enums,
    std::vector<std::unique_ptr<miko::FunctionDecl>>& functions,
    std::unordered_map<std::string, std::string>& sources,
    std::unordered_set<std::string>& loading,
    std::unordered_map<std::string, ModuleInfo>& loadedModules,
    bool isEntryModule,
    const std::string& requestedModuleName) {
    const std::string canonical = canonicalPath(path);
    const auto loaded = loadedModules.find(canonical);
    if (loaded != loadedModules.end()) {
        return loaded->second;
    }
    if (loading.find(canonical) != loading.end()) {
        throw std::runtime_error("cyclic import involving " + canonical);
    }

    loading.insert(canonical);
    const std::string source = readFile(canonical);
    sources[canonical] = source;
    std::unique_ptr<miko::Program> parsed = parseProgramOrThrow(canonical, source);

    std::unordered_map<std::string, ImportNamespace> namespaces;
    for (const auto& import : parsed->imports()) {
        ImportNamespace imported = loadImportNamespace(*import, canonical, structs, enums, functions, sources, loading, loadedModules);
        const std::string alias = defaultAliasForImport(*import, imported.moduleName);
        if (namespaces.find(alias) != namespaces.end()) {
            const miko::SourceLocation& location = import->location();
            throw std::runtime_error(location.file + ":" + std::to_string(location.line) + ":" + std::to_string(location.column)
                                     + ": duplicate import alias '" + alias + "'");
        }
        namespaces[alias] = std::move(imported);
    }

    const std::string moduleName = requestedModuleName.empty() ? moduleNameForEntry(canonical) : requestedModuleName;
    ModuleInfo info;
    info.path = canonical;
    info.moduleName = moduleName;

    std::unordered_map<std::string, std::string> exports;
    std::unordered_map<std::string, std::string> structExports;
    std::unordered_map<std::string, std::string> enumExports;
    for (const auto& structure : parsed->structs()) {
        if (isEntryModule || structure->isPublic()) {
            structExports[structure->name()] = physicalStructName(moduleName, structure->name(), isEntryModule);
        }
    }

    for (const auto& enumeration : parsed->enums()) {
        if (isEntryModule || enumeration->isPublic()) {
            enumExports[enumeration->name()] = physicalStructName(moduleName, enumeration->name(), isEntryModule);
        }
    }

    for (const auto& function : parsed->functions()) {
        if (isEntryModule || function->isPublic()) {
            exports[function->name()] = physicalFunctionName(moduleName, function->name(), isEntryModule);
        }
    }

    std::vector<std::unique_ptr<miko::StructDecl>> localStructs = parsed->takeStructs();
    std::vector<std::unique_ptr<miko::EnumDecl>> localEnums = parsed->takeEnums();
    std::vector<std::unique_ptr<miko::FunctionDecl>> localFunctions = parsed->takeFunctions();
    rewriteModuleDeclarations(localStructs, localEnums, localFunctions, moduleName, isEntryModule, namespaces);
    info.exports = std::move(exports);
    info.structExports = std::move(structExports);
    info.enumExports = std::move(enumExports);

    for (auto& structure : localStructs) {
        structure->setPublic(true);
        structs.push_back(std::move(structure));
    }

    for (auto& enumeration : localEnums) {
        enumeration->setPublic(true);
        enums.push_back(std::move(enumeration));
    }

    for (auto& function : localFunctions) {
        function->setPublic(true);
        functions.push_back(std::move(function));
    }

    loading.erase(canonical);
    loadedModules[canonical] = info;
    return info;
}

LoadedProgram loadProgramWithImports(const std::string& inputPath) {
    std::vector<std::unique_ptr<miko::StructDecl>> structs;
    std::vector<std::unique_ptr<miko::EnumDecl>> enums;
    std::vector<std::unique_ptr<miko::FunctionDecl>> functions;
    std::unordered_map<std::string, std::string> sources;
    std::unordered_set<std::string> loading;
    std::unordered_map<std::string, ModuleInfo> loadedModules;

    loadProgramRecursive(canonicalPath(inputPath), structs, enums, functions, sources, loading, loadedModules, true, "");
    auto program = std::make_unique<miko::Program>(std::vector<std::unique_ptr<miko::ImportDecl>>{}, std::move(structs), std::move(enums), std::move(functions));
    return LoadedProgram{std::move(program), std::move(sources)};
}

bool analyzeOrPrintErrors(const miko::Program& program, const std::unordered_map<std::string, std::string>& sources) {
    miko::Sema sema;
    if (sema.analyze(program)) {
        return true;
    }

    printDiagnostics(std::cerr, sema.errors(), sources);
    return false;
}

std::string defaultOutputPath(const std::string& inputFile, const Options& options) {
    std::filesystem::path p(inputFile);
    if (options.emitIr) {
        p.replace_extension(".ll");
        return p.string();
    }
    if (options.emitObj) {
#ifdef _WIN32
        p.replace_extension(".obj");
#else
        p.replace_extension(".o");
#endif
        return p.string();
    }
    if (options.emitAsm) {
        p.replace_extension(".s");
        return p.string();
    }

#ifdef _WIN32
    p.replace_extension(".exe");
#else
    p.replace_extension("");
#endif
    return p.string();
}

int run(const Options& options) {
    if (options.showHelp) {
        std::cout << kUsage;
        return 0;
    }

    if (options.showVersion) {
        std::cout << "miko " << MIKO_VERSION;
#if MIKO_ENABLE_LLVM
        std::cout << " (LLVM enabled)";
#else
        std::cout << " (LLVM disabled)";
#endif
        std::cout << '\n';
        return 0;
    }

    if (options.inputFile.empty()) {
        std::cerr << kUsage;
        return 1;
    }

    const std::string source = readFile(options.inputFile);

    if (options.debugTokens) {
        miko::Lexer lexer(options.inputFile, source);
        for (const miko::Token& token : lexer.lexAll()) {
            std::cout << miko::formatToken(token) << '\n';
            if (token.kind == miko::TokenKind::Invalid) {
                return 1;
            }
        }
        return 0;
    }

    if (options.debugAst) {
        const std::unique_ptr<miko::Program> program = parseProgramOrThrow(options.inputFile, source);
        program->dump(std::cout);
        return 0;
    }

    if (options.debugTypes) {
        LoadedProgram loaded = loadProgramWithImports(options.inputFile);
        miko::Sema sema;
        if (!sema.analyze(*loaded.program)) {
            printDiagnostics(std::cerr, sema.errors(), loaded.sources);
            return 1;
        }

        sema.dumpTypes(std::cout);
        return 0;
    }

    if (options.runProgram) {
        LoadedProgram loaded = loadProgramWithImports(options.inputFile);
        if (!analyzeOrPrintErrors(*loaded.program, loaded.sources)) {
            return 1;
        }

        miko::Interpreter interpreter(std::cout, options.programArgs);
        if (!interpreter.run(*loaded.program)) {
            for (const std::string& error : interpreter.errors()) {
                std::cerr << "error: " << error << '\n';
            }
            return 1;
        }

        return 0;
    }

    if (options.compile) {
#if MIKO_ENABLE_LLVM
        const int emitModeCount = (options.emitIr ? 1 : 0) + (options.emitObj ? 1 : 0) + (options.emitAsm ? 1 : 0);
        if (emitModeCount > 1) {
            std::cerr << "error: choose only one of --emit-ir, --emit-obj, or --emit-asm\n";
            return 1;
        }

        LoadedProgram loaded = loadProgramWithImports(options.inputFile);
        if (!analyzeOrPrintErrors(*loaded.program, loaded.sources)) {
            return 1;
        }

        miko::NativeCompileOptions compileOpts;
        compileOpts.inputPath = options.inputFile;
        compileOpts.outputPath = options.outputFile.empty() ? defaultOutputPath(options.inputFile, options) : options.outputFile;
        compileOpts.targetTriple = options.targetTriple;
        compileOpts.optLevel = options.optLevel;
        compileOpts.keepIr = options.keepIr;
        compileOpts.emitIr = options.emitIr;
        compileOpts.emitObj = options.emitObj;
        compileOpts.emitBc = false;
        compileOpts.emitAsm = options.emitAsm;
        compileOpts.clangPath = options.clangPath.empty() ? "clang" : options.clangPath;

        miko::NativeCompileResult result = miko::compileNativeExecutable(*loaded.program, compileOpts);

        if (!result.success) {
            for (const std::string& error : result.errors) {
                std::cerr << "error: " << error << '\n';
            }
            return 1;
        }

        std::cout << "compiled " << options.inputFile << " -> " << compileOpts.outputPath;
        if (!result.objPath.empty()) {
            std::cout << " (object: " << result.objPath << ")";
        }
        if (!result.asmPath.empty()) {
            std::cout << " (asm: " << result.asmPath << ")";
        }
        if (!result.irPath.empty()) {
            std::cout << " (ir: " << result.irPath << ")";
        }
        std::cout << '\n';
        return 0;
#else
        std::cerr << "error: --compile requires LLVM support. Rebuild with -DMIKO_ENABLE_LLVM=ON\n";
        return 1;
#endif
    }

    std::cerr << "error: no compilation stage selected yet. Try --compile, --run, --debug-tokens, --debug-ast, or --debug-types.\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string> args(argv, argv + argc);
        return run(parseArgs(args));
    } catch (const std::exception& error) {
        const std::string message = error.what();
        if (message.rfind("error:", 0) == 0) {
            std::cerr << message;
            if (message.empty() || message.back() != '\n') {
                std::cerr << '\n';
            }
        } else {
            std::cerr << "error: " << message << '\n';
        }
        return 1;
    }
}
