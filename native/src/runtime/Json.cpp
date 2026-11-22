#include <runtime/Json.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>

namespace canton::runtime {
    namespace {
        constexpr int maxDepth = 128;

        class Parser {
           public:
            explicit Parser(std::string_view text) : text_(text) {}

            JsonValue parse() {
                auto value = parseValue(0);
                skipSpace();
                if (at_ != text_.size()) fail("末尾有多余内容");
                return value;
            }

           private:
            [[noreturn]] void fail(const char* reason) const {
                throw std::runtime_error("JSON 无效（偏移 " + std::to_string(at_) + "）：" + reason);
            }

            void skipSpace() {
                while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) ++at_;
            }

            bool consume(std::string_view word) {
                if (text_.substr(at_, word.size()) != word) return false;
                at_ += word.size();
                return true;
            }

            JsonValue parseValue(int depth) {
                if (depth > maxDepth) fail("嵌套过深");
                skipSpace();
                if (at_ >= text_.size()) fail("内容提前结束");
                const auto c = text_[at_];
                if (c == '{') return parseObject(depth);
                if (c == '[') return parseArray(depth);
                if (c == '"') return {parseString()};
                if (consume("true")) return {true};
                if (consume("false")) return {false};
                if (consume("null")) return {};
                return {parseNumber()};
            }

            JsonValue parseObject(int depth) {
                ++at_;
                JsonValue::Object object;
                skipSpace();
                if (at_ < text_.size() && text_[at_] == '}') {
                    ++at_;
                    return {std::move(object)};
                }
                while (true) {
                    skipSpace();
                    if (at_ >= text_.size() || text_[at_] != '"') fail("对象的键必须是字符串");
                    auto key = parseString();
                    skipSpace();
                    if (at_ >= text_.size() || text_[at_] != ':') fail("对象缺少冒号");
                    ++at_;
                    object.insert_or_assign(std::move(key), parseValue(depth + 1));
                    skipSpace();
                    if (at_ < text_.size() && text_[at_] == ',') {
                        ++at_;
                        continue;
                    }
                    if (at_ < text_.size() && text_[at_] == '}') {
                        ++at_;
                        return {std::move(object)};
                    }
                    fail("对象缺少逗号或右花括号");
                }
            }

            JsonValue parseArray(int depth) {
                ++at_;
                JsonValue::Array array;
                skipSpace();
                if (at_ < text_.size() && text_[at_] == ']') {
                    ++at_;
                    return {std::move(array)};
                }
                while (true) {
                    array.push_back(parseValue(depth + 1));
                    skipSpace();
                    if (at_ < text_.size() && text_[at_] == ',') {
                        ++at_;
                        continue;
                    }
                    if (at_ < text_.size() && text_[at_] == ']') {
                        ++at_;
                        return {std::move(array)};
                    }
                    fail("数组缺少逗号或右方括号");
                }
            }

            unsigned parseHex4() {
                if (at_ + 4 > text_.size()) fail("转义序列提前结束");
                unsigned value = 0;
                const auto parsed = std::from_chars(text_.data() + at_, text_.data() + at_ + 4, value, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != text_.data() + at_ + 4) fail("转义序列不是四位十六进制");
                at_ += 4;
                return value;
            }

            static void appendUtf8(std::string& out, std::uint32_t codepoint) {
                if (codepoint < 0x80)
                    out += static_cast<char>(codepoint);
                else if (codepoint < 0x800) {
                    out += static_cast<char>(0xc0 | (codepoint >> 6));
                    out += static_cast<char>(0x80 | (codepoint & 0x3f));
                } else if (codepoint < 0x10000) {
                    out += static_cast<char>(0xe0 | (codepoint >> 12));
                    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (codepoint & 0x3f));
                } else {
                    out += static_cast<char>(0xf0 | (codepoint >> 18));
                    out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
                    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (codepoint & 0x3f));
                }
            }

            std::string parseString() {
                ++at_;
                std::string result;
                while (true) {
                    if (at_ >= text_.size()) fail("字符串没有结束");
                    const auto c = static_cast<unsigned char>(text_[at_++]);
                    if (c == '"') return result;
                    if (c < 0x20) fail("字符串里有未转义的控制字符");
                    if (c != '\\') {
                        result += static_cast<char>(c);
                        continue;
                    }
                    if (at_ >= text_.size()) fail("转义序列提前结束");
                    switch (text_[at_++]) {
                        case '"':
                            result += '"';
                            break;
                        case '\\':
                            result += '\\';
                            break;
                        case '/':
                            result += '/';
                            break;
                        case 'b':
                            result += '\b';
                            break;
                        case 'f':
                            result += '\f';
                            break;
                        case 'n':
                            result += '\n';
                            break;
                        case 'r':
                            result += '\r';
                            break;
                        case 't':
                            result += '\t';
                            break;
                        case 'u': {
                            std::uint32_t codepoint = parseHex4();
                            if (codepoint >= 0xd800 && codepoint <= 0xdbff && text_.substr(at_, 2) == "\\u") {
                                // 代理对合成一个码点；落单的代理项换成替换字符
                                const auto saved = at_;
                                at_ += 2;
                                const auto low = parseHex4();
                                if (low >= 0xdc00 && low <= 0xdfff)
                                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                                else
                                    at_ = saved;
                            }
                            if (codepoint >= 0xd800 && codepoint <= 0xdfff) codepoint = 0xfffd;
                            appendUtf8(result, codepoint);
                            break;
                        }
                        default:
                            fail("未知的转义序列");
                    }
                }
            }

            double parseNumber() {
                const auto start = at_;
                if (at_ < text_.size() && text_[at_] == '-') ++at_;
                const auto digits = [&] {
                    const auto from = at_;
                    while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
                    return at_ - from;
                };
                const auto leading = at_;
                if (digits() == 0) fail("不是合法的值");
                if (text_[leading] == '0' && at_ - leading > 1) fail("数字不能有前导零");
                if (at_ < text_.size() && text_[at_] == '.') {
                    ++at_;
                    if (digits() == 0) fail("小数点后缺少数字");
                }
                if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
                    ++at_;
                    if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;
                    if (digits() == 0) fail("指数缺少数字");
                }
                double value = 0;
                const auto parsed = std::from_chars(text_.data() + start, text_.data() + at_, value);
                if (parsed.ec != std::errc{} || parsed.ptr != text_.data() + at_) fail("数字超出范围");
                return value;
            }

            std::string_view text_;
            std::size_t at_ = 0;
        };

        void writeString(std::string& out, std::string_view text) {
            constexpr char digits[] = "0123456789abcdef";
            out += '"';
            for (const auto raw : text) {
                const auto c = static_cast<unsigned char>(raw);
                if (c == '"')
                    out += "\\\"";
                else if (c == '\\')
                    out += "\\\\";
                else if (c == '\n')
                    out += "\\n";
                else if (c == '\r')
                    out += "\\r";
                else if (c == '\t')
                    out += "\\t";
                else if (c < 0x20) {
                    out += "\\u00";
                    out += digits[c >> 4];
                    out += digits[c & 15];
                } else
                    out += raw;
            }
            out += '"';
        }

        void write(std::string& out, const JsonValue& value) {
            if (std::holds_alternative<std::monostate>(value.value))
                out += "null";
            else if (const auto* flag = std::get_if<bool>(&value.value))
                out += *flag ? "true" : "false";
            else if (const auto* number = std::get_if<double>(&value.value)) {
                // JSON 没有无穷与 NaN
                if (!std::isfinite(*number))
                    out += "null";
                else {
                    char buffer[32];
                    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), *number);
                    out.append(buffer, written.ptr);
                }
            } else if (const auto* text = std::get_if<std::string>(&value.value))
                writeString(out, *text);
            else if (const auto* array = std::get_if<JsonValue::Array>(&value.value)) {
                out += '[';
                for (std::size_t i = 0; i < array->size(); ++i) {
                    if (i) out += ',';
                    write(out, (*array)[i]);
                }
                out += ']';
            } else {
                const auto& object = std::get<JsonValue::Object>(value.value);
                out += '{';
                bool first = true;
                for (const auto& [key, item] : object) {
                    if (!std::exchange(first, false)) out += ',';
                    writeString(out, key);
                    out += ':';
                    write(out, item);
                }
                out += '}';
            }
        }
    }

    JsonValue parseJson(std::string_view text) {
        return Parser(text).parse();
    }

    std::string writeJson(const JsonValue& value) {
        std::string out;
        write(out, value);
        return out;
    }

    const JsonValue* jsonMember(const JsonValue& value, std::string_view name) {
        const auto* object = std::get_if<JsonValue::Object>(&value.value);
        if (!object) return nullptr;
        const auto found = object->find(std::string(name));
        return found == object->end() ? nullptr : &found->second;
    }

    const std::string* jsonString(const JsonValue* value) {
        return value ? std::get_if<std::string>(&value->value) : nullptr;
    }

    const double* jsonNumber(const JsonValue* value) {
        return value ? std::get_if<double>(&value->value) : nullptr;
    }

    const JsonValue::Array* jsonArray(const JsonValue* value) {
        return value ? std::get_if<JsonValue::Array>(&value->value) : nullptr;
    }

    bool jsonBoolean(const JsonValue* value, bool fallback) {
        const auto* flag = value ? std::get_if<bool>(&value->value) : nullptr;
        return flag ? *flag : fallback;
    }
}
