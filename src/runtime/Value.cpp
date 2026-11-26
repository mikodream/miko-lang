#include "miko/runtime/Value.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace miko {

Value::Value() : data_(std::monostate{}) {
}

Value Value::voidValue() {
    return Value();
}

Value Value::integer(long long value) {
    Value result;
    result.kind_ = ValueKind::I64;
    result.data_ = value;
    return result;
}

Value Value::floating(double value) {
    Value result;
    result.kind_ = ValueKind::F64;
    result.data_ = value;
    return result;
}

Value Value::boolean(bool value) {
    Value result;
    result.kind_ = ValueKind::Bool;
    result.data_ = value;
    return result;
}

Value Value::character(char value) {
    Value result;
    result.kind_ = ValueKind::Char;
    result.data_ = value;
    return result;
}

Value Value::string(std::string value) {
    Value result;
    result.kind_ = ValueKind::String;
    result.data_ = std::move(value);
    return result;
}

Value Value::array(std::vector<Value> values) {
    Value result;
    result.kind_ = ValueKind::Array;
    result.data_ = std::move(values);
    return result;
}

Value Value::vec() {
    Value result;
    result.kind_ = ValueKind::Vec;
    result.data_ = std::make_shared<std::vector<Value>>();
    return result;
}

Value Value::vec(std::vector<Value> values) {
    Value result;
    result.kind_ = ValueKind::Vec;
    result.data_ = std::make_shared<std::vector<Value>>(std::move(values));
    return result;
}

Value Value::structure(std::string typeName, std::unordered_map<std::string, Value> fields) {
    Value result;
    result.kind_ = ValueKind::Struct;
    result.data_ = StructValue{std::move(typeName), std::move(fields)};
    return result;
}

ValueKind Value::kind() const {
    return kind_;
}

bool Value::isVoid() const {
    return kind_ == ValueKind::Void;
}

bool Value::isNumeric() const {
    return kind_ == ValueKind::I64 || kind_ == ValueKind::F64 || kind_ == ValueKind::Char;
}

bool Value::isFloat() const {
    return kind_ == ValueKind::F64;
}

long long Value::asInteger() const {
    if (kind_ == ValueKind::I64) {
        return std::get<long long>(data_);
    }
    if (kind_ == ValueKind::Bool) {
        return std::get<bool>(data_) ? 1 : 0;
    }
    if (kind_ == ValueKind::Char) {
        return static_cast<unsigned char>(std::get<char>(data_));
    }
    if (kind_ == ValueKind::F64) {
        return static_cast<long long>(std::get<double>(data_));
    }
    throw std::runtime_error("value is not integer-compatible");
}

double Value::asFloat() const {
    if (kind_ == ValueKind::F64) {
        return std::get<double>(data_);
    }
    if (kind_ == ValueKind::I64) {
        return static_cast<double>(std::get<long long>(data_));
    }
    if (kind_ == ValueKind::Bool) {
        return std::get<bool>(data_) ? 1.0 : 0.0;
    }
    if (kind_ == ValueKind::Char) {
        return static_cast<unsigned char>(std::get<char>(data_));
    }
    throw std::runtime_error("value is not float-compatible");
}

bool Value::asBool() const {
    if (kind_ == ValueKind::Bool) {
        return std::get<bool>(data_);
    }
    if (kind_ == ValueKind::I64) {
        return std::get<long long>(data_) != 0;
    }
    if (kind_ == ValueKind::F64) {
        return std::get<double>(data_) != 0.0;
    }
    if (kind_ == ValueKind::String) {
        return !std::get<std::string>(data_).empty();
    }
    if (kind_ == ValueKind::Char) {
        return std::get<char>(data_) != '\0';
    }
    if (kind_ == ValueKind::Array) {
        return !std::get<std::vector<Value>>(data_).empty();
    }
    if (kind_ == ValueKind::Vec) {
        return !asVec().empty();
    }
    if (kind_ == ValueKind::Struct) {
        return true;
    }
    return false;
}

const std::string& Value::asString() const {
    if (kind_ != ValueKind::String) {
        throw std::runtime_error("value is not a string");
    }
    return std::get<std::string>(data_);
}

char Value::asChar() const {
    if (kind_ == ValueKind::Char) {
        return std::get<char>(data_);
    }
    if (kind_ == ValueKind::I64) {
        return static_cast<char>(std::get<long long>(data_));
    }
    throw std::runtime_error("value is not char-compatible");
}

const std::vector<Value>& Value::asArray() const {
    if (kind_ != ValueKind::Array) {
        throw std::runtime_error("value is not an array");
    }
    return std::get<std::vector<Value>>(data_);
}

std::vector<Value>& Value::asArray() {
    if (kind_ != ValueKind::Array) {
        throw std::runtime_error("value is not an array");
    }
    return std::get<std::vector<Value>>(data_);
}

const std::vector<Value>& Value::asVec() const {
    if (kind_ != ValueKind::Vec) {
        throw std::runtime_error("value is not a vec");
    }
    return *std::get<std::shared_ptr<std::vector<Value>>>(data_);
}

std::vector<Value>& Value::asVec() {
    if (kind_ != ValueKind::Vec) {
        throw std::runtime_error("value is not a vec");
    }
    return *std::get<std::shared_ptr<std::vector<Value>>>(data_);
}

const std::string& Value::structTypeName() const {
    if (kind_ != ValueKind::Struct) {
        throw std::runtime_error("value is not a struct");
    }
    return std::get<StructValue>(data_).typeName;
}

const std::unordered_map<std::string, Value>& Value::asStructFields() const {
    if (kind_ != ValueKind::Struct) {
        throw std::runtime_error("value is not a struct");
    }
    return std::get<StructValue>(data_).fields;
}

std::unordered_map<std::string, Value>& Value::structFieldsMut() {
    if (kind_ != ValueKind::Struct) {
        throw std::runtime_error("value is not a struct");
    }
    return std::get<StructValue>(data_).fields;
}

std::string Value::toString() const {
    switch (kind_) {
    case ValueKind::Void:
        return "void";
    case ValueKind::I64:
        return std::to_string(std::get<long long>(data_));
    case ValueKind::F64: {
        std::ostringstream out;
        out << std::setprecision(12) << std::get<double>(data_);
        return out.str();
    }
    case ValueKind::Bool:
        return std::get<bool>(data_) ? "true" : "false";
    case ValueKind::Char:
        return std::string(1, std::get<char>(data_));
    case ValueKind::String:
        return std::get<std::string>(data_);
    case ValueKind::Array: {
        const auto& values = std::get<std::vector<Value>>(data_);
        std::ostringstream out;
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << values[i].toString();
        }
        out << ']';
        return out.str();
    }
    case ValueKind::Vec: {
        const auto& values = *std::get<std::shared_ptr<std::vector<Value>>>(data_);
        std::ostringstream out;
        out << "vec[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << values[i].toString();
        }
        out << ']';
        return out.str();
    }
    case ValueKind::Struct: {
        const auto& value = std::get<StructValue>(data_);
        std::ostringstream out;
        out << value.typeName << " {";
        bool first = true;
        for (const auto& field : value.fields) {
            if (!first) {
                out << ", ";
            }
            first = false;
            out << field.first << ": " << field.second.toString();
        }
        out << '}';
        return out.str();
    }
    }

    return "void";
}

} // namespace miko
