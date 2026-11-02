// Minimal JSON value + parser + serializer. Accepts JSONC (// and /* */
// comments, trailing commas) so config files can carry notes for mod authors.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fl::json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : t_(Type::Bool), b_(b) {}
    Value(int n) : t_(Type::Number), n_(n) {}
    Value(int64_t n) : t_(Type::Number), n_(static_cast<double>(n)) {}
    Value(uint64_t n) : t_(Type::Number), n_(static_cast<double>(n)) {}
    Value(double n) : t_(Type::Number), n_(n) {}
    Value(const char* s) : t_(Type::String), s_(s) {}
    Value(std::string s) : t_(Type::String), s_(std::move(s)) {}

    static Value array() { Value v; v.t_ = Type::Array; return v; }
    static Value object() { Value v; v.t_ = Type::Object; return v; }

    Type type() const { return t_; }
    bool isNull() const { return t_ == Type::Null; }
    bool isBool() const { return t_ == Type::Bool; }
    bool isNumber() const { return t_ == Type::Number; }
    bool isString() const { return t_ == Type::String; }
    bool isArray() const { return t_ == Type::Array; }
    bool isObject() const { return t_ == Type::Object; }

    bool boolean(bool def = false) const { return t_ == Type::Bool ? b_ : def; }
    double number(double def = 0) const { return t_ == Type::Number ? n_ : def; }
    const std::string& str() const;

    // Arrays and objects share item storage; objects also keep keys.
    size_t size() const { return items_.size(); }
    const Value& at(size_t i) const { return items_[i]; }
    Value& at(size_t i) { return items_[i]; }
    const std::string& keyAt(size_t i) const { return keys_[i]; }
    const Value* find(std::string_view key) const;

    Value& set(const std::string& key, Value v);  // object: insert or replace
    Value& push(Value v);                          // array: append

private:
    Type t_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> items_;
    std::vector<std::string> keys_;
};

// Returns Null and fills `error` ("line 4, column 12: expected ','") on failure.
Value parse(std::string_view text, std::string* error);
std::string serialize(const Value& v, bool pretty = true);

}  // namespace fl::json
