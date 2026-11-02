#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fl::json {

const std::string& Value::str() const {
    static const std::string empty;
    return t_ == Type::String ? s_ : empty;
}

const Value* Value::find(std::string_view key) const {
    if (t_ != Type::Object) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return &items_[i];
    return nullptr;
}

Value& Value::set(const std::string& key, Value v) {
    if (t_ != Type::Object) *this = object();
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            items_[i] = std::move(v);
            return items_[i];
        }
    }
    keys_.push_back(key);
    items_.push_back(std::move(v));
    return items_.back();
}

Value& Value::push(Value v) {
    if (t_ != Type::Array) *this = array();
    items_.push_back(std::move(v));
    return items_.back();
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool run(Value& out) {
        skipWs();
        if (!value(out, 0)) return false;
        skipWs();
        if (i_ != s_.size()) return fail("unexpected text after the end of the document");
        return true;
    }
    const std::string& error() const { return err_; }

private:
    std::string_view s_;
    size_t i_ = 0;
    std::string err_;

    bool fail(const char* msg) {
        if (err_.empty()) {
            size_t line = 1, col = 1;
            for (size_t k = 0; k < i_ && k < s_.size(); ++k) {
                if (s_[k] == '\n') {
                    ++line;
                    col = 1;
                } else {
                    ++col;
                }
            }
            err_ = "line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + msg;
        }
        return false;
    }

    void skipWs() {
        for (;;) {
            while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_;
            if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '/') {
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                continue;
            }
            if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '*') {
                size_t end = s_.find("*/", i_ + 2);
                i_ = end == std::string_view::npos ? s_.size() : end + 2;
                continue;
            }
            // UTF-8 BOM
            if (i_ == 0 && s_.size() >= 3 && (unsigned char)s_[0] == 0xEF && (unsigned char)s_[1] == 0xBB &&
                (unsigned char)s_[2] == 0xBF) {
                i_ = 3;
                continue;
            }
            break;
        }
    }

    bool literal(const char* word) {
        size_t n = std::char_traits<char>::length(word);
        if (s_.substr(i_, n) != word) return fail("invalid literal");
        i_ += n;
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& out) {
        if (i_ + 4 > s_.size()) return fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        ++i_;  // opening quote
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\n') return fail("line break inside a string (use \\n)");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) break;
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    --i_;
                    return fail("invalid escape in string (write backslashes as \\\\)");
            }
        }
        return fail("unterminated string");
    }

    bool number(Value& out) {
        size_t start = i_;
        if (s_[i_] == '-') ++i_;
        while (i_ < s_.size() && (isdigit((unsigned char)s_[i_]) || s_[i_] == '.' || s_[i_] == 'e' ||
                                  s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
            ++i_;
        std::string tmp(s_.substr(start, i_ - start));
        char* end = nullptr;
        double d = strtod(tmp.c_str(), &end);
        if (!end || *end != '\0' || tmp.empty()) {
            i_ = start;
            return fail("invalid number");
        }
        out = Value(d);
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > 64) return fail("document is nested too deeply");
        if (i_ >= s_.size()) return fail("unexpected end of document");
        char c = s_[i_];
        if (c == '{') {
            ++i_;
            out = Value::object();
            skipWs();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            for (;;) {
                skipWs();
                if (i_ < s_.size() && s_[i_] == '}') {  // trailing comma
                    ++i_;
                    return true;
                }
                if (i_ >= s_.size() || s_[i_] != '"') return fail("expected a quoted key");
                std::string key;
                if (!string(key)) return false;
                skipWs();
                if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':' after key");
                ++i_;
                skipWs();
                Value v;
                if (!value(v, depth + 1)) return false;
                out.set(key, std::move(v));
                skipWs();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') {
                    ++i_;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i_;
            out = Value::array();
            for (;;) {
                skipWs();
                if (i_ < s_.size() && s_[i_] == ']') {
                    ++i_;
                    return true;
                }
                Value v;
                if (!value(v, depth + 1)) return false;
                out.push(std::move(v));
                skipWs();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') {
                    ++i_;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!string(s)) return false;
            out = Value(std::move(s));
            return true;
        }
        if (c == 't') {
            out = Value(true);
            return literal("true");
        }
        if (c == 'f') {
            out = Value(false);
            return literal("false");
        }
        if (c == 'n') {
            out = Value();
            return literal("null");
        }
        if (c == '-' || isdigit((unsigned char)c)) return number(out);
        return fail("unexpected character");
    }
};

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void write(std::string& out, const Value& v, bool pretty, int indent) {
    auto newline = [&](int level) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<size_t>(level) * 2, ' ');
    };
    switch (v.type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.boolean() ? "true" : "false"; break;
        case Type::Number: {
            double d = v.number();
            char buf[40];
            if (std::floor(d) == d && std::fabs(d) < 1e15) snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d));
            else snprintf(buf, sizeof buf, "%.17g", d);
            out += buf;
            break;
        }
        case Type::String: writeString(out, v.str()); break;
        case Type::Array:
            if (v.size() == 0) {
                out += "[]";
                break;
            }
            out += '[';
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) out += ',';
                newline(indent + 1);
                write(out, v.at(i), pretty, indent + 1);
            }
            newline(indent);
            out += ']';
            break;
        case Type::Object:
            if (v.size() == 0) {
                out += "{}";
                break;
            }
            out += '{';
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) out += ',';
                newline(indent + 1);
                writeString(out, v.keyAt(i));
                out += pretty ? ": " : ":";
                write(out, v.at(i), pretty, indent + 1);
            }
            newline(indent);
            out += '}';
            break;
    }
}

}  // namespace

Value parse(std::string_view text, std::string* error) {
    Parser p(text);
    Value v;
    if (!p.run(v)) {
        if (error) *error = p.error();
        return Value();
    }
    if (error) error->clear();
    return v;
}

std::string serialize(const Value& v, bool pretty) {
    std::string out;
    write(out, v, pretty, 0);
    if (pretty) out += '\n';
    return out;
}

}  // namespace fl::json
