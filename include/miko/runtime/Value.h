#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace miko {

enum class ValueKind {
    Void,
    I64,
    F64,
    Bool,
    Char,
    String,
    Array,
    Vec,
    Struct
};

class Value {
public:
    Value();

    static Value voidValue();
    static Value integer(long long value);
    static Value floating(double value);
    static Value boolean(bool value);
    static Value character(char value);
    static Value string(std::string value);
    static Value array(std::vector<Value> values);
    static Value vec();
    static Value vec(std::vector<Value> values);
    static Value structure(std::string typeName, std::unordered_map<std::string, Value> fields);

    ValueKind kind() const;
    bool isVoid() const;
    bool isNumeric() const;
    bool isFloat() const;
    long long asInteger() const;
    double asFloat() const;
    bool asBool() const;
    const std::string& asString() const;
    char asChar() const;
    const std::vector<Value>& asArray() const;
    std::vector<Value>& asArray();
    const std::vector<Value>& asVec() const;
    std::vector<Value>& asVec();
    const std::string& structTypeName() const;
    const std::unordered_map<std::string, Value>& asStructFields() const;
    std::unordered_map<std::string, Value>& structFieldsMut();
    std::string toString() const;

private:
    struct StructValue {
        std::string typeName;
        std::unordered_map<std::string, Value> fields;
    };

    ValueKind kind_ = ValueKind::Void;
    std::variant<std::monostate, long long, double, bool, char, std::string, std::vector<Value>,
                 std::shared_ptr<std::vector<Value>>, StructValue> data_;
};

} // namespace miko
