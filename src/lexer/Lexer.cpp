#include "miko/lexer/Lexer.h"

#include <cctype>
#include <unordered_map>
#include <utility>

namespace miko {

Lexer::Lexer(std::string fileName, std::string source)
    : fileName_(std::move(fileName)), source_(std::move(source)) {
}

Token Lexer::nextToken() {
    skipWhitespaceAndComments();

    const SourceLocation start{fileName_, line_, column_, index_};
    const std::size_t startOffset = index_;

    if (isAtEnd()) {
        return makeToken(TokenKind::EndOfFile, start, startOffset);
    }

    const char ch = current();

    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
        return lexIdentifierOrKeyword();
    }

    if (std::isdigit(static_cast<unsigned char>(ch))) {
        return lexNumber();
    }

    if (ch == '"') {
        return lexString();
    }

    if (ch == '\'') {
        return lexChar();
    }

    advance();
    switch (ch) {
    case '+':
        return makeToken(TokenKind::Plus, start, startOffset);
    case '-':
        if (match('>')) {
            return makeToken(TokenKind::Arrow, start, startOffset);
        }
        return makeToken(TokenKind::Minus, start, startOffset);
    case '*':
        return makeToken(TokenKind::Star, start, startOffset);
    case '/':
        return makeToken(TokenKind::Slash, start, startOffset);
    case '%':
        return makeToken(TokenKind::Percent, start, startOffset);
    case '=':
        if (match('=')) {
            return makeToken(TokenKind::EqualEqual, start, startOffset);
        }
        return makeToken(TokenKind::Equal, start, startOffset);
    case '!':
        if (match('=')) {
            return makeToken(TokenKind::BangEqual, start, startOffset);
        }
        return makeToken(TokenKind::Bang, start, startOffset);
    case '<':
        if (match('=')) {
            return makeToken(TokenKind::LessEqual, start, startOffset);
        }
        return makeToken(TokenKind::Less, start, startOffset);
    case '>':
        if (match('=')) {
            return makeToken(TokenKind::GreaterEqual, start, startOffset);
        }
        return makeToken(TokenKind::Greater, start, startOffset);
    case '&':
        if (match('&')) {
            return makeToken(TokenKind::AndAnd, start, startOffset);
        }
        return lexInvalid(ch, start, startOffset);
    case '|':
        if (match('|')) {
            return makeToken(TokenKind::OrOr, start, startOffset);
        }
        return lexInvalid(ch, start, startOffset);
    case '(':
        return makeToken(TokenKind::LeftParen, start, startOffset);
    case ')':
        return makeToken(TokenKind::RightParen, start, startOffset);
    case '{':
        return makeToken(TokenKind::LeftBrace, start, startOffset);
    case '}':
        return makeToken(TokenKind::RightBrace, start, startOffset);
    case '[':
        return makeToken(TokenKind::LeftBracket, start, startOffset);
    case ']':
        return makeToken(TokenKind::RightBracket, start, startOffset);
    case ',':
        return makeToken(TokenKind::Comma, start, startOffset);
    case '.':
        return makeToken(TokenKind::Dot, start, startOffset);
    case ':':
        if (match(':')) {
            return makeToken(TokenKind::DoubleColon, start, startOffset);
        }
        return makeToken(TokenKind::Colon, start, startOffset);
    case ';':
        return makeToken(TokenKind::Semicolon, start, startOffset);
    default:
        return lexInvalid(ch, start, startOffset);
    }
}

std::vector<Token> Lexer::lexAll() {
    std::vector<Token> tokens;

    while (true) {
        Token token = nextToken();
        const bool done = token.kind == TokenKind::EndOfFile;
        tokens.push_back(std::move(token));

        if (done) {
            break;
        }
    }

    return tokens;
}

char Lexer::current() const {
    if (isAtEnd()) {
        return '\0';
    }

    return source_[index_];
}

char Lexer::peek(std::size_t lookahead) const {
    const std::size_t nextIndex = index_ + lookahead;
    if (nextIndex >= source_.size()) {
        return '\0';
    }

    return source_[nextIndex];
}

bool Lexer::isAtEnd() const {
    return index_ >= source_.size();
}

bool Lexer::match(char expected) {
    if (isAtEnd() || current() != expected) {
        return false;
    }

    advance();
    return true;
}

char Lexer::advance() {
    const char ch = current();
    ++index_;

    if (ch == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }

    return ch;
}

void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        const char ch = current();

        if (ch == ' ' || ch == '\r' || ch == '\t' || ch == '\n') {
            advance();
            continue;
        }

        if (ch == '/' && peek() == '/') {
            while (!isAtEnd() && current() != '\n') {
                advance();
            }
            continue;
        }

        if (ch == '/' && peek() == '*') {
            advance();
            advance();

            while (!isAtEnd()) {
                if (current() == '*' && peek() == '/') {
                    advance();
                    advance();
                    break;
                }
                advance();
            }
            continue;
        }

        break;
    }
}

Token Lexer::makeToken(TokenKind kind, SourceLocation start, std::size_t startOffset) {
    return Token{kind, source_.substr(startOffset, index_ - startOffset), std::move(start)};
}

Token Lexer::lexIdentifierOrKeyword() {
    const SourceLocation start{fileName_, line_, column_, index_};
    const std::size_t startOffset = index_;

    while (!isAtEnd()) {
        const char ch = current();
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            break;
        }
        advance();
    }

    const std::string text = source_.substr(startOffset, index_ - startOffset);
    return Token{keywordKind(text), text, start};
}

Token Lexer::lexNumber() {
    const SourceLocation start{fileName_, line_, column_, index_};
    const std::size_t startOffset = index_;
    bool isFloat = false;

    // 0x / 0b / 0o literals: consume the radix digit run and stop. They are
    // always integers, so no fractional part is looked for.
    if (current() == '0' && (peek() == 'x' || peek() == 'X' || peek() == 'b' || peek() == 'B' ||
                             peek() == 'o' || peek() == 'O')) {
        const char marker = peek();
        advance();
        advance();
        const bool hex = marker == 'x' || marker == 'X';
        while (!isAtEnd()) {
            const char ch = current();
            const bool digit = std::isdigit(static_cast<unsigned char>(ch)) ||
                               (hex && std::isxdigit(static_cast<unsigned char>(ch))) || ch == '_';
            if (!digit) {
                break;
            }
            advance();
        }
        return makeToken(TokenKind::IntegerLiteral, start, startOffset);
    }

    while (std::isdigit(static_cast<unsigned char>(current())) || current() == '_') {
        advance();
    }

    if (current() == '.' && std::isdigit(static_cast<unsigned char>(peek()))) {
        isFloat = true;
        advance();

        while (std::isdigit(static_cast<unsigned char>(current())) || current() == '_') {
            advance();
        }
    }

    // Exponent: only consumed when it really is one, so `1e` stays an error
    // rather than being silently split.
    if (current() == 'e' || current() == 'E') {
        const char next = peek();
        const bool signedExponent = (next == '+' || next == '-');
        const char afterSign = signedExponent ? peek(2) : next;
        if (std::isdigit(static_cast<unsigned char>(afterSign))) {
            isFloat = true;
            advance();
            if (signedExponent) {
                advance();
            }
            while (std::isdigit(static_cast<unsigned char>(current())) || current() == '_') {
                advance();
            }
        }
    }

    return makeToken(isFloat ? TokenKind::FloatLiteral : TokenKind::IntegerLiteral, start, startOffset);
}

Token Lexer::lexString() {
    const SourceLocation start{fileName_, line_, column_, index_};
    const std::size_t startOffset = index_;

    advance();

    while (!isAtEnd() && current() != '"') {
        if (current() == '\\' && !isAtEnd()) {
            advance();
        }
        advance();
    }

    if (isAtEnd()) {
        return Token{TokenKind::Invalid, source_.substr(startOffset), start};
    }

    advance();
    return makeToken(TokenKind::StringLiteral, start, startOffset);
}

Token Lexer::lexChar() {
    const SourceLocation start{fileName_, line_, column_, index_};
    const std::size_t startOffset = index_;

    advance();
    if (isAtEnd() || current() == '\n') {
        return Token{TokenKind::Invalid, source_.substr(startOffset, index_ - startOffset), start};
    }

    if (current() == '\\') {
        advance();
        if (isAtEnd() || current() == '\n') {
            return Token{TokenKind::Invalid, source_.substr(startOffset, index_ - startOffset), start};
        }
        advance();
    } else {
        advance();
    }

    if (isAtEnd() || current() != '\'') {
        return Token{TokenKind::Invalid, source_.substr(startOffset, index_ - startOffset), start};
    }

    advance();
    return makeToken(TokenKind::CharLiteral, start, startOffset);
}

Token Lexer::lexInvalid(char, SourceLocation start, std::size_t startOffset) {
    return makeToken(TokenKind::Invalid, std::move(start), startOffset);
}

TokenKind Lexer::keywordKind(const std::string& text) {
    static const std::unordered_map<std::string, TokenKind> keywords{
        {"fn", TokenKind::KwFn},
        {"let", TokenKind::KwLet},
        {"return", TokenKind::KwReturn},
        {"if", TokenKind::KwIf},
        {"else", TokenKind::KwElse},
        {"while", TokenKind::KwWhile},
        {"for", TokenKind::KwFor},
        {"break", TokenKind::KwBreak},
        {"continue", TokenKind::KwContinue},
        {"true", TokenKind::KwTrue},
        {"false", TokenKind::KwFalse},
        {"const", TokenKind::KwConst},
        {"pub", TokenKind::KwPub},
        {"as", TokenKind::KwAs},
        {"extern", TokenKind::KwExtern},
        {"import", TokenKind::KwImport},
        {"struct", TokenKind::KwStruct},
        {"enum", TokenKind::KwEnum},
    };

    const auto it = keywords.find(text);
    if (it != keywords.end()) {
        return it->second;
    }

    return TokenKind::Identifier;
}

} // namespace miko
