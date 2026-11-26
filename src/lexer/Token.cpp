#include "miko/lexer/Token.h"

#include <iomanip>
#include <sstream>

namespace miko {

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
    case TokenKind::EndOfFile:
        return "EndOfFile";
    case TokenKind::Invalid:
        return "Invalid";
    case TokenKind::Identifier:
        return "Identifier";
    case TokenKind::IntegerLiteral:
        return "IntegerLiteral";
    case TokenKind::FloatLiteral:
        return "FloatLiteral";
    case TokenKind::StringLiteral:
        return "StringLiteral";
    case TokenKind::CharLiteral:
        return "CharLiteral";
    case TokenKind::KwFn:
        return "KwFn";
    case TokenKind::KwLet:
        return "KwLet";
    case TokenKind::KwReturn:
        return "KwReturn";
    case TokenKind::KwIf:
        return "KwIf";
    case TokenKind::KwElse:
        return "KwElse";
    case TokenKind::KwWhile:
        return "KwWhile";
    case TokenKind::KwFor:
        return "KwFor";
    case TokenKind::KwBreak:
        return "KwBreak";
    case TokenKind::KwContinue:
        return "KwContinue";
    case TokenKind::KwTrue:
        return "KwTrue";
    case TokenKind::KwFalse:
        return "KwFalse";
    case TokenKind::KwConst:
        return "KwConst";
    case TokenKind::KwPub:
        return "KwPub";
    case TokenKind::KwAs:
        return "KwAs";
    case TokenKind::KwExtern:
        return "KwExtern";
    case TokenKind::KwImport:
        return "KwImport";
    case TokenKind::KwStruct:
        return "KwStruct";
    case TokenKind::KwEnum:
        return "KwEnum";
    case TokenKind::Plus:
        return "Plus";
    case TokenKind::Minus:
        return "Minus";
    case TokenKind::Star:
        return "Star";
    case TokenKind::Slash:
        return "Slash";
    case TokenKind::Percent:
        return "Percent";
    case TokenKind::Equal:
        return "Equal";
    case TokenKind::EqualEqual:
        return "EqualEqual";
    case TokenKind::Bang:
        return "Bang";
    case TokenKind::BangEqual:
        return "BangEqual";
    case TokenKind::Less:
        return "Less";
    case TokenKind::LessEqual:
        return "LessEqual";
    case TokenKind::Greater:
        return "Greater";
    case TokenKind::GreaterEqual:
        return "GreaterEqual";
    case TokenKind::AndAnd:
        return "AndAnd";
    case TokenKind::OrOr:
        return "OrOr";
    case TokenKind::LeftParen:
        return "LeftParen";
    case TokenKind::RightParen:
        return "RightParen";
    case TokenKind::LeftBrace:
        return "LeftBrace";
    case TokenKind::RightBrace:
        return "RightBrace";
    case TokenKind::LeftBracket:
        return "LeftBracket";
    case TokenKind::RightBracket:
        return "RightBracket";
    case TokenKind::Comma:
        return "Comma";
    case TokenKind::Dot:
        return "Dot";
    case TokenKind::Colon:
        return "Colon";
    case TokenKind::DoubleColon:
        return "DoubleColon";
    case TokenKind::Semicolon:
        return "Semicolon";
    case TokenKind::Arrow:
        return "Arrow";
    }

    return "Unknown";
}

std::string formatToken(const Token& token) {
    std::ostringstream out;
    out << token.location.file << ':' << token.location.line << ':' << token.location.column
        << "  " << std::left << std::setw(16) << tokenKindName(token.kind);

    if (!token.lexeme.empty()) {
        out << " `" << token.lexeme << '`';
    }

    return out.str();
}

} // namespace miko
