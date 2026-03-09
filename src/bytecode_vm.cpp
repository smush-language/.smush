#include "bytecode_vm.h"

#include "ui_backend.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unistd.h>
#endif

namespace smush {

namespace {

std::vector<std::string> g_processArgs;
std::vector<std::string> g_scriptArgs;
std::mt19937_64 g_prng(std::random_device{}());

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

struct ThrownValue {
    Value value;
    SourceLocation loc;
};

NativeSocket toNativeSocket(intptr_t handle) {
    return static_cast<NativeSocket>(handle);
}

void closeNativeSocket(intptr_t handle) {
    if (handle < 0) {
        return;
    }
#if defined(_WIN32)
    closesocket(toNativeSocket(handle));
#else
    close(toNativeSocket(handle));
#endif
}

void ensureSocketsInitialized() {
#if defined(_WIN32)
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("failed to initialize Winsock");
        }
        initialized = true;
    }
#endif
}

std::string urlEncodeComponent(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

std::string urlDecodeComponent(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            result.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
            i += 2;
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(ch); break;
        }
    }
    return result;
}

std::string hexEncode(const std::vector<uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        result.push_back(kHex[(byte >> 4) & 0x0f]);
        result.push_back(kHex[byte & 0x0f]);
    }
    return result;
}

std::string hexEncode64(uint64_t value) {
    std::vector<uint8_t> bytes(8);
    for (size_t i = 0; i < 8; ++i) {
        bytes[7 - i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffu);
    }
    return hexEncode(bytes);
}

uint64_t fnv1a64(const uint8_t* data, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

uint32_t rotr32(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32u - bits));
}

std::array<uint8_t, 32> sha256Bytes(const uint8_t* data, size_t size) {
    static constexpr std::array<uint32_t, 64> k = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    std::vector<uint8_t> padded(data, data + size);
    padded.push_back(0x80u);
    while ((padded.size() % 64) != 56) {
        padded.push_back(0);
    }
    const uint64_t bitLength = static_cast<uint64_t>(size) * 8ull;
    for (int i = 7; i >= 0; --i) {
        padded.push_back(static_cast<uint8_t>((bitLength >> (i * 8)) & 0xffu));
    }

    uint32_t h0 = 0x6a09e667u;
    uint32_t h1 = 0xbb67ae85u;
    uint32_t h2 = 0x3c6ef372u;
    uint32_t h3 = 0xa54ff53au;
    uint32_t h4 = 0x510e527fu;
    uint32_t h5 = 0x9b05688cu;
    uint32_t h6 = 0x1f83d9abu;
    uint32_t h7 = 0x5be0cd19u;

    for (size_t chunk = 0; chunk < padded.size(); chunk += 64) {
        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t offset = chunk + (i * 4);
            w[i] = (static_cast<uint32_t>(padded[offset]) << 24) |
                   (static_cast<uint32_t>(padded[offset + 1]) << 16) |
                   (static_cast<uint32_t>(padded[offset + 2]) << 8) |
                   static_cast<uint32_t>(padded[offset + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        uint32_t f = h5;
        uint32_t g = h6;
        uint32_t h = h7;

        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }

    std::array<uint8_t, 32> digest{};
    const std::array<uint32_t, 8> state = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (size_t i = 0; i < state.size(); ++i) {
        digest[i * 4] = static_cast<uint8_t>((state[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xffu);
    }
    return digest;
}

std::string sha256Hex(const uint8_t* data, size_t size) {
    const auto digest = sha256Bytes(data, size);
    return hexEncode(std::vector<uint8_t>(digest.begin(), digest.end()));
}

std::string jsonStringifyValue(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Null:
            return "null";
        case Value::Kind::Bool:
            return value.boolValue ? "true" : "false";
        case Value::Kind::Int:
            return std::to_string(value.intValue);
        case Value::Kind::Float: {
            std::ostringstream stream;
            stream << value.floatValue;
            return stream.str();
        }
        case Value::Kind::String:
            return "\"" + jsonEscape(value.stringValue) + "\"";
        case Value::Kind::Object:
            if (!value.objectValue) {
                return "null";
            }
            if (value.objectValue->kind == ObjectKind::List) {
                auto* list = static_cast<ListObject*>(value.objectValue);
                std::string result = "[";
                for (size_t i = 0; i < list->elements.size(); ++i) {
                    if (i > 0) {
                        result += ",";
                    }
                    result += jsonStringifyValue(list->elements[i]);
                }
                result += "]";
                return result;
            }
            if (value.objectValue->kind == ObjectKind::Tuple) {
                auto* tuple = static_cast<TupleObject*>(value.objectValue);
                std::string result = "[";
                for (size_t i = 0; i < tuple->elements.size(); ++i) {
                    if (i > 0) {
                        result += ",";
                    }
                    result += jsonStringifyValue(tuple->elements[i]);
                }
                result += "]";
                return result;
            }
            if (value.objectValue->kind == ObjectKind::Map) {
                auto* map = static_cast<MapObject*>(value.objectValue);
                std::string result = "{";
                for (size_t i = 0; i < map->entries.size(); ++i) {
                    if (i > 0) {
                        result += ",";
                    }
                    if (map->entries[i].first.kind != Value::Kind::String) {
                        throw std::runtime_error("json_stringify expects map keys to be strings");
                    }
                    result += "\"" + jsonEscape(map->entries[i].first.stringValue) + "\":" +
                              jsonStringifyValue(map->entries[i].second);
                }
                result += "}";
                return result;
            }
            throw std::runtime_error("json_stringify does not support this runtime value");
    }
    return "null";
}

class JsonParser {
public:
    JsonParser(BytecodeVM& runtime, std::string source)
        : vm(runtime), text(std::move(source)) {}

    Value parse() {
        skipWhitespace();
        Value value = parseValue();
        skipWhitespace();
        if (pos != text.size()) {
            throw std::runtime_error("json_parse encountered trailing content");
        }
        return value;
    }

private:
    BytecodeVM& vm;
    std::string text;
    size_t pos = 0;

    void skipWhitespace() {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
            ++pos;
        }
    }

    char peek() const {
        return pos < text.size() ? text[pos] : '\0';
    }

    char consume() {
        if (pos >= text.size()) {
            throw std::runtime_error("json_parse reached unexpected end of input");
        }
        return text[pos++];
    }

    bool consumeIf(char expected) {
        if (peek() == expected) {
            ++pos;
            return true;
        }
        return false;
    }

    void expectLiteral(const std::string& literal) {
        if (text.compare(pos, literal.size(), literal) != 0) {
            throw std::runtime_error("json_parse expected '" + literal + "'");
        }
        pos += literal.size();
    }

    Value parseValue() {
        skipWhitespace();
        switch (peek()) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return Value::string(parseString());
            case 't': expectLiteral("true"); return Value::boolean(true);
            case 'f': expectLiteral("false"); return Value::boolean(false);
            case 'n': expectLiteral("null"); return Value::null();
            default:
                if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                    return parseNumber();
                }
                throw std::runtime_error("json_parse encountered an invalid value");
        }
    }

    std::string parseString() {
        if (consume() != '"') {
            throw std::runtime_error("json_parse expected string");
        }
        std::string result;
        while (true) {
            if (pos >= text.size()) {
                throw std::runtime_error("json_parse encountered unterminated string");
            }
            char ch = consume();
            if (ch == '"') {
                break;
            }
            if (ch == '\\') {
                if (pos >= text.size()) {
                    throw std::runtime_error("json_parse encountered invalid escape");
                }
                char esc = consume();
                switch (esc) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    default: throw std::runtime_error("json_parse does not support this escape sequence");
                }
                continue;
            }
            result.push_back(ch);
        }
        return result;
    }

    Value parseNumber() {
        const size_t start = pos;
        consumeIf('-');
        while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            ++pos;
        }
        bool isFloat = false;
        if (consumeIf('.')) {
            isFloat = true;
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++pos;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            isFloat = true;
            ++pos;
            if (peek() == '+' || peek() == '-') {
                ++pos;
            }
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++pos;
            }
        }
        const std::string token = text.substr(start, pos - start);
        return isFloat ? Value::floating(std::stod(token)) : Value::integer(std::stoll(token));
    }

    Value parseArray() {
        consume();
        std::vector<Value> elements;
        skipWhitespace();
        if (consumeIf(']')) {
            return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(elements)));
        }
        while (true) {
            elements.push_back(parseValue());
            skipWhitespace();
            if (consumeIf(']')) {
                break;
            }
            if (!consumeIf(',')) {
                throw std::runtime_error("json_parse expected ',' or ']'");
            }
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(elements)));
    }

    Value parseObject() {
        consume();
        auto* map = vm.runtimeHeap().allocate<MapObject>();
        skipWhitespace();
        if (consumeIf('}')) {
            return Value::object(map);
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                throw std::runtime_error("json_parse expected string object key");
            }
            Value key = Value::string(parseString());
            skipWhitespace();
            if (!consumeIf(':')) {
                throw std::runtime_error("json_parse expected ':' after object key");
            }
            Value value = parseValue();
            map->entries.push_back({std::move(key), std::move(value)});
            skipWhitespace();
            if (consumeIf('}')) {
                break;
            }
            if (!consumeIf(',')) {
                throw std::runtime_error("json_parse expected ',' or '}'");
            }
        }
        return Value::object(map);
    }
};

std::string readStringConstant(const BytecodeModule& module, int64_t operand) {
    return std::get<std::string>(module.constants[static_cast<size_t>(operand)].value);
}

std::string trimAscii(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string lowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string upperAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string replaceAll(std::string value, const std::string& needle, const std::string& replacement) {
    if (needle.empty()) {
        return value;
    }
    size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return value;
}

std::vector<std::string> splitString(const std::string& value, const std::string& separator) {
    if (separator.empty()) {
        std::vector<std::string> pieces;
        pieces.reserve(value.size());
        for (char ch : value) {
            pieces.push_back(std::string(1, ch));
        }
        return pieces;
    }
    std::vector<std::string> pieces;
    size_t start = 0;
    while (true) {
        const size_t pos = value.find(separator, start);
        if (pos == std::string::npos) {
            pieces.push_back(value.substr(start));
            break;
        }
        pieces.push_back(value.substr(start, pos - start));
        start = pos + separator.size();
    }
    return pieces;
}

std::string joinValues(const std::vector<Value>& values, const std::string& separator) {
    std::string result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            result += separator;
        }
        result += toString(values[i]);
    }
    return result;
}

std::string formatTemplate(const std::string& pattern, const std::vector<Value>& values) {
    std::string result;
    size_t valueIndex = 0;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            if (valueIndex < values.size()) {
                result += toString(values[valueIndex++]);
            } else {
                result += "{}";
            }
            ++i;
            continue;
        }
        result.push_back(pattern[i]);
    }
    return result;
}

std::vector<Value> iterableValues(const Value& iterable) {
    std::vector<Value> values;
    if (iterable.kind != Value::Kind::Object || !iterable.objectValue) {
        throw std::runtime_error("value is not iterable");
    }
    switch (iterable.objectValue->kind) {
        case ObjectKind::List:
            values = static_cast<ListObject*>(iterable.objectValue)->elements;
            return values;
        case ObjectKind::Tuple:
            values = static_cast<TupleObject*>(iterable.objectValue)->elements;
            return values;
        case ObjectKind::Set:
            values = static_cast<SetObject*>(iterable.objectValue)->elements;
            return values;
        case ObjectKind::Range: {
            auto* range = static_cast<RangeObject*>(iterable.objectValue);
            const int64_t step = range->start <= range->end ? 1 : -1;
            const int64_t span = std::llabs(range->end - range->start) + (range->inclusive ? 1 : 0);
            values.reserve(static_cast<size_t>(std::max<int64_t>(span, 0)));
            for (int64_t i = 0; i < span; ++i) {
                values.push_back(Value::integer(range->start + (i * step)));
            }
            return values;
        }
        default:
            break;
    }
    throw std::runtime_error("value is not iterable");
}

std::string genericPathString(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

std::string zeroPadInt(int value, int width) {
    std::ostringstream stream;
    stream << std::setw(width) << std::setfill('0') << value;
    return stream.str();
}

std::string formatDateParts(int year, int month, int day) {
    return zeroPadInt(year, 4) + "-" + zeroPadInt(month, 2) + "-" + zeroPadInt(day, 2);
}

void parseIsoDate(const std::string& value, int& year, int& month, int& day) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
        throw std::runtime_error("expected date in YYYY-MM-DD format");
    }
    year = std::stoi(value.substr(0, 4));
    month = std::stoi(value.substr(5, 2));
    day = std::stoi(value.substr(8, 2));
}

std::tm utcTime(std::time_t value) {
    std::tm result{};
#if defined(_WIN32)
    gmtime_s(&result, &value);
#else
    gmtime_r(&value, &result);
#endif
    return result;
}

uint32_t decodeHandlerPc(int64_t operand) {
    return static_cast<uint32_t>((static_cast<uint64_t>(operand) >> 32) & 0xffffffffu);
}

uint32_t decodeHandlerSlot(int64_t operand) {
    return static_cast<uint32_t>(static_cast<uint64_t>(operand) & 0xffffffffu);
}

std::string runtimeTypeName(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Null: return "Null";
        case Value::Kind::Bool: return "Bool";
        case Value::Kind::Int: return "Int";
        case Value::Kind::Float: return "Float";
        case Value::Kind::String: return "String";
        case Value::Kind::Object:
            if (!value.objectValue) {
                return "Null";
            }
            if (value.objectValue->kind == ObjectKind::Instance) {
                return static_cast<InstanceObject*>(value.objectValue)->typeName;
            }
            if (value.objectValue->kind == ObjectKind::Future) {
                return "Future";
            }
            if (value.objectValue->kind == ObjectKind::Bytes) {
                return "Bytes";
            }
            if (value.objectValue->kind == ObjectKind::TextStream) {
                return "TextStream";
            }
            if (value.objectValue->kind == ObjectKind::TcpSocket) {
                return "Socket";
            }
            if (value.objectValue->kind == ObjectKind::TcpListener) {
                return "Listener";
            }
            if (value.objectValue->kind == ObjectKind::UiApp) {
                return "UiApp";
            }
            if (value.objectValue->kind == ObjectKind::UiWindow) {
                return "UiWindow";
            }
            if (value.objectValue->kind == ObjectKind::UiView) {
                return "UiView";
            }
            if (value.objectValue->kind == ObjectKind::UiTheme) {
                return "UiTheme";
            }
            if (value.objectValue->kind == ObjectKind::Tuple) {
                return "Tuple";
            }
            if (value.objectValue->kind == ObjectKind::Map) {
                return "Map";
            }
            if (value.objectValue->kind == ObjectKind::Set) {
                return "Set";
            }
            return value.objectValue->describe();
    }
    return "Unknown";
}

bool isFunctionTypeName(const std::string& name) {
    return name.find("->") != std::string::npos;
}

bool isFunctionValue(const BytecodeModule& module, const Value& value) {
    if (value.kind == Value::Kind::String) {
        return module.functionMap.find(value.stringValue) != module.functionMap.end();
    }
    if (value.kind != Value::Kind::Object || !value.objectValue) {
        return false;
    }
    return value.objectValue->kind == ObjectKind::BytecodeBoundMethod ||
           value.objectValue->kind == ObjectKind::BytecodeClosure;
}

bool bytecodeNamedSubtype(const BytecodeModule& module, const std::string& source, const std::string& target) {
    if (source == target || target.empty()) {
        return true;
    }
    auto typeIt = module.typeMap.find(source);
    while (typeIt != module.typeMap.end()) {
        const auto& type = module.types[typeIt->second];
        if (type.baseType == target) {
            return true;
        }
        if (std::find(type.interfaces.begin(), type.interfaces.end(), target) != type.interfaces.end()) {
            return true;
        }
        if (type.baseType.empty()) {
            break;
        }
        typeIt = module.typeMap.find(type.baseType);
    }
    auto ifaceIt = module.interfaceMap.find(source);
    while (ifaceIt != module.interfaceMap.end()) {
        const auto& iface = module.interfaces[ifaceIt->second];
        if (std::find(iface.baseInterfaces.begin(), iface.baseInterfaces.end(), target) != iface.baseInterfaces.end()) {
            return true;
        }
        if (iface.baseInterfaces.empty()) {
            break;
        }
        bool advanced = false;
        for (const auto& base : iface.baseInterfaces) {
            auto nextIt = module.interfaceMap.find(base);
            if (nextIt != module.interfaceMap.end()) {
                ifaceIt = nextIt;
                advanced = true;
                break;
            }
        }
        if (!advanced) {
            break;
        }
    }
    return false;
}

bool bytecodeTypeEquivalent(const BytecodeModule& module, const IRType& left, const IRType& right);
bool sameBytecodeSignature(const BytecodeFunction& left, const BytecodeFunction& right);

bool bytecodeTypeAssignable(const BytecodeModule& module, const IRType& target, const IRType& source) {
    if (target.kind == IRTypeKind::Any || source.kind == IRTypeKind::Any) {
        return true;
    }
    if (source.kind == IRTypeKind::Null) {
        return target.nullable || target.kind == IRTypeKind::Null || target.kind == IRTypeKind::Any;
    }
    if (target.kind == IRTypeKind::Float && source.kind == IRTypeKind::Int) {
        return true;
    }
    if (target.kind == IRTypeKind::Object && source.kind == IRTypeKind::Object) {
        return bytecodeNamedSubtype(module, source.name, target.name) &&
               (target.nullable || !source.nullable);
    }
    if (target.kind != source.kind || target.name != source.name) {
        return false;
    }
    return target.nullable || !source.nullable;
}

bool bytecodeTypeEquivalent(const BytecodeModule& module, const IRType& left, const IRType& right) {
    return bytecodeTypeAssignable(module, left, right) && bytecodeTypeAssignable(module, right, left);
}

bool interfaceMethodMatches(const BytecodeModule& module, const BytecodeInterfaceMethod& required, const BytecodeFunction& candidate) {
    if (required.name != candidate.name ||
        required.async != candidate.async ||
        required.staticMethod != candidate.staticMethod ||
        required.parameterTypes.size() != candidate.parameters.size() - (candidate.staticMethod ? 0 : 1)) {
        return false;
    }
    const size_t offset = candidate.staticMethod ? 0 : 1;
    for (size_t i = 0; i < required.parameterTypes.size(); ++i) {
        if (!bytecodeTypeEquivalent(module, required.parameterTypes[i], candidate.parameters[offset + i].type)) {
            return false;
        }
    }
    return bytecodeTypeAssignable(module, required.returnType, candidate.returnType);
}

bool instanceSatisfiesInterface(const BytecodeModule& module, const InstanceObject& instance, const std::string& interfaceName);

bool interfaceLayoutSatisfied(const BytecodeModule& module, const InstanceObject& instance, const BytecodeInterfaceLayout& iface) {
    for (const auto& base : iface.baseInterfaces) {
        if (!instanceSatisfiesInterface(module, instance, base)) {
            return false;
        }
    }
    for (const auto& required : iface.methods) {
        auto methodIt = instance.bytecodeMethods.find(required.name);
        if (methodIt == instance.bytecodeMethods.end()) {
            return false;
        }
        bool matched = false;
        for (uint32_t functionIndex : methodIt->second) {
            if (functionIndex >= module.functions.size()) {
                continue;
            }
            const auto& function = module.functions[functionIndex];
            if (!function.abstractMethod && interfaceMethodMatches(module, required, function)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

bool instanceSatisfiesInterface(const BytecodeModule& module, const InstanceObject& instance, const std::string& interfaceName) {
    if (instance.interfaces.find(interfaceName) != instance.interfaces.end() ||
        instance.supertypes.find(interfaceName) != instance.supertypes.end()) {
        return true;
    }
    auto ifaceIt = module.interfaceMap.find(interfaceName);
    if (ifaceIt == module.interfaceMap.end()) {
        return false;
    }
    return interfaceLayoutSatisfied(module, instance, module.interfaces[ifaceIt->second]);
}

bool runtimeMatchesIRType(const BytecodeModule& module, const Value& value, const IRType& type) {
    if (type.kind == IRTypeKind::Any) {
        return true;
    }
    if (value.kind == Value::Kind::Null) {
        return type.nullable || type.kind == IRTypeKind::Null || type.kind == IRTypeKind::Any;
    }
    switch (type.kind) {
        case IRTypeKind::Bool:
            return value.kind == Value::Kind::Bool;
        case IRTypeKind::Int:
            return value.kind == Value::Kind::Int;
        case IRTypeKind::Float:
            return value.kind == Value::Kind::Float || value.kind == Value::Kind::Int;
        case IRTypeKind::String:
            return value.kind == Value::Kind::String;
        case IRTypeKind::List:
            return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::List;
        case IRTypeKind::Tuple:
            return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::Tuple;
        case IRTypeKind::Map:
            return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::Map;
        case IRTypeKind::Set:
            return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::Set;
        case IRTypeKind::Range:
            return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::Range;
        case IRTypeKind::Future:
            return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::Future;
        case IRTypeKind::Object:
            if (isFunctionTypeName(type.name)) {
                return isFunctionValue(module, value);
            }
            if (type.name == "Bytes") {
                return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::Bytes;
            }
            if (type.name == "TextStream") {
                return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::TextStream;
            }
            if (type.name == "Socket") {
                return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::TcpSocket;
            }
            if (type.name == "Listener") {
                return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::TcpListener;
            }
            if (type.name == "UiApp") {
                return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::UiApp;
            }
            if (type.name == "UiWindow") {
                return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::UiWindow;
            }
            if (type.name == "UiView") {
                return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::UiView;
            }
            if (type.name == "UiTheme") {
                return value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::UiTheme;
            }
            if (value.kind == Value::Kind::Object && value.objectValue && value.objectValue->kind == ObjectKind::Instance) {
                auto* instance = static_cast<InstanceObject*>(value.objectValue);
                return type.name.empty() || instance->typeName == type.name ||
                       instanceSatisfiesInterface(module, *instance, type.name) ||
                       instance->supertypes.find(type.name) != instance->supertypes.end();
            }
            return type.name.find("Function") != std::string::npos && value.kind == Value::Kind::Object;
        case IRTypeKind::Void:
        case IRTypeKind::Null:
        case IRTypeKind::Any:
            break;
    }
    return false;
}

const BytecodeFieldLayout* findFieldLayout(const BytecodeModule& module,
                                           const std::string& typeName,
                                           const std::string& fieldName) {
    auto typeIt = module.typeMap.find(typeName);
    if (typeIt == module.typeMap.end()) {
        return nullptr;
    }
    const auto& layout = module.types[typeIt->second];
    for (const auto& field : layout.fields) {
        if (field.name == fieldName) {
            return &field;
        }
    }
    return nullptr;
}

void collectInheritedInstanceMethods(const BytecodeModule& module,
                                     uint32_t typeIndex,
                                     const std::string& member,
                                     std::vector<uint32_t>& out) {
    if (typeIndex >= module.types.size()) {
        return;
    }
    const auto& type = module.types[typeIndex];
    if (!type.baseType.empty()) {
        auto baseIt = module.typeMap.find(type.baseType);
        if (baseIt != module.typeMap.end()) {
            collectInheritedInstanceMethods(module, baseIt->second, member, out);
        }
    }
    for (uint32_t methodIndex : type.methodIndices) {
        const auto& function = module.functions[methodIndex];
        if (function.name != member) {
            continue;
        }
        out.erase(
            std::remove_if(
                out.begin(),
                out.end(),
                [&](uint32_t existingIndex) {
                    return sameBytecodeSignature(module.functions[existingIndex], function);
                }),
            out.end());
        out.push_back(methodIndex);
    }
}

bool fieldAllowsImplicitNull(const BytecodeFieldLayout& field) {
    return field.type.nullable ||
           field.type.kind == IRTypeKind::Any ||
           field.type.kind == IRTypeKind::Null;
}

Value safeCastValue(const BytecodeModule& module, const Value& value, const IRType& targetType) {
    return runtimeMatchesIRType(module, value, targetType) ? value : Value::null();
}

Value checkedCastValue(const BytecodeModule& module, const Value& value, const IRType& targetType, const SourceLocation& loc) {
    if (runtimeMatchesIRType(module, value, targetType)) {
        return value;
    }
    throw std::runtime_error(
        loc.filename + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column) +
        ": cast to '" + targetType.toString() + "' failed for value of type '" + runtimeTypeName(value) + "'");
}

int bytecodeOverloadScore(const BytecodeModule& module, const BytecodeFunction& function, const std::vector<Value>& arguments, bool boundMethod) {
    const size_t offset = boundMethod ? 1 : 0;
    if (function.parameters.size() < offset || function.parameters.size() - offset != arguments.size()) {
        return -1;
    }
    int score = 0;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const IRType& type = function.parameters[offset + i].type;
        if (!runtimeMatchesIRType(module, arguments[i], type)) {
            return -1;
        }
        if (type.kind == IRTypeKind::Any) {
            score += 0;
        } else if (type.kind == IRTypeKind::Float) {
            score += arguments[i].kind == Value::Kind::Float ? 2 : 1;
        } else {
            score += 2;
        }
    }
    return score;
}

bool sameBytecodeSignature(const BytecodeFunction& left, const BytecodeFunction& right) {
    if (left.name != right.name ||
        left.staticMethod != right.staticMethod ||
        left.constructor != right.constructor ||
        left.parameters.size() != right.parameters.size()) {
        return false;
    }
    const size_t offset = (!left.staticMethod && !left.parameters.empty() && !right.parameters.empty()) ? 1 : 0;
    for (size_t i = offset; i < left.parameters.size(); ++i) {
        if (left.parameters[i].type.toString() != right.parameters[i].type.toString()) {
            return false;
        }
    }
    return true;
}

void collectInheritedStaticMethods(const BytecodeModule& module,
                                  uint32_t typeIndex,
                                  const std::string& member,
                                  std::vector<uint32_t>& out) {
    if (typeIndex >= module.types.size()) {
        return;
    }
    const auto& type = module.types[typeIndex];
    if (!type.baseType.empty()) {
        auto baseIt = module.typeMap.find(type.baseType);
        if (baseIt != module.typeMap.end()) {
            collectInheritedStaticMethods(module, baseIt->second, member, out);
        }
    }
    for (uint32_t methodIndex : type.staticMethodIndices) {
        const auto& function = module.functions[methodIndex];
        if (function.name != member) {
            continue;
        }
        out.erase(
            std::remove_if(
                out.begin(),
                out.end(),
                [&](uint32_t existingIndex) {
                    return sameBytecodeSignature(module.functions[existingIndex], function);
                }),
            out.end());
        out.push_back(methodIndex);
    }
}

void populateInstanceForType(const BytecodeModule& module,
                             uint32_t typeIndex,
                             InstanceObject& instance) {
    if (typeIndex >= module.types.size()) {
        return;
    }
    const auto& type = module.types[typeIndex];
    if (!type.baseType.empty()) {
        auto baseIt = module.typeMap.find(type.baseType);
        if (baseIt != module.typeMap.end()) {
            populateInstanceForType(module, baseIt->second, instance);
            instance.supertypes.insert(type.baseType);
        }
    }
    instance.supertypes.insert(type.name);
    for (const auto& field : type.fields) {
        instance.fields.emplace(field.name, Value::null());
    }
    for (const auto& iface : type.interfaces) {
        instance.interfaces.insert(iface);
        instance.supertypes.insert(iface);
    }
    for (uint32_t methodIndex : type.methodIndices) {
        const auto& method = module.functions[methodIndex];
        auto& overloads = instance.bytecodeMethods[method.name];
        overloads.erase(
            std::remove_if(
                overloads.begin(),
                overloads.end(),
                [&](uint32_t existingIndex) {
                    return sameBytecodeSignature(module.functions[existingIndex], method);
                }),
            overloads.end());
        overloads.push_back(methodIndex);
    }
}

void validateInitializedFields(const BytecodeModule& module,
                               const InstanceObject& instance,
                               const SourceLocation& loc) {
    auto typeIt = module.typeMap.find(instance.typeName);
    if (typeIt == module.typeMap.end()) {
        return;
    }
    const auto& type = module.types[typeIt->second];
    for (const auto& field : type.fields) {
        auto valueIt = instance.fields.find(field.name);
        if (valueIt == instance.fields.end()) {
            throw std::runtime_error(
                loc.filename + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column) +
                ": missing field '" + field.name + "' on '" + instance.typeName + "'");
        }
        if (valueIt->second.kind == Value::Kind::Null && !fieldAllowsImplicitNull(field)) {
            throw std::runtime_error(
                loc.filename + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column) +
                ": field '" + field.name + "' on '" + instance.typeName + "' is not initialized");
        }
        if (!runtimeMatchesIRType(module, valueIt->second, field.type)) {
            throw std::runtime_error(
                loc.filename + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column) +
                ": field '" + field.name + "' on '" + instance.typeName + "' expected '" + field.type.toString() +
                "' but got '" + runtimeTypeName(valueIt->second) + "'");
        }
    }
}

}  // namespace

BytecodeVM::BytecodeVM() : gcRootEnv(std::make_shared<Environment>()) {
    environmentRoots.push_back(gcRootEnv.get());
    installBuiltins();
}

void BytecodeVM::setProcessArgs(std::vector<std::string> args) {
    g_processArgs = std::move(args);
}

void BytecodeVM::setScriptArgs(std::vector<std::string> args) {
    g_scriptArgs = std::move(args);
}

Value BytecodeVM::run(const std::shared_ptr<BytecodeModule>& module) {
    return runInternal(module, false);
}

Value BytecodeVM::runInteractive(const std::shared_ptr<BytecodeModule>& module) {
    return runInternal(module, true);
}

Value BytecodeVM::invokeUiCallback(const Value& callback,
                                   const std::vector<Value>& arguments,
                                   const SourceLocation& loc) {
    if (callback.kind == Value::Kind::Null) {
        return Value::null();
    }
    Value result = callValue(callback, arguments, loc);
    if (result.kind == Value::Kind::Object && result.objectValue &&
        result.objectValue->kind == ObjectKind::Future) {
        return driveFuture(result, loc);
    }
    return result;
}

Value BytecodeVM::runInternal(const std::shared_ptr<BytecodeModule>& module, bool preserveGlobals) {
    errors.clear();
    std::unordered_map<std::string, Value> preservedGlobals;
    if (preserveGlobals && currentModule) {
        for (size_t i = 0; i < globals.size() && i < currentModule->globals.size(); ++i) {
            preservedGlobals.emplace(currentModule->globals[i], globals[i]);
        }
    }
    currentModule = module;
    tasks.clear();
    runnableTasks.clear();
    nextTickFutures.clear();
    driveFutureRoots.clear();
    activeTaskId = static_cast<size_t>(-1);
    globals.clear();
    globalNameToSlot.clear();
    builtins.clear();
    gcRootEnv = std::make_shared<Environment>();
    environmentRoots.clear();
    environmentRoots.push_back(gcRootEnv.get());
    installBuiltins();

    try {
        initializeModule();
        if (preserveGlobals && !preservedGlobals.empty()) {
            for (const auto& entry : preservedGlobals) {
                auto slotIt = globalNameToSlot.find(entry.first);
                if (slotIt != globalNameToSlot.end()) {
                    globals[slotIt->second] = entry.second;
                }
            }
            syncRoots();
        }
        if (!currentModule || currentModule->functions.empty()) {
            return Value::null();
        }
        if (currentModule->entry.name.empty()) {
            return Value::null();
        }
        Value result = execute();
        runReadyTasks();
        return result;
    } catch (const ThrownValue& thrown) {
        errors.push_back(
            thrown.loc.filename + ":" + std::to_string(thrown.loc.line) + ":" + std::to_string(thrown.loc.column) +
            ": uncaught throw: " + toString(thrown.value));
        return Value::null();
    } catch (const std::runtime_error& ex) {
        errors.push_back(ex.what());
        return Value::null();
    }
}

const std::vector<std::string>& BytecodeVM::getErrors() const {
    return errors;
}

const RuntimeMetrics& BytecodeVM::getMetrics() const {
    return heap.metrics();
}

Heap& BytecodeVM::runtimeHeap() {
    return heap;
}

const Heap& BytecodeVM::runtimeHeap() const {
    return heap;
}

void BytecodeVM::initializeModule() {
    if (!currentModule) {
        throw std::runtime_error("bytecode VM has no module");
    }

    globals.resize(currentModule->globals.size(), Value::null());
    for (size_t i = 0; i < currentModule->globals.size(); ++i) {
        globalNameToSlot[currentModule->globals[i]] = static_cast<uint32_t>(i);
    }
    syncRoots();

    auto initIt = currentModule->functionMap.find("__module_init");
    if (initIt != currentModule->functionMap.end()) {
        Value initResult = callFunction(initIt->second, {}, currentModule->functions[initIt->second].loc);
        if (initResult.kind == Value::Kind::Object && initResult.objectValue &&
            initResult.objectValue->kind == ObjectKind::Future) {
            driveFuture(initResult, currentModule->functions[initIt->second].loc);
        }
        collectGarbage();
    }
}

void BytecodeVM::installBuiltins() {
    builtins["print"] = [](BytecodeVM&, const std::vector<Value>& args) {
        for (const auto& arg : args) {
            std::cout << toString(arg);
        }
        return Value::null();
    };

    builtins["println"] = [](BytecodeVM&, const std::vector<Value>& args) {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                std::cout << " ";
            }
            std::cout << toString(args[i]);
        }
        std::cout << '\n';
        return Value::null();
    };

    builtins["args"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("args expects no arguments");
        }
        std::vector<Value> values;
        values.reserve(g_scriptArgs.size());
        for (const auto& arg : g_scriptArgs) {
            values.push_back(Value::string(arg));
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(values)));
    };

    builtins["input"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() > 1) {
            throw std::runtime_error("input expects zero or one string argument");
        }
        if (args.size() == 1) {
            if (args[0].kind != Value::Kind::String) {
                throw std::runtime_error("input prompt must be a string");
            }
            std::cout << args[0].stringValue;
            std::cout.flush();
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            return Value::string("");
        }
        return Value::string(line);
    };

    builtins["len"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("len expects exactly one argument");
        }
        if (args[0].kind == Value::Kind::String) {
            return Value::integer(static_cast<int64_t>(args[0].stringValue.size()));
        }
        if (args[0].kind == Value::Kind::Object && args[0].objectValue &&
            args[0].objectValue->kind == ObjectKind::List) {
            auto* list = static_cast<ListObject*>(args[0].objectValue);
            return Value::integer(static_cast<int64_t>(list->elements.size()));
        }
        if (args[0].kind == Value::Kind::Object && args[0].objectValue &&
            args[0].objectValue->kind == ObjectKind::Tuple) {
            auto* tuple = static_cast<TupleObject*>(args[0].objectValue);
            return Value::integer(static_cast<int64_t>(tuple->elements.size()));
        }
        if (args[0].kind == Value::Kind::Object && args[0].objectValue &&
            args[0].objectValue->kind == ObjectKind::Map) {
            auto* map = static_cast<MapObject*>(args[0].objectValue);
            return Value::integer(static_cast<int64_t>(map->entries.size()));
        }
        if (args[0].kind == Value::Kind::Object && args[0].objectValue &&
            args[0].objectValue->kind == ObjectKind::Set) {
            auto* set = static_cast<SetObject*>(args[0].objectValue);
            return Value::integer(static_cast<int64_t>(set->elements.size()));
        }
        throw std::runtime_error("len expects a string, list, tuple, map, or set");
    };

    builtins["tuple"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        return Value::object(vm.runtimeHeap().allocate<TupleObject>(std::vector<Value>(args.begin(), args.end())));
    };

    builtins["map"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("map expects exactly zero arguments");
        }
        return Value::object(vm.runtimeHeap().allocate<MapObject>());
    };

    builtins["set"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("set expects exactly zero arguments");
        }
        return Value::object(vm.runtimeHeap().allocate<SetObject>());
    };

    builtins["map_get"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("map_get expects exactly two arguments");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Map) {
            throw std::runtime_error("map_get expects a map as its first argument");
        }
        auto* map = static_cast<MapObject*>(args[0].objectValue);
        for (const auto& entry : map->entries) {
            if (valuesEqual(entry.first, args[1])) {
                return entry.second;
            }
        }
        return Value::null();
    };

    builtins["map_set"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 3) {
            throw std::runtime_error("map_set expects exactly three arguments");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Map) {
            throw std::runtime_error("map_set expects a map as its first argument");
        }
        auto* map = static_cast<MapObject*>(args[0].objectValue);
        for (auto& entry : map->entries) {
            if (valuesEqual(entry.first, args[1])) {
                entry.second = args[2];
                return args[0];
            }
        }
        map->entries.push_back({args[1], args[2]});
        return args[0];
    };

    builtins["map_has"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("map_has expects exactly two arguments");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Map) {
            throw std::runtime_error("map_has expects a map as its first argument");
        }
        auto* map = static_cast<MapObject*>(args[0].objectValue);
        for (const auto& entry : map->entries) {
            if (valuesEqual(entry.first, args[1])) {
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    };

    builtins["map_remove"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("map_remove expects exactly two arguments");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Map) {
            throw std::runtime_error("map_remove expects a map as its first argument");
        }
        auto* map = static_cast<MapObject*>(args[0].objectValue);
        for (auto it = map->entries.begin(); it != map->entries.end(); ++it) {
            if (valuesEqual(it->first, args[1])) {
                map->entries.erase(it);
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    };

    builtins["map_keys"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("map_keys expects exactly one argument");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Map) {
            throw std::runtime_error("map_keys expects a map as its first argument");
        }
        auto* map = static_cast<MapObject*>(args[0].objectValue);
        std::vector<Value> keys;
        keys.reserve(map->entries.size());
        for (const auto& entry : map->entries) {
            keys.push_back(entry.first);
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(keys)));
    };

    builtins["map_values"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("map_values expects exactly one argument");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Map) {
            throw std::runtime_error("map_values expects a map as its first argument");
        }
        auto* map = static_cast<MapObject*>(args[0].objectValue);
        std::vector<Value> values;
        values.reserve(map->entries.size());
        for (const auto& entry : map->entries) {
            values.push_back(entry.second);
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(values)));
    };

    builtins["list_push"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("list_push expects exactly two arguments");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("list_push expects a list as its first argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        list->elements.push_back(args[1]);
        return args[0];
    };

    builtins["list_pop"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("list_pop expects exactly one argument");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("list_pop expects a list as its first argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        if (list->elements.empty()) {
            throw std::runtime_error("list_pop cannot pop from an empty list");
        }
        Value value = list->elements.back();
        list->elements.pop_back();
        return value;
    };

    builtins["list_contains"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("list_contains expects exactly two arguments");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("list_contains expects a list as its first argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        for (const auto& element : list->elements) {
            if (valuesEqual(element, args[1])) {
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    };

    builtins["list_first"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("list_first expects exactly one argument");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("list_first expects a list as its first argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        if (list->elements.empty()) {
            throw std::runtime_error("list_first cannot read from an empty list");
        }
        return list->elements.front();
    };

    builtins["list_last"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("list_last expects exactly one argument");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("list_last expects a list as its first argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        if (list->elements.empty()) {
            throw std::runtime_error("list_last cannot read from an empty list");
        }
        return list->elements.back();
    };

    builtins["set_add"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("set_add expects exactly two arguments");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Set) {
            throw std::runtime_error("set_add expects a set as its first argument");
        }
        auto* set = static_cast<SetObject*>(args[0].objectValue);
        for (const auto& element : set->elements) {
            if (valuesEqual(element, args[1])) {
                return args[0];
            }
        }
        set->elements.push_back(args[1]);
        return args[0];
    };

    builtins["set_has"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("set_has expects exactly two arguments");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Set) {
            throw std::runtime_error("set_has expects a set as its first argument");
        }
        auto* set = static_cast<SetObject*>(args[0].objectValue);
        for (const auto& element : set->elements) {
            if (valuesEqual(element, args[1])) {
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    };

    builtins["set_remove"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("set_remove expects exactly two arguments");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Set) {
            throw std::runtime_error("set_remove expects a set as its first argument");
        }
        auto* set = static_cast<SetObject*>(args[0].objectValue);
        for (auto it = set->elements.begin(); it != set->elements.end(); ++it) {
            if (valuesEqual(*it, args[1])) {
                set->elements.erase(it);
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    };

    builtins["set_values"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("set_values expects exactly one argument");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Set) {
            throw std::runtime_error("set_values expects a set as its first argument");
        }
        auto* set = static_cast<SetObject*>(args[0].objectValue);
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::vector<Value>(set->elements.begin(), set->elements.end())));
    };

    builtins["iter_range"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("iter_range expects exactly two arguments");
        }
        return Value::object(vm.runtimeHeap().allocate<RangeObject>(toInt(args[0]), toInt(args[1]), false));
    };

    builtins["iter_collect"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("iter_collect expects exactly one argument");
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(iterableValues(args[0])));
    };

    builtins["iter_take"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("iter_take expects exactly two arguments");
        }
        std::vector<Value> values = iterableValues(args[0]);
        const int64_t count = std::max<int64_t>(0, toInt(args[1]));
        if (static_cast<size_t>(count) < values.size()) {
            values.resize(static_cast<size_t>(count));
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(values)));
    };

    builtins["iter_skip"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("iter_skip expects exactly two arguments");
        }
        std::vector<Value> values = iterableValues(args[0]);
        const int64_t count = std::max<int64_t>(0, toInt(args[1]));
        if (static_cast<size_t>(count) >= values.size()) {
            values.clear();
        } else {
            values.erase(values.begin(), values.begin() + count);
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(values)));
    };

    builtins["iter_enumerate"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("iter_enumerate expects exactly one argument");
        }
        std::vector<Value> source = iterableValues(args[0]);
        std::vector<Value> enumerated;
        enumerated.reserve(source.size());
        for (size_t i = 0; i < source.size(); ++i) {
            std::vector<Value> pair;
            pair.push_back(Value::integer(static_cast<int64_t>(i)));
            pair.push_back(source[i]);
            enumerated.push_back(Value::object(vm.runtimeHeap().allocate<TupleObject>(std::move(pair))));
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(enumerated)));
    };

    builtins["iter_first"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("iter_first expects exactly one argument");
        }
        std::vector<Value> values = iterableValues(args[0]);
        if (values.empty()) {
            throw std::runtime_error("iter_first cannot read from an empty iterable");
        }
        return values.front();
    };

    builtins["iter_last"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("iter_last expects exactly one argument");
        }
        std::vector<Value> values = iterableValues(args[0]);
        if (values.empty()) {
            throw std::runtime_error("iter_last cannot read from an empty iterable");
        }
        return values.back();
    };

    builtins["iter_map"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("iter_map expects exactly two arguments");
        }
        std::vector<Value> source = iterableValues(args[0]);
        std::vector<Value> mapped;
        mapped.reserve(source.size());
        SourceLocation loc;
        for (const auto& item : source) {
            mapped.push_back(vm.callValue(args[1], {item}, loc));
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(mapped)));
    };

    builtins["iter_filter"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("iter_filter expects exactly two arguments");
        }
        std::vector<Value> source = iterableValues(args[0]);
        std::vector<Value> filtered;
        SourceLocation loc;
        for (const auto& item : source) {
            if (isTruthy(vm.callValue(args[1], {item}, loc))) {
                filtered.push_back(item);
            }
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(filtered)));
    };

    builtins["iter_fold"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 3) {
            throw std::runtime_error("iter_fold expects exactly three arguments");
        }
        std::vector<Value> source = iterableValues(args[0]);
        Value accumulator = args[1];
        SourceLocation loc;
        for (const auto& item : source) {
            accumulator = vm.callValue(args[2], {accumulator, item}, loc);
        }
        return accumulator;
    };

    builtins["math_abs"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("math_abs expects exactly one argument");
        }
        return Value::floating(std::fabs(toNumber(args[0])));
    };

    builtins["math_min"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("math_min expects exactly two arguments");
        }
        return Value::floating(std::min(toNumber(args[0]), toNumber(args[1])));
    };

    builtins["math_max"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("math_max expects exactly two arguments");
        }
        return Value::floating(std::max(toNumber(args[0]), toNumber(args[1])));
    };

    builtins["math_clamp"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 3) {
            throw std::runtime_error("math_clamp expects exactly three arguments");
        }
        const double value = toNumber(args[0]);
        const double low = toNumber(args[1]);
        const double high = toNumber(args[2]);
        return Value::floating(std::clamp(value, low, high));
    };

    builtins["math_floor"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("math_floor expects exactly one argument");
        }
        return Value::integer(static_cast<int64_t>(std::floor(toNumber(args[0]))));
    };

    builtins["math_ceil"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("math_ceil expects exactly one argument");
        }
        return Value::integer(static_cast<int64_t>(std::ceil(toNumber(args[0]))));
    };

    builtins["math_round"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("math_round expects exactly one argument");
        }
        return Value::integer(static_cast<int64_t>(std::llround(toNumber(args[0]))));
    };

    builtins["math_sqrt"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("math_sqrt expects exactly one argument");
        }
        return Value::floating(std::sqrt(toNumber(args[0])));
    };

    builtins["math_sin"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("math_sin expects exactly one argument");
        }
        return Value::floating(std::sin(toNumber(args[0])));
    };

    builtins["math_cos"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("math_cos expects exactly one argument");
        }
        return Value::floating(std::cos(toNumber(args[0])));
    };

    builtins["math_tan"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("math_tan expects exactly one argument");
        }
        return Value::floating(std::tan(toNumber(args[0])));
    };

    builtins["fs_read_text"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("fs_read_text expects exactly one string argument");
        }
        std::ifstream input(args[0].stringValue, std::ios::binary);
        if (!input) {
            throw std::runtime_error("fs_read_text failed to open '" + args[0].stringValue + "'");
        }
        return Value::string(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
    };

    builtins["fs_write_text"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("fs_write_text expects a path string and a text string");
        }
        std::ofstream output(args[0].stringValue, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("fs_write_text failed to open '" + args[0].stringValue + "'");
        }
        output << args[1].stringValue;
        return Value::boolean(static_cast<bool>(output));
    };

    builtins["fs_append_text"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("fs_append_text expects a path string and a text string");
        }
        std::ofstream output(args[0].stringValue, std::ios::binary | std::ios::app);
        if (!output) {
            throw std::runtime_error("fs_append_text failed to open '" + args[0].stringValue + "'");
        }
        output << args[1].stringValue;
        return Value::boolean(static_cast<bool>(output));
    };

    builtins["fs_exists"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("fs_exists expects exactly one string argument");
        }
        std::error_code ec;
        return Value::boolean(std::filesystem::exists(args[0].stringValue, ec) && !ec);
    };

    builtins["fs_create_dir"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("fs_create_dir expects exactly one string argument");
        }
        std::error_code ec;
        const bool created = std::filesystem::create_directories(args[0].stringValue, ec);
        if (ec) {
            throw std::runtime_error("fs_create_dir failed for '" + args[0].stringValue + "'");
        }
        return Value::boolean(created || std::filesystem::is_directory(args[0].stringValue));
    };

    builtins["fs_remove_file"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("fs_remove_file expects exactly one string argument");
        }
        std::error_code ec;
        const bool removed = std::filesystem::remove(args[0].stringValue, ec);
        if (ec) {
            throw std::runtime_error("fs_remove_file failed for '" + args[0].stringValue + "'");
        }
        return Value::boolean(removed);
    };

    builtins["fs_remove_dir"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("fs_remove_dir expects exactly one string argument");
        }
        std::error_code ec;
        const bool removed = std::filesystem::remove(args[0].stringValue, ec);
        if (ec) {
            throw std::runtime_error("fs_remove_dir failed for '" + args[0].stringValue + "'");
        }
        return Value::boolean(removed);
    };

    builtins["fs_list_dir"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("fs_list_dir expects exactly one string argument");
        }
        std::error_code ec;
        std::vector<std::string> entries;
        for (const auto& entry : std::filesystem::directory_iterator(args[0].stringValue, ec)) {
            if (ec) {
                break;
            }
            entries.push_back(genericPathString(entry.path()));
        }
        if (ec) {
            throw std::runtime_error("fs_list_dir failed for '" + args[0].stringValue + "'");
        }
        std::sort(entries.begin(), entries.end());
        std::vector<Value> values;
        values.reserve(entries.size());
        for (const auto& entry : entries) {
            values.push_back(Value::string(entry));
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(values)));
    };

    builtins["path_join"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("path_join expects two string arguments");
        }
        return Value::string(genericPathString(std::filesystem::path(args[0].stringValue) / args[1].stringValue));
    };

    builtins["path_normalize"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("path_normalize expects exactly one string argument");
        }
        return Value::string(genericPathString(std::filesystem::path(args[0].stringValue)));
    };

    builtins["path_basename"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("path_basename expects exactly one string argument");
        }
        return Value::string(std::filesystem::path(args[0].stringValue).filename().generic_string());
    };

    builtins["path_dirname"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("path_dirname expects exactly one string argument");
        }
        return Value::string(std::filesystem::path(args[0].stringValue).parent_path().generic_string());
    };

    builtins["path_extension"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("path_extension expects exactly one string argument");
        }
        return Value::string(std::filesystem::path(args[0].stringValue).extension().generic_string());
    };

    builtins["path_stem"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("path_stem expects exactly one string argument");
        }
        return Value::string(std::filesystem::path(args[0].stringValue).stem().generic_string());
    };

    builtins["time_now_ms"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("time_now_ms expects no arguments");
        }
        const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
        return Value::integer(now.time_since_epoch().count());
    };

    builtins["time_now_seconds"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("time_now_seconds expects no arguments");
        }
        const auto now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
        return Value::integer(now.time_since_epoch().count());
    };

    builtins["time_sleep_ms"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("time_sleep_ms expects exactly one integer argument");
        }
        const int64_t duration = toInt(args[0]);
        if (duration < 0) {
            throw std::runtime_error("time_sleep_ms expects a non-negative duration");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
        return Value::null();
    };

    builtins["duration_millis"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("duration_millis expects exactly one integer argument");
        }
        return Value::integer(toInt(args[0]));
    };

    builtins["duration_seconds"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("duration_seconds expects exactly one integer argument");
        }
        return Value::integer(toInt(args[0]) * 1000);
    };

    builtins["duration_minutes"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("duration_minutes expects exactly one integer argument");
        }
        return Value::integer(toInt(args[0]) * 60 * 1000);
    };

    builtins["duration_hours"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("duration_hours expects exactly one integer argument");
        }
        return Value::integer(toInt(args[0]) * 60 * 60 * 1000);
    };

    builtins["duration_days"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("duration_days expects exactly one integer argument");
        }
        return Value::integer(toInt(args[0]) * 24 * 60 * 60 * 1000);
    };

    builtins["duration_in_millis"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("duration_in_millis expects exactly one integer argument");
        }
        return Value::integer(toInt(args[0]));
    };

    builtins["duration_in_seconds"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("duration_in_seconds expects exactly one integer argument");
        }
        return Value::integer(toInt(args[0]) / 1000);
    };

    builtins["date_from_parts"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 3) {
            throw std::runtime_error("date_from_parts expects year, month, and day");
        }
        const int year = static_cast<int>(toInt(args[0]));
        const int month = static_cast<int>(toInt(args[1]));
        const int day = static_cast<int>(toInt(args[2]));
        if (month < 1 || month > 12 || day < 1 || day > 31) {
            throw std::runtime_error("date_from_parts received an out-of-range date component");
        }
        return Value::string(formatDateParts(year, month, day));
    };

    builtins["date_year"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("date_year expects exactly one string argument");
        }
        int year = 0;
        int month = 0;
        int day = 0;
        parseIsoDate(args[0].stringValue, year, month, day);
        return Value::integer(year);
    };

    builtins["date_month"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("date_month expects exactly one string argument");
        }
        int year = 0;
        int month = 0;
        int day = 0;
        parseIsoDate(args[0].stringValue, year, month, day);
        return Value::integer(month);
    };

    builtins["date_day"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("date_day expects exactly one string argument");
        }
        int year = 0;
        int month = 0;
        int day = 0;
        parseIsoDate(args[0].stringValue, year, month, day);
        return Value::integer(day);
    };

    builtins["date_today_utc"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("date_today_utc expects no arguments");
        }
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        const std::tm current = utcTime(now);
        return Value::string(formatDateParts(current.tm_year + 1900, current.tm_mon + 1, current.tm_mday));
    };

    builtins["env_get"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("env_get expects exactly one string argument");
        }
        const char* value = std::getenv(args[0].stringValue.c_str());
        return value ? Value::string(value) : Value::null();
    };

    builtins["env_set"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("env_set expects a name string and a value string");
        }
#if defined(_WIN32)
        const int result = _putenv_s(args[0].stringValue.c_str(), args[1].stringValue.c_str());
#else
        const int result = setenv(args[0].stringValue.c_str(), args[1].stringValue.c_str(), 1);
#endif
        return Value::boolean(result == 0);
    };

    builtins["env_has"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("env_has expects exactly one string argument");
        }
        return Value::boolean(std::getenv(args[0].stringValue.c_str()) != nullptr);
    };

    builtins["env_args"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("env_args expects no arguments");
        }
        std::vector<Value> values;
        values.reserve(g_processArgs.size());
        for (const auto& arg : g_processArgs) {
            values.push_back(Value::string(arg));
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(values)));
    };

    builtins["env_cwd"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("env_cwd expects no arguments");
        }
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (ec) {
            throw std::runtime_error("env_cwd failed");
        }
        return Value::string(genericPathString(cwd));
    };

    builtins["process_pid"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("process_pid expects no arguments");
        }
#if defined(_WIN32)
        return Value::integer(static_cast<int64_t>(_getpid()));
#else
        return Value::integer(static_cast<int64_t>(getpid()));
#endif
    };

    builtins["process_run"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("process_run expects exactly one string argument");
        }
        return Value::integer(static_cast<int64_t>(std::system(args[0].stringValue.c_str())));
    };

    builtins["process_exit"] = [](BytecodeVM&, const std::vector<Value>& args) -> Value {
        if (args.size() != 1) {
            throw std::runtime_error("process_exit expects exactly one integer argument");
        }
        std::exit(static_cast<int>(toInt(args[0])));
    };

    builtins["io_open_reader"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("io_open_reader expects exactly one string argument");
        }
        auto* stream = vm.runtimeHeap().allocate<TextStreamObject>(args[0].stringValue, TextStreamObject::Mode::Reader);
        stream->input = std::make_unique<std::ifstream>(args[0].stringValue, std::ios::binary);
        if (!stream->input || !(*stream->input)) {
            throw std::runtime_error("io_open_reader failed to open '" + args[0].stringValue + "'");
        }
        return Value::object(stream);
    };

    builtins["io_open_writer"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("io_open_writer expects exactly one string argument");
        }
        auto* stream = vm.runtimeHeap().allocate<TextStreamObject>(args[0].stringValue, TextStreamObject::Mode::Writer);
        stream->output = std::make_unique<std::ofstream>(args[0].stringValue, std::ios::binary | std::ios::trunc);
        if (!stream->output || !(*stream->output)) {
            throw std::runtime_error("io_open_writer failed to open '" + args[0].stringValue + "'");
        }
        return Value::object(stream);
    };

    builtins["io_open_appender"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("io_open_appender expects exactly one string argument");
        }
        auto* stream = vm.runtimeHeap().allocate<TextStreamObject>(args[0].stringValue, TextStreamObject::Mode::Appender);
        stream->output = std::make_unique<std::ofstream>(args[0].stringValue, std::ios::binary | std::ios::app);
        if (!stream->output || !(*stream->output)) {
            throw std::runtime_error("io_open_appender failed to open '" + args[0].stringValue + "'");
        }
        return Value::object(stream);
    };

    builtins["io_read_line"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TextStream) {
            throw std::runtime_error("io_read_line expects exactly one TextStream argument");
        }
        auto* stream = static_cast<TextStreamObject*>(args[0].objectValue);
        if (stream->closed || !stream->input) {
            throw std::runtime_error("io_read_line expects an open reader stream");
        }
        std::string line;
        if (!std::getline(*stream->input, line)) {
            return Value::null();
        }
        return Value::string(line);
    };

    builtins["io_read_all"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TextStream) {
            throw std::runtime_error("io_read_all expects exactly one TextStream argument");
        }
        auto* stream = static_cast<TextStreamObject*>(args[0].objectValue);
        if (stream->closed || !stream->input) {
            throw std::runtime_error("io_read_all expects an open reader stream");
        }
        std::ostringstream contents;
        contents << stream->input->rdbuf();
        return Value::string(contents.str());
    };

    builtins["io_write_text"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TextStream || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("io_write_text expects a TextStream and a string");
        }
        auto* stream = static_cast<TextStreamObject*>(args[0].objectValue);
        if (stream->closed || !stream->output) {
            throw std::runtime_error("io_write_text expects an open writer stream");
        }
        (*stream->output) << args[1].stringValue;
        return Value::null();
    };

    builtins["io_write_line"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TextStream || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("io_write_line expects a TextStream and a string");
        }
        auto* stream = static_cast<TextStreamObject*>(args[0].objectValue);
        if (stream->closed || !stream->output) {
            throw std::runtime_error("io_write_line expects an open writer stream");
        }
        (*stream->output) << args[1].stringValue << '\n';
        return Value::null();
    };

    builtins["io_flush_stream"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TextStream) {
            throw std::runtime_error("io_flush_stream expects exactly one TextStream argument");
        }
        auto* stream = static_cast<TextStreamObject*>(args[0].objectValue);
        if (stream->closed || !stream->output) {
            throw std::runtime_error("io_flush_stream expects an open writer stream");
        }
        stream->output->flush();
        return Value::null();
    };

    builtins["io_close_stream"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TextStream) {
            throw std::runtime_error("io_close_stream expects exactly one TextStream argument");
        }
        auto* stream = static_cast<TextStreamObject*>(args[0].objectValue);
        if (!stream->closed) {
            if (stream->output) {
                stream->output->flush();
                stream->output->close();
                stream->output.reset();
            }
            if (stream->input) {
                stream->input->close();
                stream->input.reset();
            }
            stream->closed = true;
        }
        return Value::null();
    };

    builtins["json_stringify"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("json_stringify expects exactly one argument");
        }
        return Value::string(jsonStringifyValue(args[0]));
    };

    builtins["json_parse"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("json_parse expects exactly one string argument");
        }
        JsonParser parser(vm, args[0].stringValue);
        return parser.parse();
    };

    builtins["bytes_from_string"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("bytes_from_string expects exactly one string argument");
        }
        std::vector<uint8_t> data(args[0].stringValue.begin(), args[0].stringValue.end());
        return Value::object(vm.runtimeHeap().allocate<BytesObject>(std::move(data)));
    };

    builtins["bytes_to_string"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Bytes) {
            throw std::runtime_error("bytes_to_string expects exactly one Bytes argument");
        }
        auto* bytes = static_cast<BytesObject*>(args[0].objectValue);
        return Value::string(std::string(bytes->data.begin(), bytes->data.end()));
    };

    builtins["bytes_from_int_list"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("bytes_from_int_list expects exactly one List<Int> argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        std::vector<uint8_t> data;
        data.reserve(list->elements.size());
        for (const auto& element : list->elements) {
            const int64_t byte = toInt(element);
            if (byte < 0 || byte > 255) {
                throw std::runtime_error("bytes_from_int_list expects values in the range 0..255");
            }
            data.push_back(static_cast<uint8_t>(byte));
        }
        return Value::object(vm.runtimeHeap().allocate<BytesObject>(std::move(data)));
    };

    builtins["bytes_to_int_list"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Bytes) {
            throw std::runtime_error("bytes_to_int_list expects exactly one Bytes argument");
        }
        auto* bytes = static_cast<BytesObject*>(args[0].objectValue);
        std::vector<Value> values;
        values.reserve(bytes->data.size());
        for (uint8_t byte : bytes->data) {
            values.push_back(Value::integer(byte));
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(values)));
    };

    builtins["bytes_length"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Bytes) {
            throw std::runtime_error("bytes_length expects exactly one Bytes argument");
        }
        auto* bytes = static_cast<BytesObject*>(args[0].objectValue);
        return Value::integer(static_cast<int64_t>(bytes->data.size()));
    };

    builtins["random_int"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("random_int expects low and high int arguments");
        }
        const int64_t low = toInt(args[0]);
        const int64_t high = toInt(args[1]);
        if (low > high) {
            throw std::runtime_error("random_int expects low <= high");
        }
        std::uniform_int_distribution<int64_t> dist(low, high);
        return Value::integer(dist(g_prng));
    };

    builtins["random_float"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("random_float expects no arguments");
        }
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return Value::floating(dist(g_prng));
    };

    builtins["random_choice"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("random_choice expects exactly one List argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        if (list->elements.empty()) {
            throw std::runtime_error("random_choice expects a non-empty list");
        }
        std::uniform_int_distribution<size_t> dist(0, list->elements.size() - 1);
        return list->elements[dist(g_prng)];
    };

    builtins["random_shuffle"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("random_shuffle expects exactly one List argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        std::vector<Value> shuffled = list->elements;
        std::shuffle(shuffled.begin(), shuffled.end(), g_prng);
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(shuffled)));
    };

    builtins["random_bytes"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("random_bytes expects exactly one Int argument");
        }
        const int64_t count = toInt(args[0]);
        if (count < 0) {
            throw std::runtime_error("random_bytes expects a non-negative count");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(count));
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& byte : bytes) {
            byte = static_cast<uint8_t>(dist(g_prng));
        }
        return Value::object(vm.runtimeHeap().allocate<BytesObject>(std::move(bytes)));
    };

    builtins["hash_fnv1a64_text"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("hash_fnv1a64_text expects exactly one string argument");
        }
        const auto& text = args[0].stringValue;
        return Value::string(hexEncode64(fnv1a64(reinterpret_cast<const uint8_t*>(text.data()), text.size())));
    };

    builtins["hash_fnv1a64_bytes"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Bytes) {
            throw std::runtime_error("hash_fnv1a64_bytes expects exactly one Bytes argument");
        }
        auto* bytes = static_cast<BytesObject*>(args[0].objectValue);
        return Value::string(hexEncode64(fnv1a64(bytes->data.data(), bytes->data.size())));
    };

    builtins["hash_sha256_text"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("hash_sha256_text expects exactly one string argument");
        }
        const auto& text = args[0].stringValue;
        return Value::string(sha256Hex(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    };

    builtins["hash_sha256_bytes"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::Bytes) {
            throw std::runtime_error("hash_sha256_bytes expects exactly one Bytes argument");
        }
        auto* bytes = static_cast<BytesObject*>(args[0].objectValue);
        return Value::string(sha256Hex(bytes->data.data(), bytes->data.size()));
    };

    builtins["crypto_random_bytes"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("crypto_random_bytes expects exactly one Int argument");
        }
        const int64_t count = toInt(args[0]);
        if (count < 0) {
            throw std::runtime_error("crypto_random_bytes expects a non-negative count");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(count));
        std::random_device rd;
        for (auto& byte : bytes) {
            byte = static_cast<uint8_t>(rd() & 0xffu);
        }
        return Value::object(vm.runtimeHeap().allocate<BytesObject>(std::move(bytes)));
    };

    builtins["crypto_random_hex"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("crypto_random_hex expects exactly one Int argument");
        }
        const int64_t count = toInt(args[0]);
        if (count < 0) {
            throw std::runtime_error("crypto_random_hex expects a non-negative count");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(count));
        std::random_device rd;
        for (auto& byte : bytes) {
            byte = static_cast<uint8_t>(rd() & 0xffu);
        }
        return Value::string(hexEncode(bytes));
    };

    builtins["net_listen_tcp"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("net_listen_tcp expects a host string and port int");
        }
        ensureSocketsInitialized();
        const std::string host = args[0].stringValue;
        const std::string portString = std::to_string(toInt(args[1]));

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;

        addrinfo* info = nullptr;
        const int rc = getaddrinfo(host.empty() ? nullptr : host.c_str(), portString.c_str(), &hints, &info);
        if (rc != 0 || !info) {
            throw std::runtime_error("net_listen_tcp failed to resolve address");
        }

        NativeSocket listener = kInvalidSocket;
        int boundPort = 0;
        for (addrinfo* current = info; current != nullptr; current = current->ai_next) {
            listener = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (listener == kInvalidSocket) {
                continue;
            }
            int reuse = 1;
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            if (bind(listener, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0 &&
                listen(listener, 4) == 0) {
                sockaddr_in addr{};
                int addrLen = sizeof(addr);
                if (getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) == 0) {
                    boundPort = ntohs(addr.sin_port);
                }
                break;
            }
            closeNativeSocket(static_cast<intptr_t>(listener));
            listener = kInvalidSocket;
        }
        freeaddrinfo(info);
        if (listener == kInvalidSocket) {
            throw std::runtime_error("net_listen_tcp failed to bind listener");
        }
        return Value::object(vm.runtimeHeap().allocate<TcpListenerObject>(static_cast<intptr_t>(listener), boundPort));
    };

    builtins["net_listener_port"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TcpListener) {
            throw std::runtime_error("net_listener_port expects exactly one Listener argument");
        }
        auto* listener = static_cast<TcpListenerObject*>(args[0].objectValue);
        return Value::integer(listener->port);
    };

    builtins["net_accept"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TcpListener) {
            throw std::runtime_error("net_accept expects exactly one Listener argument");
        }
        auto* listener = static_cast<TcpListenerObject*>(args[0].objectValue);
        if (listener->closed) {
            throw std::runtime_error("net_accept expects an open listener");
        }
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        NativeSocket client = accept(toNativeSocket(listener->handle), reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (client == kInvalidSocket) {
            throw std::runtime_error("net_accept failed");
        }
        return Value::object(vm.runtimeHeap().allocate<TcpSocketObject>(static_cast<intptr_t>(client)));
    };

    builtins["net_connect_tcp"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("net_connect_tcp expects a host string and port int");
        }
        ensureSocketsInitialized();
        const std::string host = args[0].stringValue;
        const std::string portString = std::to_string(toInt(args[1]));

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* info = nullptr;
        const int rc = getaddrinfo(host.c_str(), portString.c_str(), &hints, &info);
        if (rc != 0 || !info) {
            throw std::runtime_error("net_connect_tcp failed to resolve address");
        }

        NativeSocket socketHandle = kInvalidSocket;
        for (addrinfo* current = info; current != nullptr; current = current->ai_next) {
            socketHandle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (socketHandle == kInvalidSocket) {
                continue;
            }
            if (connect(socketHandle, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
                break;
            }
            closeNativeSocket(static_cast<intptr_t>(socketHandle));
            socketHandle = kInvalidSocket;
        }
        freeaddrinfo(info);
        if (socketHandle == kInvalidSocket) {
            throw std::runtime_error("net_connect_tcp failed to connect");
        }
        return Value::object(vm.runtimeHeap().allocate<TcpSocketObject>(static_cast<intptr_t>(socketHandle)));
    };

    builtins["net_send_text"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[1].kind != Value::Kind::String ||
            args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TcpSocket) {
            throw std::runtime_error("net_send_text expects a Socket and a string");
        }
        auto* socketObject = static_cast<TcpSocketObject*>(args[0].objectValue);
        if (socketObject->closed) {
            throw std::runtime_error("net_send_text expects an open socket");
        }
        const std::string& text = args[1].stringValue;
        int total = 0;
        while (total < static_cast<int>(text.size())) {
            const int sent = send(toNativeSocket(socketObject->handle), text.data() + total, static_cast<int>(text.size()) - total, 0);
            if (sent <= 0) {
                throw std::runtime_error("net_send_text failed");
            }
            total += sent;
        }
        return Value::integer(total);
    };

    builtins["net_receive_text"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TcpSocket) {
            throw std::runtime_error("net_receive_text expects exactly one Socket argument");
        }
        auto* socketObject = static_cast<TcpSocketObject*>(args[0].objectValue);
        if (socketObject->closed) {
            throw std::runtime_error("net_receive_text expects an open socket");
        }
        std::string result;
        char buffer[1024];
        while (true) {
            const int received = recv(toNativeSocket(socketObject->handle), buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received < 0) {
                throw std::runtime_error("net_receive_text failed");
            }
            if (received == 0) {
                break;
            }
            result.append(buffer, static_cast<size_t>(received));
            const auto headerEnd = result.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                const std::string loweredHeaders = lowerAscii(result.substr(0, headerEnd));
                const auto contentLengthPos = loweredHeaders.find("content-length:");
                if (contentLengthPos == std::string::npos) {
                    break;
                }
                const auto lineEnd = loweredHeaders.find("\r\n", contentLengthPos);
                if (lineEnd == std::string::npos) {
                    continue;
                }
                const auto valueStart = contentLengthPos + std::strlen("content-length:");
                const std::string lengthText = trimAscii(result.substr(valueStart, lineEnd - valueStart));
                size_t expectedBodySize = 0;
                try {
                    expectedBodySize = static_cast<size_t>(std::stoull(lengthText));
                } catch (...) {
                    throw std::runtime_error("net_receive_text failed to parse content-length");
                }
                const size_t bodySize = result.size() - (headerEnd + 4);
                if (bodySize >= expectedBodySize) {
                    break;
                }
            }
            if (received < static_cast<int>(sizeof(buffer))) {
                break;
            }
        }
        return Value::string(result);
    };

    builtins["net_close_socket"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TcpSocket) {
            throw std::runtime_error("net_close_socket expects exactly one Socket argument");
        }
        auto* socketObject = static_cast<TcpSocketObject*>(args[0].objectValue);
        if (!socketObject->closed) {
            closeNativeSocket(socketObject->handle);
            socketObject->closed = true;
            socketObject->handle = -1;
        }
        return Value::null();
    };

    builtins["net_close_listener"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::TcpListener) {
            throw std::runtime_error("net_close_listener expects exactly one Listener argument");
        }
        auto* listener = static_cast<TcpListenerObject*>(args[0].objectValue);
        if (!listener->closed) {
            closeNativeSocket(listener->handle);
            listener->closed = true;
            listener->handle = -1;
        }
        return Value::null();
    };

    builtins["net_url_encode_component"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("net_url_encode_component expects exactly one string argument");
        }
        return Value::string(urlEncodeComponent(args[0].stringValue));
    };

    builtins["net_url_decode_component"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("net_url_decode_component expects exactly one string argument");
        }
        return Value::string(urlDecodeComponent(args[0].stringValue));
    };

    builtins["net_url_build"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 5 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String ||
            args[3].kind != Value::Kind::String || args[4].kind != Value::Kind::String) {
            throw std::runtime_error("net_url_build expects scheme, host, port, path, query");
        }
        std::string url = args[0].stringValue + "://" + args[1].stringValue;
        const int64_t port = toInt(args[2]);
        if (port > 0) {
            url += ":" + std::to_string(port);
        }
        if (!args[3].stringValue.empty()) {
            if (args[3].stringValue.front() != '/') {
                url += "/";
            }
            url += args[3].stringValue;
        }
        if (!args[4].stringValue.empty()) {
            url += "?" + args[4].stringValue;
        }
        return Value::string(url);
    };

    builtins["net_url_parse"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("net_url_parse expects exactly one string argument");
        }
        const std::string& url = args[0].stringValue;
        const size_t schemePos = url.find("://");
        if (schemePos == std::string::npos) {
            throw std::runtime_error("net_url_parse expected scheme://host form");
        }
        const std::string scheme = url.substr(0, schemePos);
        size_t hostStart = schemePos + 3;
        size_t pathStart = url.find('/', hostStart);
        size_t queryStart = url.find('?', hostStart);
        size_t authorityEnd = std::min(pathStart == std::string::npos ? url.size() : pathStart,
                                       queryStart == std::string::npos ? url.size() : queryStart);
        const std::string authority = url.substr(hostStart, authorityEnd - hostStart);
        const size_t colon = authority.rfind(':');
        std::string host = authority;
        int64_t port = 0;
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = std::stoll(authority.substr(colon + 1));
        }
        std::string path = pathStart == std::string::npos ? "/" :
                           url.substr(pathStart, (queryStart == std::string::npos ? url.size() : queryStart) - pathStart);
        std::string query = queryStart == std::string::npos ? "" : url.substr(queryStart + 1);
        auto* map = vm.runtimeHeap().allocate<MapObject>();
        map->entries.push_back({Value::string("scheme"), Value::string(scheme)});
        map->entries.push_back({Value::string("host"), Value::string(host)});
        map->entries.push_back({Value::string("port"), Value::integer(port)});
        map->entries.push_back({Value::string("path"), Value::string(path)});
        map->entries.push_back({Value::string("query"), Value::string(query)});
        return Value::object(map);
    };

    builtins["ui_backend_name"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("ui_backend_name expects no arguments");
        }
        return Value::string(ui::backendName());
    };

    builtins["ui_backend_available"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("ui_backend_available expects no arguments");
        }
        return Value::boolean(ui::backendAvailable());
    };

    builtins["ui_app_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("ui_app_new expects exactly one string argument");
        }
        return Value::object(vm.runtimeHeap().allocate<UiAppObject>(args[0].stringValue, ui::backendAvailable()));
    };

    builtins["ui_app_run"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiApp) {
            throw std::runtime_error("ui_app_run expects exactly one UiApp argument");
        }
        auto* app = static_cast<UiAppObject*>(args[0].objectValue);
        return Value::integer(ui::runApp(vm, *app));
    };

    builtins["ui_app_name"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiApp) {
            throw std::runtime_error("ui_app_name expects exactly one UiApp argument");
        }
        return Value::string(static_cast<UiAppObject*>(args[0].objectValue)->name);
    };

    builtins["ui_window_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 5 || args[1].kind != Value::Kind::String ||
            args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiApp ||
            args[4].kind != Value::Kind::Object || !args[4].objectValue ||
            args[4].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_window_new expects app, title, width, height, and content");
        }
        auto* window = vm.runtimeHeap().allocate<UiWindowObject>(
            args[1].stringValue,
            static_cast<int>(toInt(args[2])),
            static_cast<int>(toInt(args[3])));
        window->app = args[0];
        window->content = args[4];
        auto* app = static_cast<UiAppObject*>(args[0].objectValue);
        Value windowValue = Value::object(window);
        app->windows.push_back(windowValue);
        return windowValue;
    };

    builtins["ui_window_show"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow) {
            throw std::runtime_error("ui_window_show expects exactly one UiWindow argument");
        }
        ui::showWindow(*static_cast<UiWindowObject*>(args[0].objectValue));
        return Value::null();
    };

    builtins["ui_window_close"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow) {
            throw std::runtime_error("ui_window_close expects exactly one UiWindow argument");
        }
        auto* window = static_cast<UiWindowObject*>(args[0].objectValue);
        ui::closeWindow(vm, *window);
        return Value::null();
    };

    builtins["ui_window_is_visible"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow) {
            throw std::runtime_error("ui_window_is_visible expects exactly one UiWindow argument");
        }
        return Value::boolean(static_cast<UiWindowObject*>(args[0].objectValue)->visible);
    };

    builtins["ui_window_get_title"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow) {
            throw std::runtime_error("ui_window_get_title expects exactly one UiWindow argument");
        }
        return Value::string(static_cast<UiWindowObject*>(args[0].objectValue)->title);
    };

    builtins["ui_window_set_title"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[1].kind != Value::Kind::String ||
            args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow) {
            throw std::runtime_error("ui_window_set_title expects a UiWindow and a string");
        }
        auto* window = static_cast<UiWindowObject*>(args[0].objectValue);
        window->title = args[1].stringValue;
        ui::setWindowTitle(*window);
        return Value::null();
    };

    builtins["ui_window_get_width"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow) {
            throw std::runtime_error("ui_window_get_width expects exactly one UiWindow argument");
        }
        return Value::integer(static_cast<UiWindowObject*>(args[0].objectValue)->width);
    };

    builtins["ui_window_get_height"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow) {
            throw std::runtime_error("ui_window_get_height expects exactly one UiWindow argument");
        }
        return Value::integer(static_cast<UiWindowObject*>(args[0].objectValue)->height);
    };

    builtins["ui_window_set_size"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 3 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow) {
            throw std::runtime_error("ui_window_set_size expects a UiWindow and two Int values");
        }
        auto* window = static_cast<UiWindowObject*>(args[0].objectValue);
        window->width = static_cast<int>(toInt(args[1]));
        window->height = static_cast<int>(toInt(args[2]));
        ui::setWindowSize(*window);
        return Value::null();
    };

    builtins["ui_window_set_content"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow ||
            args[1].kind != Value::Kind::Object || !args[1].objectValue ||
            args[1].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_window_set_content expects a UiWindow and a UiView");
        }
        auto* window = static_cast<UiWindowObject*>(args[0].objectValue);
        window->content = args[1];
        ui::setWindowContent(vm, *window);
        return Value::null();
    };

    builtins["ui_window_on_close"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow) {
            throw std::runtime_error("ui_window_on_close expects a UiWindow and a callback");
        }
        static_cast<UiWindowObject*>(args[0].objectValue)->onClose = args[1];
        return Value::null();
    };

    builtins["ui_window_apply_theme"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiWindow ||
            args[1].kind != Value::Kind::Object || !args[1].objectValue ||
            args[1].objectValue->kind != ObjectKind::UiTheme) {
            throw std::runtime_error("ui_window_apply_theme expects a UiWindow and a UiTheme");
        }
        auto* window = static_cast<UiWindowObject*>(args[0].objectValue);
        window->theme = args[1];
        ui::applyTheme(*window);
        return Value::null();
    };

    builtins["ui_label_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("ui_label_new expects exactly one string argument");
        }
        auto* view = vm.runtimeHeap().allocate<UiViewObject>(UiViewKind::Label);
        view->text = args[0].stringValue;
        return Value::object(view);
    };

    builtins["ui_button_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("ui_button_new expects exactly one string argument");
        }
        auto* view = vm.runtimeHeap().allocate<UiViewObject>(UiViewKind::Button);
        view->text = args[0].stringValue;
        return Value::object(view);
    };

    builtins["ui_text_field_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("ui_text_field_new expects text and placeholder strings");
        }
        auto* view = vm.runtimeHeap().allocate<UiViewObject>(UiViewKind::TextField);
        view->text = args[0].stringValue;
        view->placeholder = args[1].stringValue;
        return Value::object(view);
    };

    builtins["ui_text_area_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("ui_text_area_new expects text and placeholder strings");
        }
        auto* view = vm.runtimeHeap().allocate<UiViewObject>(UiViewKind::TextArea);
        view->text = args[0].stringValue;
        view->placeholder = args[1].stringValue;
        return Value::object(view);
    };

    builtins["ui_check_box_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("ui_check_box_new expects a text string and a Bool");
        }
        auto* view = vm.runtimeHeap().allocate<UiViewObject>(UiViewKind::CheckBox);
        view->text = args[0].stringValue;
        view->checked = isTruthy(args[1]);
        return Value::object(view);
    };

    builtins["ui_row_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("ui_row_new expects exactly one List argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        auto* view = vm.runtimeHeap().allocate<UiViewObject>(UiViewKind::Row);
        for (const auto& child : list->elements) {
            if (child.kind != Value::Kind::Object || !child.objectValue || child.objectValue->kind != ObjectKind::UiView) {
                throw std::runtime_error("ui_row_new expects only UiView children");
            }
            view->children.push_back(child);
        }
        return Value::object(view);
    };

    builtins["ui_column_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("ui_column_new expects exactly one List argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        auto* view = vm.runtimeHeap().allocate<UiViewObject>(UiViewKind::Column);
        for (const auto& child : list->elements) {
            if (child.kind != Value::Kind::Object || !child.objectValue || child.objectValue->kind != ObjectKind::UiView) {
                throw std::runtime_error("ui_column_new expects only UiView children");
            }
            view->children.push_back(child);
        }
        return Value::object(view);
    };

    builtins["ui_grid_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("ui_grid_new expects a List and column count");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        auto* view = vm.runtimeHeap().allocate<UiViewObject>(UiViewKind::Grid);
        view->gridColumns = static_cast<int>(toInt(args[1]));
        for (const auto& child : list->elements) {
            if (child.kind != Value::Kind::Object || !child.objectValue || child.objectValue->kind != ObjectKind::UiView) {
                throw std::runtime_error("ui_grid_new expects only UiView children");
            }
            view->children.push_back(child);
        }
        return Value::object(view);
    };

    builtins["ui_view_get_text"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_view_get_text expects exactly one UiView argument");
        }
        return Value::string(static_cast<UiViewObject*>(args[0].objectValue)->text);
    };

    builtins["ui_view_set_text"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[1].kind != Value::Kind::String ||
            args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_view_set_text expects a UiView and a string");
        }
        auto* view = static_cast<UiViewObject*>(args[0].objectValue);
        view->text = args[1].stringValue;
        ui::setViewText(vm, *view);
        return Value::null();
    };

    builtins["ui_view_get_placeholder"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_view_get_placeholder expects exactly one UiView argument");
        }
        auto* view = static_cast<UiViewObject*>(args[0].objectValue);
        if (view->viewKind != UiViewKind::TextField && view->viewKind != UiViewKind::TextArea) {
            throw std::runtime_error("ui_view_get_placeholder expects a TextField or TextArea");
        }
        return Value::string(view->placeholder);
    };

    builtins["ui_view_set_placeholder"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[1].kind != Value::Kind::String ||
            args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_view_set_placeholder expects a UiView and a string");
        }
        auto* view = static_cast<UiViewObject*>(args[0].objectValue);
        if (view->viewKind != UiViewKind::TextField && view->viewKind != UiViewKind::TextArea) {
            throw std::runtime_error("ui_view_set_placeholder expects a TextField or TextArea");
        }
        view->placeholder = args[1].stringValue;
        ui::setViewPlaceholder(*view);
        return Value::null();
    };

    builtins["ui_view_get_checked"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_view_get_checked expects exactly one UiView argument");
        }
        auto* view = static_cast<UiViewObject*>(args[0].objectValue);
        if (view->viewKind != UiViewKind::CheckBox) {
            throw std::runtime_error("ui_view_get_checked expects a CheckBox");
        }
        return Value::boolean(view->checked);
    };

    builtins["ui_view_set_checked"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_view_set_checked expects a CheckBox and a Bool");
        }
        auto* view = static_cast<UiViewObject*>(args[0].objectValue);
        if (view->viewKind != UiViewKind::CheckBox) {
            throw std::runtime_error("ui_view_set_checked expects a CheckBox");
        }
        view->checked = isTruthy(args[1]);
        ui::setViewChecked(vm, *view);
        return Value::null();
    };

    builtins["ui_view_on_click"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_view_on_click expects a UiView and a callback");
        }
        static_cast<UiViewObject*>(args[0].objectValue)->onClick = args[1];
        return Value::null();
    };

    builtins["ui_view_on_change"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_view_on_change expects a UiView and a callback");
        }
        static_cast<UiViewObject*>(args[0].objectValue)->onChange = args[1];
        return Value::null();
    };

    builtins["ui_view_click"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiView) {
            throw std::runtime_error("ui_view_click expects exactly one UiView argument");
        }
        auto* view = static_cast<UiViewObject*>(args[0].objectValue);
        if (view->viewKind != UiViewKind::Button && view->viewKind != UiViewKind::CheckBox) {
            throw std::runtime_error("ui_view_click expects a Button or CheckBox");
        }
        ui::clickView(vm, *view);
        return Value::null();
    };

    builtins["ui_dialog_info"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("ui_dialog_info expects title and message strings");
        }
        ui::infoDialog(args[0].stringValue, args[1].stringValue);
        return Value::null();
    };

    builtins["ui_dialog_confirm"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("ui_dialog_confirm expects title and message strings");
        }
        return Value::boolean(ui::confirmDialog(args[0].stringValue, args[1].stringValue));
    };

    builtins["ui_theme_new"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("ui_theme_new expects exactly one string argument");
        }
        return Value::object(vm.runtimeHeap().allocate<UiThemeObject>(args[0].stringValue));
    };

    builtins["ui_theme_set"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 3 || args[1].kind != Value::Kind::String || args[2].kind != Value::Kind::String ||
            args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::UiTheme) {
            throw std::runtime_error("ui_theme_set expects a UiTheme, key, and value");
        }
        auto* theme = static_cast<UiThemeObject*>(args[0].objectValue);
        theme->values[args[1].stringValue] = args[2].stringValue;
        return args[0];
    };

    builtins["async_ready"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("async_ready expects exactly one argument");
        }
        return Value::object(vm.runtimeHeap().allocate<FutureObject>(args[0]));
    };

    builtins["async_yield"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (!args.empty()) {
            throw std::runtime_error("async_yield expects no arguments");
        }
        Value future = Value::object(vm.runtimeHeap().allocate<FutureObject>());
        vm.nextTickFutures.push_back(future);
        return future;
    };

    builtins["str"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("str expects exactly one argument");
        }
        return Value::string(toString(args[0]));
    };

    builtins["int"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("int expects exactly one argument");
        }
        return Value::integer(toInt(args[0]));
    };

    builtins["string_trim"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("string_trim expects exactly one string argument");
        }
        return Value::string(trimAscii(args[0].stringValue));
    };

    builtins["string_lower"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("string_lower expects exactly one string argument");
        }
        return Value::string(lowerAscii(args[0].stringValue));
    };

    builtins["string_upper"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("string_upper expects exactly one string argument");
        }
        return Value::string(upperAscii(args[0].stringValue));
    };

    builtins["string_contains"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("string_contains expects two string arguments");
        }
        return Value::boolean(args[0].stringValue.find(args[1].stringValue) != std::string::npos);
    };

    builtins["string_starts_with"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("string_starts_with expects two string arguments");
        }
        return Value::boolean(
            args[0].stringValue.size() >= args[1].stringValue.size() &&
            args[0].stringValue.compare(0, args[1].stringValue.size(), args[1].stringValue) == 0);
    };

    builtins["string_ends_with"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("string_ends_with expects two string arguments");
        }
        return Value::boolean(
            args[0].stringValue.size() >= args[1].stringValue.size() &&
            args[0].stringValue.compare(
                args[0].stringValue.size() - args[1].stringValue.size(),
                args[1].stringValue.size(),
                args[1].stringValue) == 0);
    };

    builtins["string_replace"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 3 ||
            args[0].kind != Value::Kind::String ||
            args[1].kind != Value::Kind::String ||
            args[2].kind != Value::Kind::String) {
            throw std::runtime_error("string_replace expects three string arguments");
        }
        return Value::string(replaceAll(args[0].stringValue, args[1].stringValue, args[2].stringValue));
    };

    builtins["string_split"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("string_split expects two string arguments");
        }
        std::vector<Value> parts;
        for (const auto& piece : splitString(args[0].stringValue, args[1].stringValue)) {
            parts.push_back(Value::string(piece));
        }
        return Value::object(vm.runtimeHeap().allocate<ListObject>(std::move(parts)));
    };

    builtins["string_join"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[1].kind != Value::Kind::String) {
            throw std::runtime_error("string_join expects a list and a string separator");
        }
        if (args[0].kind != Value::Kind::Object || !args[0].objectValue ||
            args[0].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("string_join expects a list as its first argument");
        }
        auto* list = static_cast<ListObject*>(args[0].objectValue);
        return Value::string(joinValues(list->elements, args[1].stringValue));
    };

    builtins["string_format"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 2 || args[0].kind != Value::Kind::String) {
            throw std::runtime_error("string_format expects a string pattern and a list of values");
        }
        if (args[1].kind != Value::Kind::Object || !args[1].objectValue ||
            args[1].objectValue->kind != ObjectKind::List) {
            throw std::runtime_error("string_format expects a list as its second argument");
        }
        auto* list = static_cast<ListObject*>(args[1].objectValue);
        return Value::string(formatTemplate(args[0].stringValue, list->elements));
    };

    builtins["type_name"] = [](BytecodeVM&, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("type_name expects exactly one argument");
        }
        switch (args[0].kind) {
            case Value::Kind::Null: return Value::string("Null");
            case Value::Kind::Bool: return Value::string("Bool");
            case Value::Kind::Int: return Value::string("Int");
            case Value::Kind::Float: return Value::string("Float");
            case Value::Kind::String: return Value::string("String");
            case Value::Kind::Object:
                if (args[0].objectValue && args[0].objectValue->kind == ObjectKind::Future) {
                    return Value::string("Future");
                }
                if (args[0].objectValue && args[0].objectValue->kind == ObjectKind::Tuple) {
                    return Value::string("Tuple");
                }
                if (args[0].objectValue && args[0].objectValue->kind == ObjectKind::Map) {
                    return Value::string("Map");
                }
                if (args[0].objectValue && args[0].objectValue->kind == ObjectKind::Set) {
                    return Value::string("Set");
                }
                if (args[0].objectValue && args[0].objectValue->kind == ObjectKind::UiApp) {
                    return Value::string("UiApp");
                }
                if (args[0].objectValue && args[0].objectValue->kind == ObjectKind::UiWindow) {
                    return Value::string("UiWindow");
                }
                if (args[0].objectValue && args[0].objectValue->kind == ObjectKind::UiView) {
                    return Value::string("UiView");
                }
                if (args[0].objectValue && args[0].objectValue->kind == ObjectKind::UiTheme) {
                    return Value::string("UiTheme");
                }
                if (args[0].objectValue && args[0].objectValue->kind == ObjectKind::Instance) {
                    return Value::string(static_cast<InstanceObject*>(args[0].objectValue)->typeName);
                }
                return Value::string(args[0].objectValue ? args[0].objectValue->describe() : "Null");
        }
        return Value::string("Unknown");
    };

    builtins["implements"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("implements expects exactly two arguments");
        }
        if (args[0].kind == Value::Kind::Object && args[0].objectValue &&
            args[0].objectValue->kind == ObjectKind::Instance &&
            args[1].kind == Value::Kind::String) {
            auto* instance = static_cast<InstanceObject*>(args[0].objectValue);
            return Value::boolean(vm.currentModule && instanceSatisfiesInterface(*vm.currentModule, *instance, args[1].stringValue));
        }
        return Value::boolean(false);
    };

    builtins["panic"] = [](BytecodeVM&, const std::vector<Value>& args) -> Value {
        if (args.size() != 1) {
            throw std::runtime_error("panic expects exactly one argument");
        }
        throw std::runtime_error(toString(args[0]));
    };

    builtins["gc_heap_size"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        return Value::integer(static_cast<int64_t>(vm.heap.size()));
    };

    builtins["gc_collections"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        return Value::integer(static_cast<int64_t>(vm.getMetrics().collections));
    };

    builtins["gc_allocated_objects"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        return Value::integer(static_cast<int64_t>(vm.getMetrics().allocatedObjects));
    };

    builtins["gc_last_reclaimed"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        return Value::integer(static_cast<int64_t>(vm.getMetrics().lastCollectionReclaimed));
    };

    builtins["gc_peak_heap_size"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        return Value::integer(static_cast<int64_t>(vm.getMetrics().peakTrackedObjects));
    };

    builtins["gc_collect"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        vm.collectGarbage();
        return Value::integer(static_cast<int64_t>(vm.getMetrics().lastCollectionReclaimed));
    };

    builtins["gc_set_threshold"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("gc_set_threshold expects exactly one argument");
        }
        vm.runtimeHeap().setCollectionThreshold(static_cast<size_t>(toInt(args[0])));
        return Value::integer(static_cast<int64_t>(vm.runtimeHeap().options().collectionThreshold));
    };

    builtins["gc_trace"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("gc_trace expects exactly one argument");
        }
        const bool enabled = isTruthy(args[0]);
        vm.runtimeHeap().setTraceCollections(enabled);
        vm.runtimeHeap().setTraceAllocations(enabled);
        return Value::boolean(enabled);
    };

    builtins["trace_execution"] = [](BytecodeVM& vm, const std::vector<Value>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("trace_execution expects exactly one argument");
        }
        const bool enabled = isTruthy(args[0]);
        vm.runtimeHeap().setTraceExecution(enabled);
        return Value::boolean(enabled);
    };

    builtins["profile_instruction_count"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        return Value::integer(static_cast<int64_t>(vm.getMetrics().bytecodeInstructionsExecuted));
    };

    builtins["profile_statement_count"] = [](BytecodeVM&, const std::vector<Value>&) {
        return Value::integer(0);
    };

    builtins["profile_expression_count"] = [](BytecodeVM&, const std::vector<Value>&) {
        return Value::integer(0);
    };

    builtins["profile_call_count"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        return Value::integer(static_cast<int64_t>(vm.getMetrics().functionCalls));
    };

    builtins["profile_native_call_count"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        return Value::integer(static_cast<int64_t>(vm.getMetrics().nativeCalls));
    };

    builtins["profile_max_call_depth"] = [](BytecodeVM& vm, const std::vector<Value>&) {
        return Value::integer(static_cast<int64_t>(vm.getMetrics().maxCallDepth));
    };
}

Value BytecodeVM::execute() {
    Value result = callFunction(
        currentModule->entry.functionIndex,
        {},
        currentModule->functions[currentModule->entry.functionIndex].loc);
    if (result.kind == Value::Kind::Object && result.objectValue &&
        result.objectValue->kind == ObjectKind::Future) {
        return driveFuture(result, currentModule->functions[currentModule->entry.functionIndex].loc);
    }
    return result;
}

BytecodeVM::ExecutionResult BytecodeVM::executeUntilDepth(size_t targetDepth) {
    while (currentTask().frames.size() >= targetDepth) {
        try {
            if (currentTask().pendingThrow) {
                currentTask().pendingThrow = false;
                throw ThrownValue{currentTask().pendingThrownValue, currentTask().pendingThrownLoc};
            }
            Frame& frame = currentFrame();
            const BytecodeFunction& function = currentFunction();
            if (frame.pc >= function.instructions.size()) {
                currentTask().frames.pop_back();
                syncRoots();
                if (currentTask().frames.size() < targetDepth) {
                    return ExecutionResult{ExecutionResult::Kind::Returned, Value::null()};
                }
                push(Value::null());
                continue;
            }

            const Instruction instruction = function.instructions[frame.pc++];
            ++heap.metricsMutable().bytecodeInstructionsExecuted;
            if (heap.options().traceExecution) {
                std::cerr << "[exec-bc] " << function.qualifiedName << "#" << (frame.pc - 1) << '\n';
            }
            switch (instruction.opcode) {
            case OpCode::Nop:
                break;
            case OpCode::LoadConst: {
                const auto& constant = currentModule->constants[static_cast<size_t>(instruction.operand)];
                switch (constant.kind) {
                    case ConstantKind::Null:
                        push(Value::null());
                        break;
                    case ConstantKind::Bool:
                        push(Value::boolean(std::get<bool>(constant.value)));
                        break;
                    case ConstantKind::Int:
                        push(Value::integer(std::get<int64_t>(constant.value)));
                        break;
                    case ConstantKind::Float:
                        push(Value::floating(std::get<double>(constant.value)));
                        break;
                    case ConstantKind::String:
                        push(Value::string(std::get<std::string>(constant.value)));
                        break;
                }
                break;
            }
            case OpCode::LoadLocal:
                push(frame.locals[static_cast<size_t>(instruction.operand)]);
                break;
            case OpCode::StoreLocal:
                frame.locals[static_cast<size_t>(instruction.operand)] = pop(instruction.loc);
                syncRoots();
                if (heap.shouldCollect()) collectGarbage();
                break;
            case OpCode::LoadGlobal:
                push(globals[static_cast<size_t>(instruction.operand)]);
                break;
            case OpCode::StoreGlobal: {
                Value value = pop(instruction.loc);
                globals[static_cast<size_t>(instruction.operand)] = value;
                syncRoots();
                if (heap.shouldCollect()) collectGarbage();
                break;
            }
            case OpCode::Pop:
                pop(instruction.loc);
                break;
            case OpCode::Dup:
                push(peek(instruction.loc));
                break;
            case OpCode::Add: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                if (left.kind == Value::Kind::String || right.kind == Value::Kind::String) {
                    push(Value::string(toString(left) + toString(right)));
                } else if (left.kind == Value::Kind::Int && right.kind == Value::Kind::Int) {
                    push(Value::integer(left.intValue + right.intValue));
                } else {
                    push(Value::floating(toNumber(left) + toNumber(right)));
                }
                break;
            }
            case OpCode::Sub: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                if (left.kind == Value::Kind::Int && right.kind == Value::Kind::Int) {
                    push(Value::integer(left.intValue - right.intValue));
                } else {
                    push(Value::floating(toNumber(left) - toNumber(right)));
                }
                break;
            }
            case OpCode::Mul: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                if (left.kind == Value::Kind::Int && right.kind == Value::Kind::Int) {
                    push(Value::integer(left.intValue * right.intValue));
                } else {
                    push(Value::floating(toNumber(left) * toNumber(right)));
                }
                break;
            }
            case OpCode::Div: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::floating(toNumber(left) / toNumber(right)));
                break;
            }
            case OpCode::Mod: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::integer(toInt(left) % toInt(right)));
                break;
            }
            case OpCode::Pow: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::floating(std::pow(toNumber(left), toNumber(right))));
                break;
            }
            case OpCode::Neg: {
                Value value = pop(instruction.loc);
                if (value.kind == Value::Kind::Int) {
                    push(Value::integer(-value.intValue));
                } else {
                    push(Value::floating(-toNumber(value)));
                }
                break;
            }
            case OpCode::Not:
                push(Value::boolean(!isTruthy(pop(instruction.loc))));
                break;
            case OpCode::And: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::boolean(isTruthy(left) && isTruthy(right)));
                break;
            }
            case OpCode::Or: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::boolean(isTruthy(left) || isTruthy(right)));
                break;
            }
            case OpCode::Eq: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::boolean(valuesEqual(left, right)));
                break;
            }
            case OpCode::Ne: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::boolean(!valuesEqual(left, right)));
                break;
            }
            case OpCode::Lt: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::boolean(toNumber(left) < toNumber(right)));
                break;
            }
            case OpCode::Le: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::boolean(toNumber(left) <= toNumber(right)));
                break;
            }
            case OpCode::Gt: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::boolean(toNumber(left) > toNumber(right)));
                break;
            }
            case OpCode::Ge: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(Value::boolean(toNumber(left) >= toNumber(right)));
                break;
            }
            case OpCode::Jump:
                frame.pc = static_cast<size_t>(instruction.operand);
                break;
            case OpCode::JumpIfFalse: {
                Value condition = pop(instruction.loc);
                if (!isTruthy(condition)) {
                    frame.pc = static_cast<size_t>(instruction.operand);
                }
                break;
            }
            case OpCode::JumpIfTrue: {
                Value condition = pop(instruction.loc);
                if (isTruthy(condition)) {
                    frame.pc = static_cast<size_t>(instruction.operand);
                }
                break;
            }
            case OpCode::PushHandler: {
                const size_t catchPc = static_cast<size_t>(decodeHandlerPc(instruction.operand));
                const uint32_t catchSlot = decodeHandlerSlot(instruction.operand);
                if (catchPc > function.instructions.size() || catchSlot >= frame.locals.size()) {
                    runtimeError(instruction.loc, "invalid exception handler metadata");
                }
                frame.handlers.push_back(ExceptionHandler{catchPc, catchSlot, currentTask().operandStack.size()});
                break;
            }
            case OpCode::PopHandler:
                if (frame.handlers.empty()) {
                    runtimeError(instruction.loc, "exception handler stack underflow");
                }
                frame.handlers.pop_back();
                break;
            case OpCode::Throw: {
                Value value = pop(instruction.loc);
                throw ThrownValue{value, instruction.loc};
            }
            case OpCode::MakeList: {
                size_t count = static_cast<size_t>(instruction.operand);
                std::vector<Value> elements(count);
                for (size_t i = 0; i < count; ++i) {
                    elements[count - 1 - i] = pop(instruction.loc);
                }
                push(Value::object(heap.allocate<ListObject>(std::move(elements))));
                syncRoots();
                if (heap.shouldCollect()) collectGarbage();
                break;
            }
            case OpCode::MakeRange: {
                Value end = pop(instruction.loc);
                Value start = pop(instruction.loc);
                push(Value::object(heap.allocate<RangeObject>(
                    toInt(start),
                    toInt(end),
                    instruction.operand != 0)));
                syncRoots();
                if (heap.shouldCollect()) collectGarbage();
                break;
            }
            case OpCode::GetIndex: {
                Value index = pop(instruction.loc);
                Value object = pop(instruction.loc);
                int64_t i = toInt(index);
                if (object.kind == Value::Kind::String) {
                    if (i < 0 || static_cast<size_t>(i) >= object.stringValue.size()) {
                        runtimeError(instruction.loc, "string index out of bounds");
                    }
                    push(Value::string(std::string(1, object.stringValue[static_cast<size_t>(i)])));
                    break;
                }
                if (object.kind == Value::Kind::Object && object.objectValue &&
                    object.objectValue->kind == ObjectKind::List) {
                    auto* list = static_cast<ListObject*>(object.objectValue);
                    if (i < 0 || static_cast<size_t>(i) >= list->elements.size()) {
                        runtimeError(instruction.loc, "list index out of bounds");
                    }
                    push(list->elements[static_cast<size_t>(i)]);
                    break;
                }
                if (object.kind == Value::Kind::Object && object.objectValue &&
                    object.objectValue->kind == ObjectKind::Tuple) {
                    auto* tuple = static_cast<TupleObject*>(object.objectValue);
                    int64_t tupleIndex = toInt(index);
                    if (tupleIndex < 0 || static_cast<size_t>(tupleIndex) >= tuple->elements.size()) {
                        runtimeError(instruction.loc, "tuple index out of bounds");
                    }
                    push(tuple->elements[static_cast<size_t>(tupleIndex)]);
                    break;
                }
                if (object.kind == Value::Kind::Object && object.objectValue &&
                    object.objectValue->kind == ObjectKind::Map) {
                    auto* map = static_cast<MapObject*>(object.objectValue);
                    for (const auto& entry : map->entries) {
                        if (valuesEqual(entry.first, index)) {
                            push(entry.second);
                            goto done_get_index;
                        }
                    }
                    push(Value::null());
                    break;
                }
                if (object.kind == Value::Kind::Object && object.objectValue &&
                    object.objectValue->kind == ObjectKind::Range) {
                    auto* range = static_cast<RangeObject*>(object.objectValue);
                    const int64_t step = range->start <= range->end ? 1 : -1;
                    const int64_t span = std::llabs(range->end - range->start) + (range->inclusive ? 1 : 0);
                    if (i < 0 || i >= span) {
                        runtimeError(instruction.loc, "range index out of bounds");
                    }
                    push(Value::integer(range->start + (i * step)));
                    break;
                }
                runtimeError(instruction.loc, "indexing requires a list, tuple, map, range, or string");
done_get_index:
                break;
            }
            case OpCode::SetIndex: {
                Value index = pop(instruction.loc);
                Value object = pop(instruction.loc);
                Value value = pop(instruction.loc);
                if (object.kind == Value::Kind::Object && object.objectValue &&
                    object.objectValue->kind == ObjectKind::List) {
                    auto* list = static_cast<ListObject*>(object.objectValue);
                    int64_t i = toInt(index);
                    if (i < 0 || static_cast<size_t>(i) >= list->elements.size()) {
                        runtimeError(instruction.loc, "list index out of bounds");
                    }
                    list->elements[static_cast<size_t>(i)] = value;
                    push(value);
                    break;
                }
                if (object.kind == Value::Kind::Object && object.objectValue &&
                    object.objectValue->kind == ObjectKind::Map) {
                    auto* map = static_cast<MapObject*>(object.objectValue);
                    for (auto& entry : map->entries) {
                        if (valuesEqual(entry.first, index)) {
                            entry.second = value;
                            push(value);
                            goto done_set_index;
                        }
                    }
                    map->entries.push_back({index, value});
                    push(value);
                    break;
                }
                runtimeError(instruction.loc, "index assignment requires a list or map");
done_set_index:
                break;
            }
            case OpCode::GetMember:
            case OpCode::GetSuperMember:
            case OpCode::GetMemberSafe: {
                Value object = pop(instruction.loc);
                const std::string member = readStringConstant(*currentModule, instruction.operand);
                if (object.kind == Value::Kind::Null) {
                    if (instruction.opcode == OpCode::GetMemberSafe) {
                        push(Value::null());
                        break;
                    }
                    runtimeError(instruction.loc, "cannot access member on null");
                }
                if (object.kind == Value::Kind::String && member == "length") {
                    push(Value::integer(static_cast<int64_t>(object.stringValue.size())));
                    break;
                }
                if (object.kind == Value::Kind::Object && object.objectValue) {
                    if (instruction.opcode == OpCode::GetSuperMember) {
                        if (object.objectValue->kind != ObjectKind::Instance) {
                            runtimeError(instruction.loc, "super access requires an instance receiver");
                        }
                        const auto& owner = currentFunction().ownerType;
                        auto ownerIt = currentModule->typeMap.find(owner);
                        if (owner.empty() || ownerIt == currentModule->typeMap.end()) {
                            runtimeError(instruction.loc, "super access requires an instance method context");
                        }
                        const auto& ownerLayout = currentModule->types[ownerIt->second];
                        if (ownerLayout.baseType.empty()) {
                            runtimeError(instruction.loc, "super access requires a base class");
                        }
                        auto* instance = static_cast<InstanceObject*>(object.objectValue);
                        if (const auto* field = findFieldLayout(*currentModule, ownerLayout.baseType, member)) {
                            auto fieldIt = instance->fields.find(field->name);
                            if (fieldIt != instance->fields.end()) {
                                push(fieldIt->second);
                                break;
                            }
                        }
                        std::vector<uint32_t> methods;
                        auto baseIt = currentModule->typeMap.find(ownerLayout.baseType);
                        if (baseIt != currentModule->typeMap.end()) {
                            collectInheritedInstanceMethods(*currentModule, baseIt->second, member, methods);
                        }
                        if (!methods.empty()) {
                            push(Value::object(heap.allocate<BytecodeBoundMethodObject>(object, std::move(methods), member)));
                            syncRoots();
                            if (heap.shouldCollect()) collectGarbage();
                            break;
                        }
                        runtimeError(instruction.loc, "unknown super member '" + member + "'");
                    }
                    if (object.objectValue->kind == ObjectKind::List) {
                        auto* list = static_cast<ListObject*>(object.objectValue);
                        if (member == "length") {
                            push(Value::integer(static_cast<int64_t>(list->elements.size())));
                            break;
                        }
                    }
                    if (object.objectValue->kind == ObjectKind::Tuple) {
                        auto* tuple = static_cast<TupleObject*>(object.objectValue);
                        if (member == "length") {
                            push(Value::integer(static_cast<int64_t>(tuple->elements.size())));
                            break;
                        }
                    }
                    if (object.objectValue->kind == ObjectKind::Map) {
                        auto* map = static_cast<MapObject*>(object.objectValue);
                        if (member == "length" || member == "size") {
                            push(Value::integer(static_cast<int64_t>(map->entries.size())));
                            break;
                        }
                    }
                    if (object.objectValue->kind == ObjectKind::Set) {
                        auto* set = static_cast<SetObject*>(object.objectValue);
                        if (member == "length" || member == "size") {
                            push(Value::integer(static_cast<int64_t>(set->elements.size())));
                            break;
                        }
                    }
                    if (object.objectValue->kind == ObjectKind::Range) {
                        auto* range = static_cast<RangeObject*>(object.objectValue);
                        if (member == "length") {
                            const int64_t length = std::llabs(range->end - range->start) + (range->inclusive ? 1 : 0);
                            push(Value::integer(length));
                            break;
                        }
                    }
                    if (object.objectValue->kind == ObjectKind::Instance) {
                        auto* instance = static_cast<InstanceObject*>(object.objectValue);
                        auto field = instance->fields.find(member);
                        if (field != instance->fields.end()) {
                            push(field->second);
                            break;
                        }
                        auto method = instance->bytecodeMethods.find(member);
                        if (method != instance->bytecodeMethods.end()) {
                            push(Value::object(heap.allocate<BytecodeBoundMethodObject>(object, method->second, member)));
                            syncRoots();
                            if (heap.shouldCollect()) collectGarbage();
                            break;
                        }
                    }
                }
                if (object.kind == Value::Kind::String) {
                    auto typeIt = currentModule->typeMap.find(object.stringValue);
                    if (typeIt != currentModule->typeMap.end()) {
                        std::vector<uint32_t> staticMethods;
                        collectInheritedStaticMethods(*currentModule, typeIt->second, member, staticMethods);
                        if (!staticMethods.empty()) {
                            push(Value::object(heap.allocate<BytecodeBoundMethodObject>(Value::null(), std::move(staticMethods), member, true)));
                            syncRoots();
                            if (heap.shouldCollect()) collectGarbage();
                            break;
                        }
                    }
                }
                runtimeError(instruction.loc, "unknown member '" + member + "'");
            }
            case OpCode::SetMember: {
                Value object = pop(instruction.loc);
                Value value = pop(instruction.loc);
                const std::string member = readStringConstant(*currentModule, instruction.operand);
                if (object.kind == Value::Kind::Object && object.objectValue &&
                    object.objectValue->kind == ObjectKind::Instance) {
                    auto* instance = static_cast<InstanceObject*>(object.objectValue);
                    const auto* field = findFieldLayout(*currentModule, instance->typeName, member);
                    if (!field) {
                        runtimeError(instruction.loc, "unknown member '" + member + "'");
                    }
                    if (!runtimeMatchesIRType(*currentModule, value, field->type)) {
                        runtimeError(
                            instruction.loc,
                            "cannot assign '" + runtimeTypeName(value) + "' to field '" + member +
                                "' of type '" + field->type.toString() + "'");
                    }
                    instance->fields[member] = value;
                    push(value);
                    break;
                }
                runtimeError(instruction.loc, "member assignment requires an instance");
            }
            case OpCode::Coalesce: {
                Value right = pop(instruction.loc);
                Value left = pop(instruction.loc);
                push(left.kind == Value::Kind::Null ? right : left);
                break;
            }
            case OpCode::Await: {
                Value awaited = pop(instruction.loc);
                if (awaited.kind == Value::Kind::Object && awaited.objectValue &&
                    awaited.objectValue->kind == ObjectKind::Future) {
                    auto* future = static_cast<FutureObject*>(awaited.objectValue);
                    if (future->state == FutureObject::State::Resolved) {
                        push(future->resolved);
                        break;
                    }
                    if (future->state == FutureObject::State::Failed) {
                        throw ThrownValue{future->failure, future->completionLoc};
                    }
                    future->waitingTasks.push_back(currentTask().id);
                    currentTask().state = TaskState::Waiting;
                    currentTask().waitingFuture = awaited;
                    syncRoots();
                    return ExecutionResult{ExecutionResult::Kind::Suspended, Value::null()};
                }
                runtimeError(instruction.loc, "await expects a Future value");
            }
            case OpCode::TypeCheck: {
                size_t typeIndex = static_cast<size_t>(instruction.operand);
                if (typeIndex >= currentModule->typeConstants.size()) {
                    runtimeError(instruction.loc, "invalid type constant index");
                }
                Value value = pop(instruction.loc);
                push(Value::boolean(runtimeMatchesIRType(*currentModule, value, currentModule->typeConstants[typeIndex])));
                break;
            }
            case OpCode::SafeCast: {
                size_t typeIndex = static_cast<size_t>(instruction.operand);
                if (typeIndex >= currentModule->typeConstants.size()) {
                    runtimeError(instruction.loc, "invalid type constant index");
                }
                Value value = pop(instruction.loc);
                push(safeCastValue(*currentModule, value, currentModule->typeConstants[typeIndex]));
                break;
            }
            case OpCode::CheckedCast: {
                size_t typeIndex = static_cast<size_t>(instruction.operand);
                if (typeIndex >= currentModule->typeConstants.size()) {
                    runtimeError(instruction.loc, "invalid type constant index");
                }
                Value value = pop(instruction.loc);
                push(checkedCastValue(*currentModule, value, currentModule->typeConstants[typeIndex], instruction.loc));
                break;
            }
            case OpCode::CreateClosure: {
                uint32_t functionIndex = static_cast<uint32_t>(instruction.operand);
                if (functionIndex >= currentModule->functions.size()) {
                    runtimeError(instruction.loc, "invalid closure function index");
                }
                const auto& target = currentModule->functions[functionIndex];
                std::vector<Value> captures(target.captures.size());
                for (size_t i = 0; i < target.captures.size(); ++i) {
                    captures[target.captures.size() - 1 - i] = pop(instruction.loc);
                }
                push(Value::object(heap.allocate<BytecodeClosureObject>(functionIndex, target.qualifiedName, std::move(captures))));
                syncRoots();
                if (heap.shouldCollect()) collectGarbage();
                break;
            }
            case OpCode::Call: {
                size_t argc = static_cast<size_t>(instruction.operand);
                std::vector<Value> args(argc);
                for (size_t i = 0; i < argc; ++i) {
                    args[argc - 1 - i] = pop(instruction.loc);
                }
                Value callee = pop(instruction.loc);
                push(callValue(callee, args, instruction.loc));
                syncRoots();
                if (heap.shouldCollect()) collectGarbage();
                break;
            }
            case OpCode::Return: {
                Value result = currentTask().operandStack.empty() ? Value::null() : pop(instruction.loc);
                currentTask().frames.pop_back();
                syncRoots();
                if (currentTask().frames.size() < targetDepth) {
                    return ExecutionResult{ExecutionResult::Kind::Returned, result};
                }
                push(result);
                break;
            }
            case OpCode::ConstructData:
            case OpCode::ConstructClass: {
                uint32_t typeIndex = static_cast<uint32_t>((static_cast<uint64_t>(instruction.operand) >> 32) & 0xffffffffu);
                uint32_t argCount = static_cast<uint32_t>(static_cast<uint64_t>(instruction.operand) & 0xffffffffu);
                std::vector<Value> args(argCount);
                for (size_t i = 0; i < argCount; ++i) {
                    args[argCount - 1 - i] = pop(instruction.loc);
                }
                push(constructType(typeIndex, args, instruction.loc));
                syncRoots();
                if (heap.shouldCollect()) collectGarbage();
                break;
            }
            case OpCode::PrintDebugLine:
                std::cerr << "[bc] " << instruction.loc.filename << ":" << instruction.loc.line << '\n';
                break;
            case OpCode::Halt:
                return ExecutionResult{
                    ExecutionResult::Kind::Returned,
                    currentTask().operandStack.empty() ? Value::null() : pop(instruction.loc)};
            }
        } catch (const ThrownValue& thrown) {
            if (!handleThrownValue(thrown.value, thrown.loc, targetDepth)) {
                throw;
            }
        }
    }

    return ExecutionResult{ExecutionResult::Kind::Returned, Value::null()};
}

Value BytecodeVM::callValue(const Value& callee, const std::vector<Value>& arguments, const SourceLocation& loc) {
    ++heap.metricsMutable().functionCalls;
    if (callee.kind == Value::Kind::String) {
        if (builtins.find(callee.stringValue) != builtins.end()) {
            return invokeBuiltin(callee.stringValue, arguments, loc);
        }
        auto fnIt = currentModule->functionMap.find(callee.stringValue);
        if (fnIt != currentModule->functionMap.end()) {
            return callFunction(fnIt->second, arguments, loc);
        }
        auto typeIt = currentModule->typeMap.find(callee.stringValue);
        if (typeIt != currentModule->typeMap.end()) {
            return constructType(typeIt->second, arguments, loc);
        }
        runtimeError(loc, "unknown callable '" + callee.stringValue + "'");
    }

    if (callee.kind != Value::Kind::Object || !callee.objectValue) {
        runtimeError(loc, "attempted to call a non-function value");
    }

    if (callee.objectValue->kind == ObjectKind::BytecodeBoundMethod) {
        auto* method = static_cast<BytecodeBoundMethodObject*>(callee.objectValue);
        uint32_t selectedIndex = UINT32_MAX;
        int bestScore = -1;
        bool ambiguous = false;
        for (uint32_t functionIndex : method->functionIndices) {
            const auto& function = currentModule->functions[functionIndex];
            int score = bytecodeOverloadScore(*currentModule, function, arguments, !method->isStatic);
            if (score < 0) {
                continue;
            }
            if (score > bestScore) {
                selectedIndex = functionIndex;
                bestScore = score;
                ambiguous = false;
            } else if (score == bestScore) {
                ambiguous = true;
            }
        }
        if (ambiguous) {
            runtimeError(loc, "ambiguous overload for method '" + method->name + "'");
        }
        if (selectedIndex == UINT32_MAX) {
            runtimeError(loc, "no matching overload for method '" + method->name + "'");
        }
        std::vector<Value> fullArguments;
        fullArguments.reserve(arguments.size() + (method->isStatic ? 0 : 1));
        if (!method->isStatic) {
            fullArguments.push_back(method->receiver);
        }
        fullArguments.insert(fullArguments.end(), arguments.begin(), arguments.end());
        return callFunction(selectedIndex, fullArguments, loc);
    }

    if (callee.objectValue->kind == ObjectKind::BytecodeClosure) {
        auto* closure = static_cast<BytecodeClosureObject*>(callee.objectValue);
        return callFunction(closure->functionIndex, arguments, loc, &closure->captures);
    }

    runtimeError(loc, "attempted to call an unsupported callable");
}

Value BytecodeVM::callFunction(uint32_t functionIndex,
                               const std::vector<Value>& arguments,
                               const SourceLocation& loc,
                               const std::vector<Value>* captures) {
    if (functionIndex >= currentModule->functions.size()) {
        runtimeError(loc, "invalid function index");
    }

    const auto& function = currentModule->functions[functionIndex];
    if (arguments.size() != function.parameters.size()) {
        runtimeError(loc, "wrong argument count when calling '" + function.qualifiedName + "'");
    }
    if (captures && captures->size() != function.captures.size()) {
        runtimeError(loc, "wrong capture count when calling '" + function.qualifiedName + "'");
    }
    if (!captures && !function.captures.empty()) {
        runtimeError(loc, "missing closure captures for '" + function.qualifiedName + "'");
    }

    if (function.async) {
        Value futureValue = Value::object(heap.allocate<FutureObject>());
        createTask(functionIndex, arguments, loc, captures, futureValue);
        syncRoots();
        if (heap.shouldCollect()) collectGarbage();
        return futureValue;
    }

    if (activeTaskId == static_cast<size_t>(-1)) {
        Task rootTask;
        rootTask.id = tasks.size();
        rootTask.state = TaskState::Runnable;
        tasks.push_back(std::move(rootTask));
        activeTaskId = tasks.back().id;
    }

    const size_t callerTaskId = activeTaskId;
    const size_t targetDepth = tasks[callerTaskId].frames.size() + 1;
    createTask(functionIndex, arguments, loc, captures, Value::null());
    if (tasks[callerTaskId].frames.size() > heap.metrics().maxCallDepth) {
        heap.metricsMutable().maxCallDepth = tasks[callerTaskId].frames.size();
    }
    syncRoots();
    while (true) {
        ExecutionResult result = executeUntilDepth(targetDepth);
        if (result.kind == ExecutionResult::Kind::Returned) {
            return result.value;
        }
        if (tasks[callerTaskId].waitingFuture.kind == Value::Kind::Null) {
            runtimeError(loc, "synchronous function '" + function.qualifiedName + "' suspended");
        }
        driveFuture(tasks[callerTaskId].waitingFuture, loc);
        activeTaskId = callerTaskId;
    }
}

size_t BytecodeVM::createTask(uint32_t functionIndex,
                              const std::vector<Value>& arguments,
                              const SourceLocation& loc,
                              const std::vector<Value>* captures,
                              const Value& completionFuture) {
    if (functionIndex >= currentModule->functions.size()) {
        runtimeError(loc, "invalid function index");
    }

    const auto& function = currentModule->functions[functionIndex];
    Frame frame;
    frame.functionIndex = functionIndex;
    frame.pc = 0;
    frame.locals.resize(function.locals.size(), Value::null());
    if (captures) {
        for (size_t i = 0; i < captures->size(); ++i) {
            frame.locals[function.captures[i].slot] = (*captures)[i];
        }
    }
    for (size_t i = 0; i < arguments.size(); ++i) {
        frame.locals[function.parameters[i].slot] = arguments[i];
    }

    if (completionFuture.kind == Value::Kind::Null) {
        currentTask().frames.push_back(std::move(frame));
        if (currentTask().frames.size() > heap.metrics().maxCallDepth) {
            heap.metricsMutable().maxCallDepth = currentTask().frames.size();
        }
        return activeTaskId;
    }

    Task task;
    task.id = tasks.size();
    task.state = TaskState::Runnable;
    task.frames.push_back(std::move(frame));
    task.completionFuture = completionFuture;
    task.completionLoc = loc;
    tasks.push_back(std::move(task));
    runnableTasks.push_back(tasks.back().id);
    if (tasks.back().frames.size() > heap.metrics().maxCallDepth) {
        heap.metricsMutable().maxCallDepth = tasks.back().frames.size();
    }
    return tasks.back().id;
}

Value BytecodeVM::driveFuture(const Value& futureValue, const SourceLocation& loc) {
    if (futureValue.kind != Value::Kind::Object || !futureValue.objectValue ||
        futureValue.objectValue->kind != ObjectKind::Future) {
        runtimeError(loc, "await expects a Future value");
    }

    driveFutureRoots.push_back(futureValue);
    syncRoots();
    auto* future = static_cast<FutureObject*>(futureValue.objectValue);
    while (future->state == FutureObject::State::Pending) {
        if (runnableTasks.empty() && nextTickFutures.empty()) {
            driveFutureRoots.pop_back();
            runtimeError(loc, "deadlock awaiting unresolved future");
        }
        runReadyTasks();
    }
    Value result = future->resolved;
    Value failure = future->failure;
    FutureObject::State state = future->state;
    driveFutureRoots.pop_back();
    if (state == FutureObject::State::Failed) {
        throw ThrownValue{failure, future->completionLoc};
    }
    return result;
}

void BytecodeVM::runReadyTasks() {
    while (true) {
        while (!runnableTasks.empty()) {
            const size_t taskId = runnableTasks.front();
            runnableTasks.pop_front();
            if (taskId >= tasks.size()) {
                continue;
            }
            if (tasks[taskId].state != TaskState::Runnable || tasks[taskId].frames.empty()) {
                continue;
            }

            activeTaskId = taskId;
            try {
                ExecutionResult result = executeUntilDepth(1);
                if (result.kind == ExecutionResult::Kind::Returned) {
                    tasks[taskId].state = TaskState::Completed;
                    tasks[taskId].result = result.value;
                    if (tasks[taskId].completionFuture.kind == Value::Kind::Object && tasks[taskId].completionFuture.objectValue &&
                        tasks[taskId].completionFuture.objectValue->kind == ObjectKind::Future) {
                        resolveFuture(*static_cast<FutureObject*>(tasks[taskId].completionFuture.objectValue), result.value, tasks[taskId].completionLoc);
                    }
                }
            } catch (const ThrownValue& thrown) {
                tasks[taskId].state = TaskState::Failed;
                tasks[taskId].failure = thrown.value;
                tasks[taskId].failureLoc = thrown.loc;
                if (tasks[taskId].completionFuture.kind == Value::Kind::Object && tasks[taskId].completionFuture.objectValue &&
                    tasks[taskId].completionFuture.objectValue->kind == ObjectKind::Future) {
                    failFuture(*static_cast<FutureObject*>(tasks[taskId].completionFuture.objectValue), thrown.value, thrown.loc);
                } else {
                    activeTaskId = static_cast<size_t>(-1);
                    throw;
                }
            }
            activeTaskId = static_cast<size_t>(-1);
            syncRoots();
            if (heap.shouldCollect()) collectGarbage();
        }

        if (nextTickFutures.empty()) {
            break;
        }

        {
            std::vector<Value> dueFutures = std::move(nextTickFutures);
            nextTickFutures.clear();
            for (const auto& futureValue : dueFutures) {
                if (futureValue.kind != Value::Kind::Object || !futureValue.objectValue ||
                    futureValue.objectValue->kind != ObjectKind::Future) {
                    continue;
                }
                auto* future = static_cast<FutureObject*>(futureValue.objectValue);
                if (future->state == FutureObject::State::Pending) {
                    resolveFuture(*future, Value::null(), SourceLocation{});
                }
            }
        }
        syncRoots();
        if (heap.shouldCollect()) collectGarbage();
    }
}

void BytecodeVM::wakeWaitingTasks(FutureObject& future) {
    for (size_t taskId : future.waitingTasks) {
        if (taskId >= tasks.size()) {
            continue;
        }
        Task& task = tasks[taskId];
        if (task.state != TaskState::Waiting) {
            continue;
        }
        task.state = TaskState::Runnable;
        task.waitingFuture = Value::null();
        if (future.state == FutureObject::State::Resolved) {
            task.operandStack.push_back(future.resolved);
        } else if (future.state == FutureObject::State::Failed) {
            task.pendingThrow = true;
            task.pendingThrownValue = future.failure;
            task.pendingThrownLoc = future.completionLoc;
        }
        runnableTasks.push_back(taskId);
    }
    future.waitingTasks.clear();
}

void BytecodeVM::resolveFuture(FutureObject& future, const Value& value, const SourceLocation& loc) {
    future.state = FutureObject::State::Resolved;
    future.resolved = value;
    future.failure = Value::null();
    future.completionLoc = loc;
    wakeWaitingTasks(future);
}

void BytecodeVM::failFuture(FutureObject& future, const Value& value, const SourceLocation& loc) {
    future.state = FutureObject::State::Failed;
    future.resolved = Value::null();
    future.failure = value;
    future.completionLoc = loc;
    wakeWaitingTasks(future);
}

Value BytecodeVM::constructType(uint32_t typeIndex, const std::vector<Value>& arguments, const SourceLocation& loc) {
    if (typeIndex >= currentModule->types.size()) {
        runtimeError(loc, "invalid type index");
    }

    const auto& type = currentModule->types[typeIndex];
    if (type.abstractClass) {
        runtimeError(loc, "cannot instantiate abstract class '" + type.name + "'");
    }
    auto* instance = heap.allocate<InstanceObject>(type.name);
    populateInstanceForType(*currentModule, typeIndex, *instance);
    Value instanceValue = Value::object(instance);
    syncRoots();

    for (const auto& field : type.fields) {
        if (field.defaultFunctionIndex == UINT32_MAX) {
            continue;
        }
        instance->fields[field.name] = callFunction(field.defaultFunctionIndex, {instanceValue}, loc);
    }

    if (type.constructorIndices.empty()) {
        if (arguments.size() > type.fields.size()) {
            runtimeError(loc, "wrong argument count constructing '" + type.name + "'");
        }
        for (size_t i = 0; i < arguments.size(); ++i) {
            const auto& field = type.fields[i];
            if (!runtimeMatchesIRType(*currentModule, arguments[i], field.type)) {
                runtimeError(loc,
                             "constructor argument for field '" + field.name + "' has type '" +
                                 runtimeTypeName(arguments[i]) + "', expected '" + field.type.toString() + "'");
            }
            instance->fields[field.name] = arguments[i];
        }
        for (size_t i = arguments.size(); i < type.fields.size(); ++i) {
            const auto& field = type.fields[i];
            if (instance->fields[field.name].kind == Value::Kind::Null && !fieldAllowsImplicitNull(field)) {
                runtimeError(loc, "missing value for field '" + field.name + "' in '" + type.name + "'");
            }
        }
        validateInitializedFields(*currentModule, *instance, loc);
        return instanceValue;
    }

    uint32_t selectedIndex = UINT32_MAX;
    int bestScore = -1;
    bool ambiguous = false;
    for (uint32_t functionIndex : type.constructorIndices) {
        const auto& function = currentModule->functions[functionIndex];
        int score = bytecodeOverloadScore(*currentModule, function, arguments, true);
        if (score < 0) {
            continue;
        }
        if (score > bestScore) {
            selectedIndex = functionIndex;
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore) {
            ambiguous = true;
        }
    }
    if (ambiguous) {
        runtimeError(loc, "ambiguous constructor for '" + type.name + "'");
    }
    if (selectedIndex == UINT32_MAX) {
        runtimeError(loc, "no matching constructor for '" + type.name + "'");
    }
    std::vector<Value> fullArguments;
    fullArguments.reserve(arguments.size() + 1);
    fullArguments.push_back(instanceValue);
    fullArguments.insert(fullArguments.end(), arguments.begin(), arguments.end());
    callFunction(selectedIndex, fullArguments, loc);
    validateInitializedFields(*currentModule, *instance, loc);
    return instanceValue;
}

Value BytecodeVM::resolveNamedSymbol(const std::string& name, const SourceLocation& loc) {
    if (builtins.find(name) != builtins.end()) {
        return Value::string(name);
    }
    auto global = globalNameToSlot.find(name);
    if (global != globalNameToSlot.end()) {
        return globals[global->second];
    }
    if (currentModule->functionMap.find(name) != currentModule->functionMap.end()) {
        return Value::string(name);
    }
    if (currentModule->typeMap.find(name) != currentModule->typeMap.end()) {
        return Value::string(name);
    }
    runtimeError(loc, "unknown symbol '" + name + "'");
}

Value BytecodeVM::invokeBuiltin(const std::string& name, const std::vector<Value>& arguments, const SourceLocation& loc) {
    auto it = builtins.find(name);
    if (it == builtins.end()) {
        runtimeError(loc, "unknown builtin '" + name + "'");
    }
    try {
        ++heap.metricsMutable().nativeCalls;
        return it->second(*this, arguments);
    } catch (const std::runtime_error& ex) {
        runtimeError(loc, ex.what());
    }
}

void BytecodeVM::push(const Value& value) {
    currentTask().operandStack.push_back(value);
}

Value BytecodeVM::pop(const SourceLocation& loc) {
    if (currentTask().operandStack.empty()) {
        runtimeError(loc, "operand stack underflow");
    }
    Value value = currentTask().operandStack.back();
    currentTask().operandStack.pop_back();
    return value;
}

Value BytecodeVM::peek(const SourceLocation& loc, size_t depth) const {
    if (currentTask().operandStack.size() <= depth) {
        runtimeError(loc, "operand stack underflow");
    }
    return currentTask().operandStack[currentTask().operandStack.size() - 1 - depth];
}

BytecodeVM::Task& BytecodeVM::currentTask() {
    if (activeTaskId == static_cast<size_t>(-1) || activeTaskId >= tasks.size()) {
        throw std::runtime_error("bytecode VM has no active task");
    }
    return tasks[activeTaskId];
}

const BytecodeVM::Task& BytecodeVM::currentTask() const {
    if (activeTaskId == static_cast<size_t>(-1) || activeTaskId >= tasks.size()) {
        throw std::runtime_error("bytecode VM has no active task");
    }
    return tasks[activeTaskId];
}

BytecodeVM::Frame& BytecodeVM::currentFrame() {
    return currentTask().frames.back();
}

const BytecodeFunction& BytecodeVM::currentFunction() const {
    return currentModule->functions[currentTask().frames.back().functionIndex];
}

bool BytecodeVM::handleThrownValue(const Value& value, const SourceLocation& loc, size_t targetDepth) {
    while (currentTask().frames.size() >= targetDepth) {
        Frame& frame = currentFrame();
        if (!frame.handlers.empty()) {
            ExceptionHandler handler = frame.handlers.back();
            frame.handlers.pop_back();
            while (currentTask().operandStack.size() > handler.stackDepth) {
                currentTask().operandStack.pop_back();
            }
            if (handler.catchSlot >= frame.locals.size()) {
                runtimeError(loc, "invalid catch binding slot");
            }
            frame.locals[handler.catchSlot] = value;
            frame.pc = handler.catchPc;
            syncRoots();
            return true;
        }
        currentTask().frames.pop_back();
        syncRoots();
    }
    return false;
}

void BytecodeVM::syncRoots() {
    gcRootEnv = std::make_shared<Environment>();
    environmentRoots.clear();
    environmentRoots.push_back(gcRootEnv.get());

    for (size_t i = 0; i < globals.size(); ++i) {
        gcRootEnv->define("$g" + std::to_string(i), globals[i], true);
    }
    for (size_t i = 0; i < nextTickFutures.size(); ++i) {
        gcRootEnv->define("$nt" + std::to_string(i), nextTickFutures[i], true);
    }
    for (size_t i = 0; i < driveFutureRoots.size(); ++i) {
        gcRootEnv->define("$df" + std::to_string(i), driveFutureRoots[i], true);
    }
    for (size_t i = 0; i < tasks.size(); ++i) {
        const Task& task = tasks[i];
        if (task.result.kind != Value::Kind::Null) {
            gcRootEnv->define("$t" + std::to_string(i) + "_result", task.result, true);
        }
        if (task.failure.kind != Value::Kind::Null) {
            gcRootEnv->define("$t" + std::to_string(i) + "_failure", task.failure, true);
        }
        if (task.state == TaskState::Completed || task.state == TaskState::Failed) {
            if (task.completionFuture.kind != Value::Kind::Null) {
                gcRootEnv->define("$t" + std::to_string(i) + "_future", task.completionFuture, true);
            }
        }
        if (task.waitingFuture.kind != Value::Kind::Null) {
            gcRootEnv->define("$t" + std::to_string(i) + "_wait", task.waitingFuture, true);
        }
        if (task.completionFuture.kind != Value::Kind::Null) {
            gcRootEnv->define("$t" + std::to_string(i) + "_owner", task.completionFuture, true);
        }
        if (task.pendingThrow) {
            gcRootEnv->define("$t" + std::to_string(i) + "_throw", task.pendingThrownValue, true);
        }
        for (size_t j = 0; j < task.operandStack.size(); ++j) {
            gcRootEnv->define("$s" + std::to_string(i) + "_" + std::to_string(j), task.operandStack[j], true);
        }
        for (size_t j = 0; j < task.frames.size(); ++j) {
            for (size_t k = 0; k < task.frames[j].locals.size(); ++k) {
                gcRootEnv->define(
                    "$f" + std::to_string(i) + "_" + std::to_string(j) + "_" + std::to_string(k),
                    task.frames[j].locals[k],
                    true);
            }
        }
    }
}

void BytecodeVM::collectGarbage() {
    syncRoots();
    heap.collect(environmentRoots);
}

[[noreturn]] void BytecodeVM::runtimeError(const SourceLocation& loc, const std::string& message) const {
    throw std::runtime_error(
        loc.filename + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column) + ": " + message);
}

}  // namespace smush
