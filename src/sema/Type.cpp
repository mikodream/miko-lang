#include "miko/sema/Type.h"

#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace miko {

namespace {

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

} // namespace

Type::Type(TypeKind kind) : kind_(kind) {
}

Type::Type(Type elementType, std::size_t arrayLength)
    : kind_(TypeKind::Unknown),
      elementType_(std::make_unique<Type>(std::move(elementType))),
      arrayLength_(arrayLength) {
}

Type::Type(const Type& other)
    : kind_(other.kind_),
      elementType_(other.elementType_ ? std::make_unique<Type>(*other.elementType_) : nullptr),
      arrayLength_(other.arrayLength_),
      structName_(other.structName_) {
}

Type& Type::operator=(const Type& other) {
    if (this == &other) {
        return *this;
    }

    kind_ = other.kind_;
    elementType_ = other.elementType_ ? std::make_unique<Type>(*other.elementType_) : nullptr;
    arrayLength_ = other.arrayLength_;
    structName_ = other.structName_;
    return *this;
}

TypeKind Type::kind() const {
    return kind_;
}

bool Type::isUnknown() const {
    return kind_ == TypeKind::Unknown && !isArray() && !isVec();
}

bool Type::isVoid() const {
    return kind_ == TypeKind::Void;
}

bool Type::isNumeric() const {
    return isInteger() || isFloat();
}

bool Type::isInteger() const {
    return kind_ == TypeKind::Byte || kind_ == TypeKind::Char || kind_ == TypeKind::I32 || kind_ == TypeKind::I64;
}

bool Type::isFloat() const {
    return kind_ == TypeKind::F32 || kind_ == TypeKind::F64;
}

bool Type::isArray() const {
    return elementType_ != nullptr && kind_ != TypeKind::Vec;
}

bool Type::isVec() const {
    return kind_ == TypeKind::Vec && elementType_ != nullptr;
}

bool Type::isEnum() const {
    return kind_ == TypeKind::Enum;
}

const Type& Type::elementType() const {
    if (!elementType_) {
        throw std::runtime_error("type is not an array");
    }
    return *elementType_;
}

std::size_t Type::arrayLength() const {
    return arrayLength_;
}

const std::string& Type::structName() const {
    return structName_;
}

std::string Type::name() const {
    if (isVec()) {
        return "vec<" + elementType_->name() + ">";
    }

    if (isArray()) {
        return "[" + elementType_->name() + "; " + std::to_string(arrayLength_) + "]";
    }

    if (kind_ == TypeKind::Struct || kind_ == TypeKind::Enum) {
        return structName_;
    }

    switch (kind_) {
    case TypeKind::Unknown:
        return "unknown";
    case TypeKind::Void:
        return "void";
    case TypeKind::Byte:
        return "byte";
    case TypeKind::Char:
        return "char";
    case TypeKind::I32:
        return "i32";
    case TypeKind::I64:
        return "i64";
    case TypeKind::F32:
        return "f32";
    case TypeKind::F64:
        return "f64";
    case TypeKind::Bool:
        return "bool";
    case TypeKind::String:
        return "string";
    case TypeKind::Struct:
    case TypeKind::Enum:
        return structName_;
    case TypeKind::Vec:
        return elementType_ ? "vec<" + elementType_->name() + ">" : "vec<?>";
    }

    return "unknown";
}

Type Type::unknown() {
    return Type(TypeKind::Unknown);
}

Type Type::voidType() {
    return Type(TypeKind::Void);
}

Type Type::arrayOf(Type elementType, std::size_t arrayLength) {
    return Type(std::move(elementType), arrayLength);
}

Type Type::vecOf(Type elementType) {
    Type result(TypeKind::Vec);
    result.elementType_ = std::make_unique<Type>(std::move(elementType));
    return result;
}

Type Type::structOf(std::string name) {
    Type result(TypeKind::Struct);
    result.structName_ = std::move(name);
    return result;
}

Type Type::enumOf(std::string name) {
    Type result(TypeKind::Enum);
    result.structName_ = std::move(name);
    return result;
}

Type Type::fromName(const std::string& name) {
    if (name.size() >= 5 && name.compare(0, 4, "vec<") == 0 && name.back() == '>') {
        Type elementType = Type::fromName(name.substr(4, name.size() - 5));
        if (elementType.isUnknown()) {
            return Type::unknown();
        }
        return Type::vecOf(std::move(elementType));
    }

    if (name.size() >= 5 && name.front() == '[' && name.back() == ']') {
        const std::string_view inner(name.data() + 1, name.size() - 2);
        const std::size_t separator = inner.rfind(';');
        if (separator == std::string_view::npos) {
            return Type::unknown();
        }

        Type elementType = Type::fromName(trim(inner.substr(0, separator)));
        if (elementType.isUnknown()) {
            return Type::unknown();
        }

        try {
            const std::size_t length = static_cast<std::size_t>(std::stoull(trim(inner.substr(separator + 1))));
            return Type::arrayOf(std::move(elementType), length);
        } catch (...) {
            return Type::unknown();
        }
    }

    if (name == "void") {
        return Type(TypeKind::Void);
    }
    if (name == "byte" || name == "u8") {
        return Type(TypeKind::Byte);
    }
    if (name == "char") {
        return Type(TypeKind::Char);
    }
    if (name == "i32" || name == "int") {
        return Type(TypeKind::I32);
    }
    if (name == "i64" || name == "long") {
        return Type(TypeKind::I64);
    }
    if (name == "f32" || name == "float") {
        return Type(TypeKind::F32);
    }
    if (name == "f64" || name == "double") {
        return Type(TypeKind::F64);
    }
    if (name == "bool") {
        return Type(TypeKind::Bool);
    }
    if (name == "string") {
        return Type(TypeKind::String);
    }

    return Type::structOf(name);
}

bool canImplicitlyConvert(Type from, Type to) {
    if (from == to) {
        return true;
    }

    // An unknown type means type inference already failed (or was deferred,
    // e.g. `vec_new()`); don't pile a second error on top of it.
    if (from.isUnknown() || to.isUnknown()) {
        return true;
    }

    if (from.isVec() || to.isVec()) {
        return from.isVec() && to.isVec() && canImplicitlyConvert(from.elementType(), to.elementType());
    }

    if (from.isArray() || to.isArray()) {
        return from.isArray() && to.isArray()
            && from.arrayLength() == to.arrayLength()
            && canImplicitlyConvert(from.elementType(), to.elementType());
    }

    if (from.kind() == TypeKind::Struct || to.kind() == TypeKind::Struct) {
        return from.kind() == TypeKind::Struct && to.kind() == TypeKind::Struct && from.structName() == to.structName();
    }

    if (from.isEnum() || to.isEnum()) {
        return from.isEnum() && to.isEnum() && from.structName() == to.structName();
    }

    if (from.kind() == TypeKind::Bool && (to.kind() == TypeKind::Byte || to.kind() == TypeKind::Char || to.kind() == TypeKind::I32)) {
        return true;
    }

    if ((from.kind() == TypeKind::Byte || from.kind() == TypeKind::Char) && (to.kind() == TypeKind::I32 || to.kind() == TypeKind::I64 || to.kind() == TypeKind::F32 || to.kind() == TypeKind::F64)) {
        return true;
    }

    if (from.kind() == TypeKind::I32 && (to.kind() == TypeKind::I64 || to.kind() == TypeKind::F32 || to.kind() == TypeKind::F64)) {
        return true;
    }

    if (from.kind() == TypeKind::I64 && to.kind() == TypeKind::F64) {
        return true;
    }

    if (from.kind() == TypeKind::F32 && to.kind() == TypeKind::F64) {
        return true;
    }

    if (from.isNumeric() && to.kind() == TypeKind::Bool) {
        return true;
    }

    return false;
}

Type commonType(Type left, Type right) {
    if (left == right) {
        return left;
    }

    if (left.isUnknown()) {
        return right;
    }

    if (right.isUnknown()) {
        return left;
    }

    if (left.isArray() || right.isArray() || left.isVec() || right.isVec()
        || left.kind() == TypeKind::Struct || right.kind() == TypeKind::Struct
        || left.isEnum() || right.isEnum()
        || !left.isNumeric() || !right.isNumeric()) {
        return Type::unknown();
    }

    if (left.kind() == TypeKind::F64 || right.kind() == TypeKind::F64) {
        return Type(TypeKind::F64);
    }

    if (left.kind() == TypeKind::F32 || right.kind() == TypeKind::F32) {
        return Type(TypeKind::F32);
    }

    if (left.kind() == TypeKind::I64 || right.kind() == TypeKind::I64) {
        return Type(TypeKind::I64);
    }

    if (left.kind() == TypeKind::I32 || right.kind() == TypeKind::I32) {
        return Type(TypeKind::I32);
    }

    return Type(TypeKind::I32);
}

bool operator==(Type left, Type right) {
    if (left.isVec() || right.isVec()) {
        return left.isVec() && right.isVec() && left.elementType() == right.elementType();
    }

    if (left.isArray() || right.isArray()) {
        return left.isArray() && right.isArray()
            && left.arrayLength() == right.arrayLength()
            && left.elementType() == right.elementType();
    }

    if (left.kind() == TypeKind::Struct || right.kind() == TypeKind::Struct
        || left.isEnum() || right.isEnum()) {
        const bool bothStruct = left.kind() == TypeKind::Struct && right.kind() == TypeKind::Struct;
        const bool bothEnum = left.isEnum() && right.isEnum();
        return (bothStruct || bothEnum) && left.structName() == right.structName();
    }

    return left.kind() == right.kind();
}

bool operator!=(Type left, Type right) {
    return !(left == right);
}

} // namespace miko
