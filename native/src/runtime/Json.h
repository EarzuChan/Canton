#pragma once

#include <quickjs/LiveModule.h>

#include <string>
#include <string_view>

namespace canton::runtime {
    // JSON 值沿用热更新协议的 HotValue：null、布尔、数、字符串、数组、对象
    using JsonValue = quickjs::HotValue;

    // 严格按 RFC 8259 解析；格式不对、嵌套过深或有多余内容时抛 std::runtime_error
    [[nodiscard]] JsonValue parseJson(std::string_view text);
    [[nodiscard]] std::string writeJson(const JsonValue& value);

    // 取对象成员；不是对象或没有该成员时返回空指针
    [[nodiscard]] const JsonValue* jsonMember(const JsonValue& value, std::string_view name);
    [[nodiscard]] const std::string* jsonString(const JsonValue* value);
    [[nodiscard]] const double* jsonNumber(const JsonValue* value);
    [[nodiscard]] const JsonValue::Array* jsonArray(const JsonValue* value);
    [[nodiscard]] bool jsonBoolean(const JsonValue* value, bool fallback = false);
}
