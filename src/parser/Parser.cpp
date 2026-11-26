#include "miko/parser/Parser.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace miko {

namespace {

template <typename T, typename... Args>
std::unique_ptr<T> makeLocatedNode(const SourceLocation& location, Args&&... args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
    node->setLocation(location);
    return node;
}

} // namespace

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
}

std::unique_ptr<Program> Parser::parseProgram() {
    std::vector<std::unique_ptr<ImportDecl>> imports;
    std::vector<std::unique_ptr<StructDecl>> structs;
    std::vector<std::unique_ptr<EnumDecl>> enums;
    std::vector<std::unique_ptr<FunctionDecl>> functions;

    while (!isAtEnd()) {
        if (check(TokenKind::KwImport)) {
            if (auto import = parseImport()) {
                imports.push_back(std::move(import));
            }
        } else if (check(TokenKind::KwStruct)) {
            if (auto structure = parseStruct()) {
                structs.push_back(std::move(structure));
            }
        } else if (check(TokenKind::KwEnum)) {
            if (auto enumeration = parseEnum()) {
                enums.push_back(std::move(enumeration));
            }
        } else if (check(TokenKind::KwFn) || check(TokenKind::KwPub)) {
            if (check(TokenKind::KwPub) && peek().kind == TokenKind::KwStruct) {
                advance();
                if (auto structure = parseStruct(true)) {
                    structs.push_back(std::move(structure));
                }
                continue;
            }
            if (check(TokenKind::KwPub) && peek().kind == TokenKind::KwEnum) {
                advance();
                if (auto enumeration = parseEnum(true)) {
                    enums.push_back(std::move(enumeration));
                }
                continue;
            }
            if (auto function = parseFunction()) {
                functions.push_back(std::move(function));
            }
        } else {
            std::ostringstream message;
            message << current().location.file << ':' << current().location.line << ':' << current().location.column
                    << ": expected declaration";
            errors_.push_back(message.str());
            synchronize();
        }
    }

    auto program = std::make_unique<Program>(std::move(imports), std::move(structs), std::move(enums), std::move(functions));
    if (!tokens_.empty()) {
        program->setLocation(tokens_.front().location);
    }
    return program;
}

const std::vector<std::string>& Parser::errors() const {
    return errors_;
}

const Token& Parser::current() const {
    return tokens_[index_];
}

const Token& Parser::peek(std::size_t lookahead) const {
    const std::size_t target = index_ + lookahead;
    if (target >= tokens_.size()) {
        return tokens_.back();
    }

    return tokens_[target];
}

const Token& Parser::previous() const {
    return tokens_[index_ - 1];
}

bool Parser::isAtEnd() const {
    return current().kind == TokenKind::EndOfFile;
}

bool Parser::check(TokenKind kind) const {
    if (isAtEnd()) {
        return kind == TokenKind::EndOfFile;
    }

    return current().kind == kind;
}

bool Parser::match(TokenKind kind) {
    if (!check(kind)) {
        return false;
    }

    advance();
    return true;
}

const Token& Parser::advance() {
    if (!isAtEnd()) {
        ++index_;
    }

    return previous();
}

const Token& Parser::consume(TokenKind kind, const std::string& message) {
    if (check(kind)) {
        return advance();
    }

    std::ostringstream error;
    error << current().location.file << ':' << current().location.line << ':' << current().location.column
          << ": " << message << ", got " << tokenKindName(current().kind);
    errors_.push_back(error.str());

    static const Token invalidToken{TokenKind::Invalid, "", {}};
    return invalidToken;
}

void Parser::synchronize() {
    if (!isAtEnd()) {
        advance();
    }

    while (!isAtEnd()) {
        if (previous().kind == TokenKind::Semicolon) {
            return;
        }

        if (current().kind == TokenKind::KwFn || current().kind == TokenKind::KwPub || current().kind == TokenKind::KwStruct || current().kind == TokenKind::KwEnum || current().kind == TokenKind::KwLet || current().kind == TokenKind::KwConst
            || current().kind == TokenKind::KwImport || current().kind == TokenKind::KwReturn || current().kind == TokenKind::KwBreak
            || current().kind == TokenKind::KwContinue) {
            return;
        }

        advance();
    }
}

std::unique_ptr<ImportDecl> Parser::parseImport() {
    const Token& importToken = consume(TokenKind::KwImport, "expected 'import'");

    std::string path;
    bool isStringPath = false;
    if (match(TokenKind::StringLiteral)) {
        const std::string& lexeme = previous().lexeme;
        isStringPath = true;
        if (lexeme.size() >= 2) {
            path = lexeme.substr(1, lexeme.size() - 2);
        }
    } else {
        const Token& first = consume(TokenKind::Identifier, "expected module path after import");
        path = first.lexeme;
        while (match(TokenKind::Dot)) {
            const Token& segment = consume(TokenKind::Identifier, "expected module path segment after '.'");
            path += ".";
            path += segment.lexeme;
        }
    }

    std::string alias;
    if (match(TokenKind::KwAs)) {
        const Token& aliasToken = consume(TokenKind::Identifier, "expected import alias after 'as'");
        alias = aliasToken.lexeme;
    }

    consume(TokenKind::Semicolon, "expected ';' after import");
    return makeLocatedNode<ImportDecl>(importToken.location, std::move(path), std::move(alias), isStringPath);
}

std::unique_ptr<StructDecl> Parser::parseStruct(bool isPublic) {
    SourceLocation location = current().location;
    const Token& structToken = consume(TokenKind::KwStruct, "expected 'struct'");
    if (!isPublic) {
        location = structToken.location;
    }
    const Token& name = consume(TokenKind::Identifier, "expected struct name");
    consume(TokenKind::LeftBrace, "expected '{' after struct name");

    std::vector<Parameter> fields;
    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        const Token& fieldName = consume(TokenKind::Identifier, "expected struct field name");
        consume(TokenKind::Colon, "expected ':' after struct field name");
        fields.emplace_back(fieldName.lexeme, parseTypeName());
        if (!match(TokenKind::Semicolon)) {
            consume(TokenKind::Comma, "expected ';' or ',' after struct field");
        }
    }

    consume(TokenKind::RightBrace, "expected '}' after struct declaration");
    return makeLocatedNode<StructDecl>(location, name.lexeme, std::move(fields), isPublic);
}

std::unique_ptr<EnumDecl> Parser::parseEnum(bool isPublic) {
    SourceLocation location = current().location;
    const Token& enumToken = consume(TokenKind::KwEnum, "expected 'enum'");
    if (!isPublic) {
        location = enumToken.location;
    }
    const Token& name = consume(TokenKind::Identifier, "expected enum name");
    consume(TokenKind::LeftBrace, "expected '{' after enum name");

    std::vector<std::string> members;
    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        const Token& member = consume(TokenKind::Identifier, "expected enum member name");
        if (std::find(members.begin(), members.end(), member.lexeme) != members.end()) {
            errors_.push_back(member.location.file + ":" + std::to_string(member.location.line) + ":"
                              + std::to_string(member.location.column) + ": enum '" + name.lexeme
                              + "' already has a member named '" + member.lexeme + "'");
        } else {
            members.push_back(member.lexeme);
        }
        if (!match(TokenKind::Comma)) {
            break;
        }
    }

    consume(TokenKind::RightBrace, "expected '}' after enum declaration");
    return makeLocatedNode<EnumDecl>(location, name.lexeme, std::move(members), isPublic);
}

std::unique_ptr<FunctionDecl> Parser::parseFunction() {
    bool isPublic = false;
    SourceLocation location = current().location;
    if (match(TokenKind::KwPub)) {
        isPublic = true;
    }

    const Token& functionToken = consume(TokenKind::KwFn, "expected 'fn'");
    if (!isPublic) {
        location = functionToken.location;
    }
    const Token& name = consume(TokenKind::Identifier, "expected function name");
    consume(TokenKind::LeftParen, "expected '(' after function name");

    std::vector<Parameter> parameters;
    if (!check(TokenKind::RightParen)) {
        do {
            parameters.push_back(parseParameter());
        } while (match(TokenKind::Comma));
    }

    consume(TokenKind::RightParen, "expected ')' after parameter list");

    std::string returnType;
    if (match(TokenKind::Colon) || match(TokenKind::Arrow)) {
        returnType = parseTypeName();
    }

    auto body = parseBlock();
    return makeLocatedNode<FunctionDecl>(location, name.lexeme, std::move(parameters), std::move(returnType), std::move(body), isPublic);
}

Parameter Parser::parseParameter() {
    const Token& name = consume(TokenKind::Identifier, "expected parameter name");
    consume(TokenKind::Colon, "expected ':' after parameter name");
    return Parameter(name.lexeme, parseTypeName());
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    const Token& blockToken = consume(TokenKind::LeftBrace, "expected '{' before block");

    std::vector<StmtPtr> statements;
    while (!check(TokenKind::RightBrace) && !isAtEnd()) {
        if (auto statement = parseStatement()) {
            statements.push_back(std::move(statement));
        } else {
            synchronize();
        }
    }

    consume(TokenKind::RightBrace, "expected '}' after block");
    return makeLocatedNode<BlockStmt>(blockToken.location, std::move(statements));
}

StmtPtr Parser::parseStatement() {
    if (check(TokenKind::KwLet)) {
        return parseLetStatement();
    }

    if (check(TokenKind::KwConst)) {
        return parseConstStatement();
    }

    if (check(TokenKind::KwIf)) {
        return parseIfStatement();
    }

    if (check(TokenKind::KwWhile)) {
        return parseWhileStatement();
    }

    if (check(TokenKind::KwFor)) {
        return parseForStatement();
    }

    if (check(TokenKind::KwBreak)) {
        return parseBreakStatement();
    }

    if (check(TokenKind::KwContinue)) {
        return parseContinueStatement();
    }

    if (check(TokenKind::KwReturn)) {
        return parseReturnStatement();
    }

    if (check(TokenKind::Identifier) && peek().kind == TokenKind::LeftBracket) {
        return parseIndexAssignStatement();
    }

    if (check(TokenKind::Identifier) && peek().kind == TokenKind::Equal) {
        return parseAssignStatement();
    }

    return parseExpressionStatement();
}

StmtPtr Parser::parseLetStatement(bool consumeTerminator) {
    return parseVariableDeclaration(false, consumeTerminator);
}

StmtPtr Parser::parseConstStatement(bool consumeTerminator) {
    return parseVariableDeclaration(true, consumeTerminator);
}

StmtPtr Parser::parseVariableDeclaration(bool isConst, bool consumeTerminator) {
    const Token& declarationToken = consume(isConst ? TokenKind::KwConst : TokenKind::KwLet, isConst ? "expected 'const'" : "expected 'let'");
    const Token& name = consume(TokenKind::Identifier, "expected variable name");

    std::string explicitType;
    if (match(TokenKind::Colon)) {
        explicitType = parseTypeName();
    }

    ExprPtr initializer;
    if (match(TokenKind::Equal)) {
        initializer = parseExpression();
    }

    if (consumeTerminator) {
        consume(TokenKind::Semicolon, isConst ? "expected ';' after const statement" : "expected ';' after let statement");
    }
    return makeLocatedNode<LetStmt>(declarationToken.location, name.lexeme, std::move(explicitType), std::move(initializer), isConst);
}

StmtPtr Parser::parseAssignStatement(bool consumeTerminator) {
    const Token& name = consume(TokenKind::Identifier, "expected variable name");
    consume(TokenKind::Equal, "expected '=' in assignment");
    ExprPtr value = parseExpression();
    if (consumeTerminator) {
        consume(TokenKind::Semicolon, "expected ';' after assignment");
    }
    return makeLocatedNode<AssignStmt>(name.location, name.lexeme, std::move(value));
}

StmtPtr Parser::parseIndexAssignStatement(bool consumeTerminator) {
    const Token& name = consume(TokenKind::Identifier, "expected array variable name");
    consume(TokenKind::LeftBracket, "expected '[' before index expression");
    ExprPtr index = parseExpression();
    consume(TokenKind::RightBracket, "expected ']' after index expression");
    consume(TokenKind::Equal, "expected '=' in array element assignment");
    ExprPtr value = parseExpression();
    if (consumeTerminator) {
        consume(TokenKind::Semicolon, "expected ';' after array element assignment");
    }
    return makeLocatedNode<IndexAssignStmt>(name.location, name.lexeme, std::move(index), std::move(value));
}

std::unique_ptr<BlockStmt> Parser::parseElseBlock() {
    // `else if` - the else branch is another if statement rather than a block.
    // Before this, `else` unconditionally demanded '{', so `else if` was
    // reported as "expected '{' before block, got KwIf" by this frontend while
    // the self-hosted one accepted it: the same program was valid or invalid
    // depending on which compiler you used.
    if (check(TokenKind::KwIf)) {
        const SourceLocation location = peek().location;
        StmtPtr nested = parseIfStatement();
        std::vector<StmtPtr> statements;
        statements.push_back(std::move(nested));
        return std::make_unique<BlockStmt>(std::move(statements));
    }
    return parseBlock();
}

StmtPtr Parser::parseIfStatement() {
    const Token& ifToken = consume(TokenKind::KwIf, "expected 'if'");
    if (match(TokenKind::LeftParen)) {
        ExprPtr condition = parseExpression();
        consume(TokenKind::RightParen, "expected ')' after if condition");
        auto thenBlock = parseBlock();
        std::unique_ptr<BlockStmt> elseBlock;
        if (match(TokenKind::KwElse)) {
            elseBlock = parseElseBlock();
        }
        return makeLocatedNode<IfStmt>(ifToken.location, std::move(condition), std::move(thenBlock), std::move(elseBlock));
    }

    ExprPtr condition = parseExpression();
    auto thenBlock = parseBlock();
    std::unique_ptr<BlockStmt> elseBlock;
    if (match(TokenKind::KwElse)) {
        elseBlock = parseElseBlock();
    }
    return makeLocatedNode<IfStmt>(ifToken.location, std::move(condition), std::move(thenBlock), std::move(elseBlock));
}

StmtPtr Parser::parseWhileStatement() {
    const Token& whileToken = consume(TokenKind::KwWhile, "expected 'while'");
    if (match(TokenKind::LeftParen)) {
        ExprPtr condition = parseExpression();
        consume(TokenKind::RightParen, "expected ')' after while condition");
        return makeLocatedNode<WhileStmt>(whileToken.location, std::move(condition), parseBlock());
    }

    ExprPtr condition = parseExpression();
    return makeLocatedNode<WhileStmt>(whileToken.location, std::move(condition), parseBlock());
}

StmtPtr Parser::parseForStatement() {
    const Token& forToken = consume(TokenKind::KwFor, "expected 'for'");
    const bool hasParen = match(TokenKind::LeftParen);

    StmtPtr initializer;
    if (!check(TokenKind::Semicolon)) {
        if (check(TokenKind::KwLet)) {
            initializer = parseLetStatement(false);
        } else if (check(TokenKind::KwConst)) {
            initializer = parseConstStatement(false);
        } else if (check(TokenKind::Identifier) && peek().kind == TokenKind::LeftBracket) {
            initializer = parseIndexAssignStatement(false);
        } else if (check(TokenKind::Identifier) && peek().kind == TokenKind::Equal) {
            initializer = parseAssignStatement(false);
        } else {
            initializer = parseExpressionStatement(false);
        }
    }
    consume(TokenKind::Semicolon, "expected ';' after for initializer");

    ExprPtr condition;
    if (!check(TokenKind::Semicolon)) {
        condition = parseExpression();
    }
    consume(TokenKind::Semicolon, "expected ';' after for condition");

    StmtPtr increment;
    if ((hasParen && !check(TokenKind::RightParen)) || (!hasParen && !check(TokenKind::LeftBrace))) {
        increment = parseForIncrement();
    }

    if (hasParen) {
        consume(TokenKind::RightParen, "expected ')' after for clauses");
    }

    return makeLocatedNode<ForStmt>(forToken.location, std::move(initializer), std::move(condition), std::move(increment), parseBlock());
}

StmtPtr Parser::parseBreakStatement() {
    const Token& breakToken = consume(TokenKind::KwBreak, "expected 'break'");
    consume(TokenKind::Semicolon, "expected ';' after break");
    return makeLocatedNode<BreakStmt>(breakToken.location);
}

StmtPtr Parser::parseContinueStatement() {
    const Token& continueToken = consume(TokenKind::KwContinue, "expected 'continue'");
    consume(TokenKind::Semicolon, "expected ';' after continue");
    return makeLocatedNode<ContinueStmt>(continueToken.location);
}

StmtPtr Parser::parseReturnStatement() {
    const Token& returnToken = consume(TokenKind::KwReturn, "expected 'return'");

    ExprPtr value;
    if (!check(TokenKind::Semicolon)) {
        value = parseExpression();
    }

    consume(TokenKind::Semicolon, "expected ';' after return statement");
    return makeLocatedNode<ReturnStmt>(returnToken.location, std::move(value));
}

StmtPtr Parser::parseExpressionStatement(bool consumeTerminator) {
    ExprPtr expression = parseExpression();
    const SourceLocation location = expression->location();
    if (consumeTerminator) {
        consume(TokenKind::Semicolon, "expected ';' after expression");
    }
    return makeLocatedNode<ExprStmt>(location, std::move(expression));
}

StmtPtr Parser::parseForIncrement() {
    if (check(TokenKind::Identifier) && peek().kind == TokenKind::Equal) {
        return parseAssignStatement(false);
    }
    if (check(TokenKind::Identifier) && peek().kind == TokenKind::LeftBracket) {
        return parseIndexAssignStatement(false);
    }
    return parseExpressionStatement(false);
}

ExprPtr Parser::parseExpression() {
    return parseBinaryExpression(1);
}

ExprPtr Parser::parseBinaryExpression(int minPrecedence) {
    ExprPtr left = parseUnaryExpression();

    while (isBinaryOperator(current().kind) && precedence(current().kind) >= minPrecedence) {
        const Token op = advance();
        const int opPrecedence = precedence(op.kind);
        ExprPtr right = parseBinaryExpression(opPrecedence + 1);
        left = makeLocatedNode<BinaryExpr>(op.location, op.lexeme, std::move(left), std::move(right));
    }

    return left;
}

ExprPtr Parser::parseUnaryExpression() {
    if (match(TokenKind::Bang) || match(TokenKind::Minus)) {
        const Token op = previous();
        return makeLocatedNode<UnaryExpr>(op.location, op.lexeme, parseUnaryExpression());
    }

    return parsePostfixExpression();
}

ExprPtr Parser::parsePostfixExpression() {
    ExprPtr expression = parsePrimary();

    while (true) {
        if (match(TokenKind::LeftParen)) {
            auto* callee = dynamic_cast<VariableExpr*>(expression.get());
            if (!callee) {
                errors_.push_back(previous().location.file + ":" + std::to_string(previous().location.line) + ":"
                                  + std::to_string(previous().location.column) + ": only named functions can be called");
                parseArguments();
                continue;
            }

            const SourceLocation location = expression->location();
            std::string name = callee->name();
            expression = makeLocatedNode<CallExpr>(location, std::move(name), parseArguments());
            continue;
        }

        if (match(TokenKind::LeftBracket)) {
            const Token bracket = previous();
            ExprPtr index = parseExpression();
            consume(TokenKind::RightBracket, "expected ']' after index expression");
            expression = makeLocatedNode<IndexExpr>(bracket.location, std::move(expression), std::move(index));
            continue;
        }

        if (match(TokenKind::Dot)) {
            const Token& field = consume(TokenKind::Identifier, "expected field name after '.'");

            if (check(TokenKind::LeftBrace) && (peek().kind == TokenKind::RightBrace
                || (peek().kind == TokenKind::Identifier && peek(2).kind == TokenKind::Colon))) {
                auto* qualifier = dynamic_cast<VariableExpr*>(expression.get());
                if (!qualifier) {
                    errors_.push_back(field.location.file + ":" + std::to_string(field.location.line) + ":"
                                      + std::to_string(field.location.column) + ": struct literal type must be a name");
                    advance();
                    expression = makeLocatedNode<StructLiteralExpr>(field.location, field.lexeme, parseStructLiteralFields());
                    continue;
                }

                advance();
                expression = makeLocatedNode<StructLiteralExpr>(
                    qualifier->location(),
                    qualifier->name() + "." + field.lexeme,
                    parseStructLiteralFields());
                continue;
            }

            if (check(TokenKind::LeftParen)) {
                auto* qualifier = dynamic_cast<VariableExpr*>(expression.get());
                if (qualifier) {
                    advance();
                    expression = makeLocatedNode<CallExpr>(
                        qualifier->location(),
                        qualifier->name() + "." + field.lexeme,
                        parseArguments());
                    continue;
                }
            }

            expression = makeLocatedNode<FieldAccessExpr>(field.location, std::move(expression), field.lexeme);
            continue;
        }

        break;
    }

    return expression;
}

ExprPtr Parser::parsePrimary() {
    if (match(TokenKind::LeftBracket)) {
        const Token bracket = previous();
        return makeLocatedNode<ArrayExpr>(bracket.location, parseArrayElements());
    }

    if (match(TokenKind::IntegerLiteral)) {
        const Token token = previous();
        return makeLocatedNode<IntegerExpr>(token.location, token.lexeme);
    }

    if (match(TokenKind::FloatLiteral)) {
        const Token token = previous();
        return makeLocatedNode<FloatExpr>(token.location, token.lexeme);
    }

    if (match(TokenKind::StringLiteral)) {
        const Token token = previous();
        return makeLocatedNode<StringExpr>(token.location, token.lexeme);
    }

    if (match(TokenKind::CharLiteral)) {
        const Token token = previous();
        return makeLocatedNode<CharExpr>(token.location, token.lexeme);
    }

    if (match(TokenKind::KwTrue)) {
        return makeLocatedNode<BoolExpr>(previous().location, true);
    }

    if (match(TokenKind::KwFalse)) {
        return makeLocatedNode<BoolExpr>(previous().location, false);
    }

    if (match(TokenKind::Identifier)) {
        const Token name = previous();

        // Enum member access: `Color::Red` (optionally module-qualified:
        // `module.Color::Red`). Join into a single variable name; Sema and
        // the backends resolve the qualified name against declared enums.
        if (check(TokenKind::DoubleColon) || (check(TokenKind::Dot) && peek().kind == TokenKind::Identifier && peek(2).kind == TokenKind::DoubleColon)) {
            std::string qualified = name.lexeme;
            while (true) {
                if (check(TokenKind::Dot)) {
                    advance();
                    const Token& segment = consume(TokenKind::Identifier, "expected identifier after '.'");
                    qualified += ".";
                    qualified += segment.lexeme;
                    continue;
                }
                if (check(TokenKind::DoubleColon)) {
                    advance();
                    const Token& member = consume(TokenKind::Identifier, "expected enum member after '::'");
                    qualified += "::";
                    qualified += member.lexeme;
                    break;
                }
                break;
            }
            if (check(TokenKind::DoubleColon) || qualified.find("::") != std::string::npos) {
                return makeLocatedNode<VariableExpr>(name.location, qualified);
            }
        }

        if (check(TokenKind::LeftBrace) && (peek().kind == TokenKind::RightBrace
            || (peek().kind == TokenKind::Identifier && peek(2).kind == TokenKind::Colon))) {
            advance();
            return makeLocatedNode<StructLiteralExpr>(name.location, name.lexeme, parseStructLiteralFields());
        }
        return makeLocatedNode<VariableExpr>(name.location, name.lexeme);
    }

    if (match(TokenKind::LeftParen)) {
        ExprPtr expression = parseExpression();
        consume(TokenKind::RightParen, "expected ')' after expression");
        return expression;
    }

    std::ostringstream error;
    error << current().location.file << ':' << current().location.line << ':' << current().location.column
          << ": expected expression, got " << tokenKindName(current().kind);
    errors_.push_back(error.str());

    if (!isAtEnd()) {
        advance();
    }

    return makeLocatedNode<VariableExpr>(current().location, "<error>");
}

std::vector<ExprPtr> Parser::parseArguments() {
    std::vector<ExprPtr> arguments;

    if (!check(TokenKind::RightParen)) {
        do {
            arguments.push_back(parseExpression());
        } while (match(TokenKind::Comma));
    }

    consume(TokenKind::RightParen, "expected ')' after arguments");
    return arguments;
}

std::vector<ExprPtr> Parser::parseArrayElements() {
    std::vector<ExprPtr> elements;

    if (!check(TokenKind::RightBracket)) {
        do {
            elements.push_back(parseExpression());
        } while (match(TokenKind::Comma));
    }

    consume(TokenKind::RightBracket, "expected ']' after array literal");
    return elements;
}

std::vector<StructFieldInit> Parser::parseStructLiteralFields() {
    std::vector<StructFieldInit> fields;

    if (!check(TokenKind::RightBrace)) {
        do {
            const Token& name = consume(TokenKind::Identifier, "expected struct literal field name");
            consume(TokenKind::Colon, "expected ':' after struct literal field name");
            fields.emplace_back(name.lexeme, parseExpression());
        } while (match(TokenKind::Comma));
    }

    consume(TokenKind::RightBrace, "expected '}' after struct literal");
    return fields;
}

std::string Parser::parseTypeName() {
    if (match(TokenKind::LeftBracket)) {
        std::string elementType = parseTypeName();
        consume(TokenKind::Semicolon, "expected ';' after array element type");
        const Token& length = consume(TokenKind::IntegerLiteral, "expected array length");
        consume(TokenKind::RightBracket, "expected ']' after array type");
        return "[" + elementType + "; " + length.lexeme + "]";
    }

    if (check(TokenKind::Identifier) && current().lexeme == "vec" && peek().kind == TokenKind::Less) {
        advance();
        advance();
        std::string elementType = parseTypeName();
        consume(TokenKind::Greater, "expected '>' after vec element type");
        return "vec<" + elementType + ">";
    }

    const Token& typeName = consume(TokenKind::Identifier, "expected type name");
    std::string name = typeName.lexeme;
    while (match(TokenKind::Dot)) {
        const Token& segment = consume(TokenKind::Identifier, "expected type name segment after '.'");
        name += ".";
        name += segment.lexeme;
    }
    return name;
}

int Parser::precedence(TokenKind kind) {
    switch (kind) {
    case TokenKind::OrOr:
        return 1;
    case TokenKind::AndAnd:
        return 2;
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
        return 3;
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
        return 4;
    case TokenKind::Plus:
    case TokenKind::Minus:
        return 5;
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
        return 6;
    default:
        return 0;
    }
}

bool Parser::isBinaryOperator(TokenKind kind) {
    return precedence(kind) > 0;
}

} // namespace miko
