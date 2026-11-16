#ifndef IRIS_GENTL_OUTPUT_HPP
#define IRIS_GENTL_OUTPUT_HPP

#include <iris.h>

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace gentl {

// GenTL strings may contain arbitrary bytes. Preserve UTF-8 and escape invalid bytes.
inline size_t utf8_length(std::string_view text, size_t i)
{
    const auto first = static_cast<unsigned char>(text[i]);
    const size_t length = first < 0xc2 ? 0 : first < 0xe0 ? 2 :
                          first < 0xf0 ? 3 : first <= 0xf4 ? 4 : 0;
    if (!length || length > text.size() - i) return 0;
    const auto second = static_cast<unsigned char>(text[i + 1]);
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
        (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return 0;
    for (size_t j = 1; j < length; ++j)
        if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) return 0;
    return length;
}

inline std::string json_string(std::string_view text)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c >= 0x20 && c < 0x80) {
            result += static_cast<char>(c);
        } else if (const auto length = utf8_length(text, i)) {
            result.append(text.substr(i, length));
            i += length - 1;
        } else {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 0x0f];
        }
    }
    return result + '"';
}

inline std::string json_number(double value)
{
    if (!std::isfinite(value)) return "null";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

class Json {
    std::string data;
    char end;

    void append(std::string_view value)
    {
        if (data.size() > 1) data += ',';
        data += value;
    }

public:
    explicit Json(bool array = false) : data(1, array ? '[' : '{'), end(array ? ']' : '}') {}

    void raw(std::string_view key, std::string_view value)
    {
        append(json_string(key) + ':' + std::string(value));
    }

    void string(std::string_view key, std::string_view value) { raw(key, json_string(value)); }
    void item(std::string_view value) { append(value); }

    template<class Number>
    void number(std::string_view key, Number value) { raw(key, std::to_string(value)); }

    std::string str() const { return data + end; }
    void print() const { std::puts(str().c_str()); }
};

inline const char *feature_type(iris_feature_type_t type)
{
    const char *names[] = {"-", "integer", "float", "boolean", "enumeration", "string",
                           "command", "other", "category", "register"};
    const auto index = static_cast<size_t>(type);
    return index < std::size(names) ? names[index] : "-";
}

inline const char *feature_access(iris_feature_access_t access)
{
    const char *names[] = {"-", "NI", "NA", "WO", "RO", "RW"};
    const auto index = static_cast<size_t>(access);
    return index < std::size(names) ? names[index] : "-";
}

} // namespace gentl

#endif
