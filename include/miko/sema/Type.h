#pragma once

#include <string>
#include <memory>

namespace miko {

enum class TypeKind {
    Unknown,
    Void,
    Byte,
    Char,
    I32,
    I64,
    F32,
    F64,
    Bool,
    String,
    Struct,
    Enum,
    Vec
};

class Type {
public:
    Type() = default;
    explicit Type(TypeKind kind);
    Type(Type elementType, std::size_t arrayLength);
    Type(const Type& other);
    Type& operator=(const Type& other);
    Type(Type&& other) noexcept = default;
    Type& operator=(Type&& other) noexcept = default;

    TypeKind kind() const;
    bool isUnknown() const;
    bool isVoid() const;
    bool isNumeric() const;
    bool isInteger() const;
    bool isFloat() const;
    bool isArray() const;
    bool isVec() const;
    bool isEnum() const;
    const Type& elementType() const;
    std::size_t arrayLength() const;
    const std::string& structName() const;
    std::string name() const;

    static Type unknown();
    static Type voidType();
    static Type arrayOf(Type elementType, std::size_t arrayLength);
    static Type vecOf(Type elementType);
    static Type structOf(std::string name);
    static Type enumOf(std::string name);
    static Type fromName(const std::string& name);

private:
    TypeKind kind_ = TypeKind::Unknown;
    std::unique_ptr<Type> elementType_;
    std::size_t arrayLength_ = 0;
    std::string structName_;
};

bool canImplicitlyConvert(Type from, Type to);
Type commonType(Type left, Type right);
bool operator==(Type left, Type right);
bool operator!=(Type left, Type right);

} // namespace miko
