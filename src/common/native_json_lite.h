#pragma once
// native_json_lite.h — 三端共用的最小 JSON 解析器(只读、无依赖)。
//
// 用途:Android 没有 NSJSONSerialization,内容系统要读 App 基线清单(path→sha256/size)与目录信封;
// 解析结果是一棵简单值树,对象按插入顺序保存、查找取第一个匹配键(与 NSJSONSerialization 的重复键行为不同,
// 但生成物不含重复键)。数字一律 double(contentVersion 14 位、文件大小都在 2^53 内)。
// 安全边界:最大嵌套深度 64;拒绝未转义的控制字符;\u 转义含代理对 → UTF-8;不接受注释/尾逗号/NaN。
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace jl {

struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool isNull() const { return kind == Null; }
    bool isBool() const { return kind == Bool; }
    bool isNumber() const { return kind == Number; }
    bool isString() const { return kind == String; }
    bool isArray() const { return kind == Array; }
    bool isObject() const { return kind == Object; }
    const Value* get(const std::string& key) const {
        if (kind != Object) return nullptr;
        for (const auto& kv : object) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string stringOr(const std::string& fallback) const { return kind == String ? string : fallback; }
    double numberOr(double fallback) const { return kind == Number ? number : fallback; }
    bool boolOr(bool fallback) const { return kind == Bool ? boolean : fallback; }
    // 便捷:对象里键为字符串/数字时取值
    std::string stringAt(const std::string& key, const std::string& fallback = "") const {
        const Value* v = get(key); return v ? v->stringOr(fallback) : fallback;
    }
    double numberAt(const std::string& key, double fallback = 0) const {
        const Value* v = get(key); return v ? v->numberOr(fallback) : fallback;
    }
};

class Parser {
public:
    Parser(const std::string& text, size_t maxDepth) : s_(text), maxDepth_(maxDepth) {}
    bool parse(Value& out) {
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        if (i_ != s_.size()) return fail("trailing characters");
        return true;
    }
    const std::string& error() const { return error_; }

private:
    const std::string& s_;
    size_t i_ = 0;
    size_t maxDepth_;
    std::string error_;

    bool fail(const char* why) { if (error_.empty()) error_ = std::string(why) + " at " + std::to_string(i_); return false; }
    void skipWs() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++; }
    bool consume(const char* literal) {
        size_t n = 0; while (literal[n]) n++;
        if (s_.compare(i_, n, literal) != 0) return false;
        i_ += n; return true;
    }
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    }
    bool parseHex4(uint32_t& out) {
        if (i_ + 4 > s_.size()) return fail("bad \\u escape");
        out = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s_[i_ + k]; uint32_t v;
            if (c >= '0' && c <= '9') v = c - '0'; else if (c >= 'a' && c <= 'f') v = c - 'a' + 10; else if (c >= 'A' && c <= 'F') v = c - 'A' + 10; else return fail("bad \\u escape");
            out = (out << 4) | v;
        }
        i_ += 4; return true;
    }
    bool parseString(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return fail("expected string");
        i_++;
        while (true) {
            if (i_ >= s_.size()) return fail("unterminated string");
            const unsigned char c = (unsigned char)s_[i_];
            if (c == '"') { i_++; return true; }
            if (c < 0x20) return fail("control character in string");
            if (c == '\\') {
                i_++;
                if (i_ >= s_.size()) return fail("bad escape");
                const char e = s_[i_++];
                switch (e) {
                    case '"': out.push_back('"'); break; case '\\': out.push_back('\\'); break; case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break; case 'f': out.push_back('\f'); break; case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break; case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp; if (!parseHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (!(i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u')) return fail("lone high surrogate");
                            i_ += 2; uint32_t lo; if (!parseHex4(lo)) return false;
                            if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate");
                        appendUtf8(out, cp); break;
                    }
                    default: return fail("bad escape");
                }
                continue;
            }
            out.push_back((char)c); i_++;
        }
    }
    bool parseNumber(double& out) {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') i_++;
        if (i_ >= s_.size()) return fail("bad number");
        if (s_[i_] == '0') i_++;
        else if (s_[i_] >= '1' && s_[i_] <= '9') { while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') i_++; }
        else return fail("bad number");
        if (i_ < s_.size() && s_[i_] == '.') { i_++; if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return fail("bad fraction"); while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') i_++; }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) { i_++; if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) i_++; if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return fail("bad exponent"); while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') i_++; }
        const std::string token = s_.substr(start, i_ - start);
        char* end = nullptr; out = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0') return fail("bad number");
        return true;
    }
    bool parseValue(Value& out, size_t depth) {
        if (depth > maxDepth_) return fail("nesting too deep");
        if (i_ >= s_.size()) return fail("unexpected end");
        const char c = s_[i_];
        if (c == '{') {
            i_++; out.kind = Value::Object; skipWs();
            if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
            while (true) {
                skipWs(); std::string key; if (!parseString(key)) return false;
                skipWs(); if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'"); i_++; skipWs();
                Value v; if (!parseValue(v, depth + 1)) return false;
                out.object.emplace_back(std::move(key), std::move(v));
                skipWs(); if (i_ >= s_.size()) return fail("unterminated object");
                if (s_[i_] == ',') { i_++; continue; }
                if (s_[i_] == '}') { i_++; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            i_++; out.kind = Value::Array; skipWs();
            if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
            while (true) {
                skipWs(); Value v; if (!parseValue(v, depth + 1)) return false;
                out.array.push_back(std::move(v));
                skipWs(); if (i_ >= s_.size()) return fail("unterminated array");
                if (s_[i_] == ',') { i_++; continue; }
                if (s_[i_] == ']') { i_++; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { out.kind = Value::String; return parseString(out.string); }
        if (c == 't') { if (!consume("true")) return fail("bad literal"); out.kind = Value::Bool; out.boolean = true; return true; }
        if (c == 'f') { if (!consume("false")) return fail("bad literal"); out.kind = Value::Bool; out.boolean = false; return true; }
        if (c == 'n') { if (!consume("null")) return fail("bad literal"); out.kind = Value::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) { out.kind = Value::Number; return parseNumber(out.number); }
        return fail("unexpected character");
    }
};

inline bool parse(const std::string& text, Value& out, std::string* error = nullptr, size_t maxDepth = 64) {
    Parser parser(text, maxDepth);
    out = Value();
    const bool ok = parser.parse(out);
    if (!ok && error) *error = parser.error();
    return ok;
}

}  // namespace jl
