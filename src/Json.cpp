#include "Json.h"

#include <cstdio>
#include <cstdlib>

namespace ld::json
{
namespace
{

class Parser
{
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ParseResult ParseDocument()
    {
        SkipWhitespace();

        Value value;
        if (!ParseValue(value))
        {
            return {Value(), false, error_};
        }

        SkipWhitespace();
        if (pos_ != text_.size())
        {
            Fail("unexpected trailing content");
            return {Value(), false, error_};
        }

        return {value, true, {}};
    }

private:
    bool ParseValue(Value& out);
    bool ParseObject(Value& out);
    bool ParseArray(Value& out);
    bool ParseString(std::string& out);
    bool ParseNumber(double& out);
    bool ParseLiteral(Value& out);

    void SkipWhitespace()
    {
        while (pos_ < text_.size())
        {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                ++pos_;
            }
            else
            {
                break;
            }
        }
    }

    char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    bool Consume(char expected)
    {
        if (Peek() != expected)
        {
            Fail("unexpected character");
            return false;
        }

        ++pos_;
        return true;
    }

    void Fail(const char* message)
    {
        if (error_.empty())
        {
            char buffer[96];
            snprintf(buffer, sizeof(buffer), "%s at offset %zu", message, pos_);
            error_ = buffer;
        }
    }

    static void AppendUtf8(std::string& out, uint32_t codePoint)
    {
        if (codePoint < 0x80)
        {
            out.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    std::string_view text_;
    size_t pos_ = 0;
    std::string error_;
};

bool Parser::ParseValue(Value& out)
{
    SkipWhitespace();

    switch (Peek())
    {
    case '{':
        return ParseObject(out);
    case '[':
        return ParseArray(out);
    case '"':
    {
        std::string text;
        if (!ParseString(text))
        {
            return false;
        }
        out = Value(std::move(text));
        return true;
    }
    case 't':
    case 'f':
    case 'n':
        return ParseLiteral(out);
    default:
    {
        double number = 0.0;
        if (!ParseNumber(number))
        {
            return false;
        }
        out = Value(number);
        return true;
    }
    }
}

bool Parser::ParseObject(Value& out)
{
    if (!Consume('{'))
    {
        return false;
    }

    Object fields;
    SkipWhitespace();

    if (Peek() == '}')
    {
        ++pos_;
        out = Value(std::move(fields));
        return true;
    }

    for (;;)
    {
        SkipWhitespace();

        std::string key;
        if (!ParseString(key))
        {
            return false;
        }

        SkipWhitespace();
        if (!Consume(':'))
        {
            return false;
        }

        Value value;
        if (!ParseValue(value))
        {
            return false;
        }

        fields.emplace_back(std::move(key), std::move(value));

        SkipWhitespace();
        if (Peek() == ',')
        {
            ++pos_;
            continue;
        }

        if (!Consume('}'))
        {
            return false;
        }

        break;
    }

    out = Value(std::move(fields));
    return true;
}

bool Parser::ParseArray(Value& out)
{
    if (!Consume('['))
    {
        return false;
    }

    Array items;
    SkipWhitespace();

    if (Peek() == ']')
    {
        ++pos_;
        out = Value(std::move(items));
        return true;
    }

    for (;;)
    {
        Value value;
        if (!ParseValue(value))
        {
            return false;
        }

        items.push_back(std::move(value));

        SkipWhitespace();
        if (Peek() == ',')
        {
            ++pos_;
            continue;
        }

        if (!Consume(']'))
        {
            return false;
        }

        break;
    }

    out = Value(std::move(items));
    return true;
}

bool Parser::ParseString(std::string& out)
{
    if (!Consume('"'))
    {
        return false;
    }

    std::string result;

    for (;;)
    {
        if (pos_ >= text_.size())
        {
            Fail("unterminated string");
            return false;
        }

        const char c = text_[pos_++];

        if (c == '"')
        {
            break;
        }

        if (c != '\\')
        {
            result.push_back(c);
            continue;
        }

        if (pos_ >= text_.size())
        {
            Fail("unterminated escape");
            return false;
        }

        const char escape = text_[pos_++];
        switch (escape)
        {
        case '"':  result.push_back('"');  break;
        case '\\': result.push_back('\\'); break;
        case '/':  result.push_back('/');  break;
        case 'b':  result.push_back('\b'); break;
        case 'f':  result.push_back('\f'); break;
        case 'n':  result.push_back('\n'); break;
        case 'r':  result.push_back('\r'); break;
        case 't':  result.push_back('\t'); break;
        case 'u':
        {
            if (pos_ + 4 > text_.size())
            {
                Fail("truncated \\u escape");
                return false;
            }

            uint32_t codePoint = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char digit = text_[pos_ + static_cast<size_t>(i)];
                uint32_t value = 0;
                if (digit >= '0' && digit <= '9')
                {
                    value = static_cast<uint32_t>(digit - '0');
                }
                else if (digit >= 'a' && digit <= 'f')
                {
                    value = static_cast<uint32_t>(digit - 'a' + 10);
                }
                else if (digit >= 'A' && digit <= 'F')
                {
                    value = static_cast<uint32_t>(digit - 'A' + 10);
                }
                else
                {
                    Fail("invalid \\u escape");
                    return false;
                }

                codePoint = (codePoint << 4) | value;
            }

            pos_ += 4;

            // Combine a surrogate pair when present.
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF
                && pos_ + 6 <= text_.size()
                && text_[pos_] == '\\' && text_[pos_ + 1] == 'u')
            {
                uint32_t low = 0;
                bool valid = true;
                for (int i = 0; i < 4; ++i)
                {
                    const char digit = text_[pos_ + 2 + static_cast<size_t>(i)];
                    uint32_t value = 0;
                    if (digit >= '0' && digit <= '9')
                    {
                        value = static_cast<uint32_t>(digit - '0');
                    }
                    else if (digit >= 'a' && digit <= 'f')
                    {
                        value = static_cast<uint32_t>(digit - 'a' + 10);
                    }
                    else if (digit >= 'A' && digit <= 'F')
                    {
                        value = static_cast<uint32_t>(digit - 'A' + 10);
                    }
                    else
                    {
                        valid = false;
                        break;
                    }

                    low = (low << 4) | value;
                }

                if (valid && low >= 0xDC00 && low <= 0xDFFF)
                {
                    pos_ += 6;
                    codePoint = 0x10000
                        + ((codePoint - 0xD800) << 10)
                        + (low - 0xDC00);
                }
            }

            AppendUtf8(result, codePoint);
            break;
        }
        default:
            Fail("unknown escape");
            return false;
        }
    }

    out = std::move(result);
    return true;
}

bool Parser::ParseNumber(double& out)
{
    const size_t start = pos_;

    if (Peek() == '-' || Peek() == '+')
    {
        ++pos_;
    }

    bool anyDigit = false;
    while (pos_ < text_.size())
    {
        const char c = text_[pos_];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E'
            || c == '-' || c == '+')
        {
            if (c >= '0' && c <= '9')
            {
                anyDigit = true;
            }
            ++pos_;
            continue;
        }

        break;
    }

    if (!anyDigit)
    {
        Fail("invalid number");
        return false;
    }

    const std::string raw(text_.substr(start, pos_ - start));
    out = strtod(raw.c_str(), nullptr);
    return true;
}

bool Parser::ParseLiteral(Value& out)
{
    if (text_.compare(pos_, 4, "true") == 0)
    {
        pos_ += 4;
        out = Value(true);
        return true;
    }

    if (text_.compare(pos_, 5, "false") == 0)
    {
        pos_ += 5;
        out = Value(false);
        return true;
    }

    if (text_.compare(pos_, 4, "null") == 0)
    {
        pos_ += 4;
        out = Value(Value::Type::Null);
        return true;
    }

    Fail("invalid literal");
    return false;
}

void EscapeInto(std::string& out, std::string_view text)
{
    for (char c : text)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buffer[8];
                snprintf(buffer, sizeof(buffer), "\\u%04x",
                         static_cast<unsigned int>(static_cast<unsigned char>(c)));
                out += buffer;
            }
            else
            {
                out.push_back(c);
            }
        }
    }
}

std::string FormatNumber(double value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.6g", value);
    return std::string(buffer);
}

void SerializeInto(const Value& value, std::string& out, bool pretty, int depth);

void SerializeArray(const Value& value, std::string& out, bool pretty, int depth)
{
    out += '[';

    const Array& items = value.Items();
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }

        if (pretty)
        {
            out += '\n';
            out.append(static_cast<size_t>(depth + 1) * 2, ' ');
        }

        SerializeInto(items[i], out, pretty, depth + 1);
    }

    if (pretty && !items.empty())
    {
        out += '\n';
        out.append(static_cast<size_t>(depth) * 2, ' ');
    }

    out += ']';
}

void SerializeObject(const Value& value, std::string& out, bool pretty, int depth)
{
    out += '{';

    const Object& fields = value.Fields();
    for (size_t i = 0; i < fields.size(); ++i)
    {
        if (i != 0)
        {
            out += ',';
        }

        if (pretty)
        {
            out += '\n';
            out.append(static_cast<size_t>(depth + 1) * 2, ' ');
        }

        out += '"';
        EscapeInto(out, fields[i].first);
        out += "\":";
        if (pretty)
        {
            out += ' ';
        }

        SerializeInto(fields[i].second, out, pretty, depth + 1);
    }

    if (pretty && !fields.empty())
    {
        out += '\n';
        out.append(static_cast<size_t>(depth) * 2, ' ');
    }

    out += '}';
}

void SerializeInto(const Value& value, std::string& out, bool pretty, int depth)
{
    switch (value.GetType())
    {
    case Value::Type::Null:
        out += "null";
        break;
    case Value::Type::Bool:
        out += value.AsBool(false) ? "true" : "false";
        break;
    case Value::Type::Number:
        out += FormatNumber(value.AsDouble(0.0));
        break;
    case Value::Type::String:
        out += '"';
        EscapeInto(out, value.AsString());
        out += '"';
        break;
    case Value::Type::Array:
        SerializeArray(value, out, pretty, depth);
        break;
    case Value::Type::Object:
        SerializeObject(value, out, pretty, depth);
        break;
    }
}

} // namespace

bool Value::AsBool(bool fallback) const noexcept
{
    if (type_ == Type::Bool)
    {
        return bool_;
    }

    if (type_ == Type::Number)
    {
        return number_ != 0.0;
    }

    return fallback;
}

int Value::AsInt(int fallback) const noexcept
{
    if (type_ == Type::Number)
    {
        return static_cast<int>(number_);
    }

    return fallback;
}

double Value::AsDouble(double fallback) const noexcept
{
    if (type_ == Type::Number)
    {
        return number_;
    }

    if (type_ == Type::Bool)
    {
        return bool_ ? 1.0 : 0.0;
    }

    return fallback;
}

std::string Value::AsString(std::string_view fallback) const
{
    if (type_ == Type::String)
    {
        return string_;
    }

    return std::string(fallback);
}

const Value* Value::Find(std::string_view key) const
{
    if (type_ != Type::Object)
    {
        return nullptr;
    }

    for (const auto& field : object_)
    {
        if (field.first.size() == key.size()
            && std::equal(key.begin(), key.end(), field.first.begin()))
        {
            return &field.second;
        }
    }

    return nullptr;
}

Value& Value::PushBack(Value value)
{
    type_ = Type::Array;
    array_.push_back(std::move(value));
    return array_.back();
}

Value& Value::Set(std::string key, Value value)
{
    type_ = Type::Object;

    for (auto& field : object_)
    {
        if (field.first == key)
        {
            field.second = std::move(value);
            return field.second;
        }
    }

    object_.emplace_back(std::move(key), std::move(value));
    return object_.back().second;
}

ParseResult Parse(std::string_view text)
{
    Parser parser(text);
    return parser.ParseDocument();
}

std::string Serialize(const Value& value, bool pretty)
{
    std::string out;
    SerializeInto(value, out, pretty, 0);
    if (pretty)
    {
        out += '\n';
    }

    return out;
}

} // namespace ld::json
