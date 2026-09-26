#pragma once

#include "Common.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ld::json
{

class Value;
using Array  = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

/// Small self contained JSON value. Only what LightDock's config needs.
class Value
{
public:
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    Value() = default;
    Value(Type type) : type_(type) {}
    Value(bool value) : type_(Type::Bool), bool_(value) {}
    Value(int value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Value(double value) : type_(Type::Number), number_(value) {}
    Value(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Value(const char* value) : type_(Type::String), string_(value ? value : "") {}
    Value(Array value) : type_(Type::Array), array_(std::move(value)) {}
    Value(Object value) : type_(Type::Object), object_(std::move(value)) {}

    Type GetType() const noexcept { return type_; }

    bool IsNull() const noexcept   { return type_ == Type::Null; }
    bool IsBool() const noexcept   { return type_ == Type::Bool; }
    bool IsNumber() const noexcept { return type_ == Type::Number; }
    bool IsString() const noexcept { return type_ == Type::String; }
    bool IsArray() const noexcept  { return type_ == Type::Array; }
    bool IsObject() const noexcept { return type_ == Type::Object; }

    bool   AsBool(bool fallback) const noexcept;
    int    AsInt(int fallback) const noexcept;
    double AsDouble(double fallback) const noexcept;
    std::string AsString(std::string_view fallback = {}) const;

    const Array&  Items() const noexcept { return array_; }
    const Object& Fields() const noexcept { return object_; }

    /// Returns nullptr when the key is missing.
    const Value* Find(std::string_view key) const;

    Value& PushBack(Value value);
    Value& Set(std::string key, Value value);

    bool Has(std::string_view key) const { return Find(key) != nullptr; }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

struct ParseResult
{
    Value value;
    bool ok = false;
    std::string error;
};

ParseResult Parse(std::string_view text);

std::string Serialize(const Value& value, bool pretty = true);

} // namespace ld::json
