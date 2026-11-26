// StringLiterals.h - one implementation of miko's escape sequences.
//
// The interpreter and the native backend each carried their own copy of this
// decoding, and both silently dropped the backslash for anything they did not
// recognise: "A\x42C" reached the running program as the five characters
// Ax42C, and the two copies could drift apart without any test noticing.
// Everything that turns literal text into bytes now goes through here.

#ifndef MIKO_STRING_LITERALS_H
#define MIKO_STRING_LITERALS_H

#include <stdexcept>
#include <string>

namespace miko {

namespace detail {

inline int hexDigitValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

} // namespace detail

// Decodes the body of a string literal. `text` may include the surrounding
// double quotes, which are stripped when present.
inline std::string decodeStringLiteral(const std::string& text) {
    std::string result;
    const std::size_t start = text.size() >= 2 && text.front() == '"' ? 1 : 0;
    const std::size_t end = text.size() >= 2 && text.back() == '"' ? text.size() - 1 : text.size();

    for (std::size_t i = start; i < end; ++i) {
        if (text[i] != '\\' || i + 1 >= end) {
            result.push_back(text[i]);
            continue;
        }

        const char escaped = text[++i];
        switch (escaped) {
        case 'n':
            result.push_back('\n');
            break;
        case 't':
            result.push_back('\t');
            break;
        case 'r':
            result.push_back('\r');
            break;
        case '0':
            result.push_back('\0');
            break;
        case 'a':
            result.push_back('\a');
            break;
        case 'b':
            result.push_back('\b');
            break;
        case 'f':
            result.push_back('\f');
            break;
        case 'v':
            result.push_back('\v');
            break;
        case '\'':
            result.push_back('\'');
            break;
        case '"':
            result.push_back('"');
            break;
        case '\\':
            result.push_back('\\');
            break;
        case 'x': {
            // \xNN - exactly two hexadecimal digits, so a following character
            // can never be mistaken for part of the escape.
            if (i + 2 >= end) {
                throw std::runtime_error("\\x escape needs two hex digits in " + text);
            }
            const int high = detail::hexDigitValue(text[i + 1]);
            const int low = detail::hexDigitValue(text[i + 2]);
            if (high < 0 || low < 0) {
                throw std::runtime_error("\\x escape needs two hex digits in " + text);
            }
            result.push_back(static_cast<char>((high << 4) | low));
            i += 2;
            break;
        }
        default:
            // Reporting this is the point: a silently dropped backslash is a
            // corrupted string that no later stage can detect.
            throw std::runtime_error(std::string("unknown escape sequence '\\") + escaped +
                                     "' in " + text);
        }
    }

    return result;
}

// Decodes a char literal such as 'a', '\n' or '\x41'.
inline char decodeCharLiteral(const std::string& text) {
    if (text.size() < 3 || text.front() != '\'' || text.back() != '\'') {
        throw std::runtime_error("invalid char literal " + text);
    }

    if (text[1] != '\\') {
        return text[1];
    }

    if (text.size() < 4) {
        throw std::runtime_error("invalid char escape in " + text);
    }

    switch (text[2]) {
    case 'n':
        return '\n';
    case 't':
        return '\t';
    case 'r':
        return '\r';
    case '0':
        return '\0';
    case 'a':
        return '\a';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    case 'v':
        return '\v';
    case '\'':
        return '\'';
    case '"':
        return '"';
    case '\\':
        return '\\';
    case 'x': {
        if (text.size() < 6) {
            throw std::runtime_error("\\x escape needs two hex digits in " + text);
        }
        const int high = detail::hexDigitValue(text[3]);
        const int low = detail::hexDigitValue(text[4]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("\\x escape needs two hex digits in " + text);
        }
        return static_cast<char>((high << 4) | low);
    }
    default:
        throw std::runtime_error(std::string("unknown escape sequence '\\") + text[2] +
                                 "' in " + text);
    }
}

} // namespace miko

#endif // MIKO_STRING_LITERALS_H
