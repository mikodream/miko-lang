#pragma once

#include "miko/lexer/Token.h"

#include <string>
#include <vector>

namespace miko {

class Lexer {
public:
    Lexer(std::string fileName, std::string source);

    Token nextToken();
    std::vector<Token> lexAll();

private:
    char current() const;
    char peek(std::size_t lookahead = 1) const;
    bool isAtEnd() const;
    bool match(char expected);
    char advance();

    void skipWhitespaceAndComments();
    Token makeToken(TokenKind kind, SourceLocation start, std::size_t startOffset);
    Token lexIdentifierOrKeyword();
    Token lexNumber();
    Token lexString();
    Token lexChar();
    Token lexInvalid(char ch, SourceLocation start, std::size_t startOffset);

    static TokenKind keywordKind(const std::string& text);

    std::string fileName_;
    std::string source_;
    std::size_t index_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

} // namespace miko
