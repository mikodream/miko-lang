#pragma once

#include "miko/SourceLocation.h"

#include <string>

namespace miko {

enum class TokenKind {
    EndOfFile,
    Invalid,

    Identifier,
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,

    KwFn,
    KwLet,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwBreak,
    KwContinue,
    KwTrue,
    KwFalse,
    KwConst,
    KwPub,
    KwAs,
    KwExtern,
    KwImport,
    KwStruct,
    KwEnum,

    Plus,
    Minus,
    Star,
    Slash,
    Percent,

    Equal,
    EqualEqual,
    Bang,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AndAnd,
    OrOr,

    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,

    Comma,
    Dot,
    Colon,
    DoubleColon,
    Semicolon,
    Arrow
};

struct Token {
    TokenKind kind = TokenKind::Invalid;
    std::string lexeme;
    SourceLocation location;
};

const char* tokenKindName(TokenKind kind);
std::string formatToken(const Token& token);

} // namespace miko
