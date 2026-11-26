#pragma once

#include "miko/ast/Ast.h"
#include "miko/lexer/Token.h"

#include <memory>
#include <string>
#include <vector>

namespace miko {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    std::unique_ptr<Program> parseProgram();
    const std::vector<std::string>& errors() const;

private:
    const Token& current() const;
    const Token& peek(std::size_t lookahead = 1) const;
    const Token& previous() const;
    bool isAtEnd() const;
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    const Token& advance();
    const Token& consume(TokenKind kind, const std::string& message);
    void synchronize();

    std::unique_ptr<ImportDecl> parseImport();
    std::unique_ptr<StructDecl> parseStruct(bool isPublic = false);
    std::unique_ptr<EnumDecl> parseEnum(bool isPublic = false);
    std::unique_ptr<FunctionDecl> parseFunction();
    Parameter parseParameter();
    std::unique_ptr<BlockStmt> parseBlock();
    // Parses the branch after `else`. `else if` is a nested if wrapped in a
    // synthetic one-statement block, so the AST keeps if/else as a binary node.
    std::unique_ptr<BlockStmt> parseElseBlock();
    StmtPtr parseStatement();
    StmtPtr parseLetStatement(bool consumeTerminator = true);
    StmtPtr parseConstStatement(bool consumeTerminator = true);
    StmtPtr parseVariableDeclaration(bool isConst, bool consumeTerminator);
    StmtPtr parseAssignStatement(bool consumeTerminator = true);
    StmtPtr parseIndexAssignStatement(bool consumeTerminator = true);
    StmtPtr parseIfStatement();
    StmtPtr parseWhileStatement();
    StmtPtr parseForStatement();
    StmtPtr parseBreakStatement();
    StmtPtr parseContinueStatement();
    StmtPtr parseReturnStatement();
    StmtPtr parseExpressionStatement(bool consumeTerminator = true);
    StmtPtr parseForIncrement();

    ExprPtr parseExpression();
    ExprPtr parseBinaryExpression(int minPrecedence);
    ExprPtr parseUnaryExpression();
    ExprPtr parsePostfixExpression();
    ExprPtr parsePrimary();
    std::vector<ExprPtr> parseArguments();
    std::vector<ExprPtr> parseArrayElements();
    std::vector<StructFieldInit> parseStructLiteralFields();
    std::string parseTypeName();

    static int precedence(TokenKind kind);
    static bool isBinaryOperator(TokenKind kind);

    std::vector<Token> tokens_;
    std::size_t index_ = 0;
    std::vector<std::string> errors_;
};

} // namespace miko
