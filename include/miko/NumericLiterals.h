// NumericLiterals.h - one implementation of miko's numeric literal syntax.
//
// The lexer decides which characters belong to a literal; this turns the text
// into a value. Both stages were duplicated between the interpreter and the
// native backend, which is how `9223372036854775807` came to compile to -1 in
// one of them: the native side built every literal as an i32 first.
//
// Supported forms:
//   1234        decimal
//   0x1F 0X1F   hexadecimal
//   0b1010      binary
//   0o755       octal
//   1_000_000   digit separators, ignored
//   1.5 1e10 1.5e-3   floating point

#ifndef MIKO_NUMERIC_LITERALS_H
#define MIKO_NUMERIC_LITERALS_H

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace miko {

// Removes '_' digit separators so the standard converters can read the text.
inline std::string stripDigitSeparators(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (const char ch : text) {
        if (ch != '_') {
            result.push_back(ch);
        }
    }
    return result;
}

// True when the literal carries an explicit radix prefix. Such a literal is
// always an integer: `0x10` is 16, not a float.
inline bool hasRadixPrefix(const std::string& text) {
    if (text.size() < 2 || text[0] != '0') {
        return false;
    }
    const char marker = text[1];
    return marker == 'x' || marker == 'X' || marker == 'b' || marker == 'B' ||
           marker == 'o' || marker == 'O';
}

inline int radixOf(const std::string& text) {
    if (text.size() < 2 || text[0] != '0') {
        return 10;
    }
    const char marker = text[1];
    if (marker == 'x' || marker == 'X') {
        return 16;
    }
    if (marker == 'b' || marker == 'B') {
        return 2;
    }
    if (marker == 'o' || marker == 'O') {
        return 8;
    }
    return 10;
}

// Parses an integer literal. Throws instead of truncating, because a literal
// that silently changes value is the worst kind of compiler bug.
inline long long parseIntegerLiteral(const std::string& rawText) {
    const std::string text = stripDigitSeparators(rawText);
    const int radix = radixOf(text);
    // stoll in base 0 handles 0x/0b(no)/0o(no), so the prefix is stripped and
    // the base passed explicitly instead.
    const std::string digits = radix == 10 ? text : text.substr(2);
    if (digits.empty()) {
        throw std::runtime_error("malformed integer literal '" + rawText + "'");
    }
    try {
        std::size_t consumed = 0;
        const long long value = std::stoll(digits, &consumed, radix);
        if (consumed != digits.size()) {
            throw std::runtime_error("malformed integer literal '" + rawText + "'");
        }
        return value;
    } catch (const std::out_of_range&) {
        throw std::runtime_error("integer literal out of range for i64: " + rawText);
    }
}

inline double parseFloatLiteral(const std::string& rawText) {
    const std::string text = stripDigitSeparators(rawText);
    try {
        std::size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        if (consumed != text.size()) {
            throw std::runtime_error("malformed floating literal '" + rawText + "'");
        }
        return value;
    } catch (const std::out_of_range&) {
        throw std::runtime_error("floating literal out of range: " + rawText);
    }
}

} // namespace miko

#endif // MIKO_NUMERIC_LITERALS_H
