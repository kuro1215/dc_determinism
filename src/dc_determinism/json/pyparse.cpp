#include "json/pyparse.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace dc_determinism {

void JsonKeepTree::add(std::initializer_list<const char*> path) {
    Node* node = &root_;
    for (const char* key : path) node = &node->children[key];
    node->all = true;
}

namespace {

struct ParseError {
    const char* message;
    std::size_t position;
};

constexpr int kMaxDepth = 1000;

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text.data()), size_(text.size()) {}

    void parse(Json& out, const JsonKeepTree* keep) {
        if (size_ >= 3 && static_cast<unsigned char>(text_[0]) == 0xef &&
            static_cast<unsigned char>(text_[1]) == 0xbb && static_cast<unsigned char>(text_[2]) == 0xbf) {
            throw ParseError{"Unexpected UTF-8 BOM (decode using utf-8-sig)", 0};
        }
        skip_whitespace();
        const JsonKeepTree::Node* node = nullptr;
        if (keep != nullptr && !keep->root().all) node = &keep->root();
        value(&out, node, 0);
        skip_whitespace();
        if (pos_ != size_) throw ParseError{"Extra data", pos_};
    }

private:
    [[noreturn]] void fail(const char* message) const { throw ParseError{message, pos_}; }

    void skip_whitespace() {
        while (pos_ < size_) {
            char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    bool literal(const char* word) {
        std::size_t length = std::strlen(word);
        if (pos_ + length > size_ || std::memcmp(text_ + pos_, word, length) != 0) return false;
        pos_ += length;
        return true;
    }

    // `target == nullptr`: validate and discard. `node != nullptr`: only the listed members of an
    // object are kept (everything below a leaf node, and non-objects, are kept whole).
    void value(Json* target, const JsonKeepTree::Node* node, int depth) {
        if (pos_ >= size_) fail("Expecting value");
        if (depth > kMaxDepth) fail("maximum recursion depth exceeded");
        char c = text_[pos_];
        switch (c) {
            case '"': {
                if (target != nullptr) {
                    std::string text;
                    string(&text);
                    *target = std::move(text);
                } else {
                    string(nullptr);
                }
                return;
            }
            case '{': object(target, node, depth); return;
            case '[': array(target, depth); return;
            case 'n':
                if (literal("null")) {
                    if (target != nullptr) *target = nullptr;
                    return;
                }
                break;
            case 't':
                if (literal("true")) {
                    if (target != nullptr) *target = true;
                    return;
                }
                break;
            case 'f':
                if (literal("false")) {
                    if (target != nullptr) *target = false;
                    return;
                }
                break;
            case 'N':
                if (literal("NaN")) {
                    if (target != nullptr) *target = std::numeric_limits<double>::quiet_NaN();
                    return;
                }
                break;
            case 'I':
                if (literal("Infinity")) {
                    if (target != nullptr) *target = std::numeric_limits<double>::infinity();
                    return;
                }
                break;
            case '-':
                if (literal("-Infinity")) {
                    if (target != nullptr) *target = -std::numeric_limits<double>::infinity();
                    return;
                }
                number(target);
                return;
            default:
                if (c >= '0' && c <= '9') {
                    number(target);
                    return;
                }
                break;
        }
        fail("Expecting value");
    }

    static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    void number(Json* target) {
        std::size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        if (pos_ >= size_) throw ParseError{"Expecting value", start};
        if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            ++pos_;
            while (pos_ < size_ && is_digit(text_[pos_])) ++pos_;
        } else if (text_[pos_] == '0') {
            ++pos_;
        } else {
            throw ParseError{"Expecting value", start};
        }
        bool is_float = false;
        if (pos_ + 1 < size_ && text_[pos_] == '.' && is_digit(text_[pos_ + 1])) {
            pos_ += 2;
            while (pos_ < size_ && is_digit(text_[pos_])) ++pos_;
            is_float = true;
        }
        if (pos_ < size_ && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            std::size_t cursor = pos_ + 1;
            if (cursor < size_ && (text_[cursor] == '+' || text_[cursor] == '-')) ++cursor;
            if (cursor < size_ && is_digit(text_[cursor])) {
                while (cursor < size_ && is_digit(text_[cursor])) ++cursor;
                pos_ = cursor;
                is_float = true;
            }
        }
        if (target == nullptr) return;
        const char* begin = text_ + start;
        const char* end = text_ + pos_;
        if (!is_float) {
            std::int64_t signed_value = 0;
            std::from_chars_result result = std::from_chars(begin, end, signed_value);
            if (result.ec == std::errc() && result.ptr == end) {
                *target = signed_value;
                return;
            }
            if (*begin != '-') {
                std::uint64_t unsigned_value = 0;
                result = std::from_chars(begin, end, unsigned_value);
                if (result.ec == std::errc() && result.ptr == end) {
                    *target = unsigned_value;
                    return;
                }
            }
            // Beyond 64 bits: Python keeps an exact int; only float() / range checks follow.
        }
        char buffer[64];
        std::size_t length = static_cast<std::size_t>(end - begin);
        if (length < sizeof(buffer)) {
            std::memcpy(buffer, begin, length);
            buffer[length] = '\0';
            *target = std::strtod(buffer, nullptr);
        } else {
            std::string copy(begin, length);
            *target = std::strtod(copy.c_str(), nullptr);
        }
    }

    static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool read_hex4(std::size_t at, unsigned& code) const {
        if (at + 4 > size_) return false;
        unsigned result = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            int digit = hex_value(text_[at + i]);
            if (digit < 0) return false;
            result = result * 16 + static_cast<unsigned>(digit);
        }
        code = result;
        return true;
    }

    static void append_utf8(std::string* out, unsigned code) {
        if (code < 0x80) {
            out->push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out->push_back(static_cast<char>(0xc0 | (code >> 6)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out->push_back(static_cast<char>(0xe0 | (code >> 12)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out->push_back(static_cast<char>(0xf0 | (code >> 18)));
            out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    // Parses a string starting at the opening quote; `out` may be null (validate only).
    void string(std::string* out) {
        std::size_t start = pos_;
        ++pos_;
        for (;;) {
            std::size_t run = pos_;
            while (pos_ < size_) {
                unsigned char c = static_cast<unsigned char>(text_[pos_]);
                if (c == '"' || c == '\\' || c < 0x20) break;
                ++pos_;
            }
            if (out != nullptr && pos_ > run) out->append(text_ + run, pos_ - run);
            if (pos_ >= size_) throw ParseError{"Unterminated string starting at", start};
            char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return;
            }
            if (c != '\\') fail("Invalid control character at");
            ++pos_;
            if (pos_ >= size_) throw ParseError{"Unterminated string starting at", start};
            char escape = text_[pos_];
            switch (escape) {
                case '"': if (out) out->push_back('"'); ++pos_; break;
                case '\\': if (out) out->push_back('\\'); ++pos_; break;
                case '/': if (out) out->push_back('/'); ++pos_; break;
                case 'b': if (out) out->push_back('\b'); ++pos_; break;
                case 'f': if (out) out->push_back('\f'); ++pos_; break;
                case 'n': if (out) out->push_back('\n'); ++pos_; break;
                case 'r': if (out) out->push_back('\r'); ++pos_; break;
                case 't': if (out) out->push_back('\t'); ++pos_; break;
                case 'u': {
                    unsigned code = 0;
                    if (!read_hex4(pos_ + 1, code)) throw ParseError{"Invalid \\uXXXX escape", pos_ - 1};
                    pos_ += 5;
                    if (code >= 0xd800 && code <= 0xdbff && pos_ + 6 <= size_ && text_[pos_] == '\\' &&
                        text_[pos_ + 1] == 'u') {
                        unsigned low = 0;
                        if (read_hex4(pos_ + 2, low) && low >= 0xdc00 && low <= 0xdfff) {
                            code = 0x10000 + (((code - 0xd800) << 10) | (low - 0xdc00));
                            pos_ += 6;
                        }
                    }
                    if (out != nullptr) append_utf8(out, code);
                    break;
                }
                default: throw ParseError{"Invalid \\escape", pos_ - 1};
            }
        }
    }

    void array(Json* target, int depth) {
        ++pos_;
        if (target != nullptr) *target = Json::array();
        skip_whitespace();
        if (pos_ < size_ && text_[pos_] == ']') {
            ++pos_;
            return;
        }
        for (;;) {
            Json* element = nullptr;
            if (target != nullptr) {
                target->push_back(Json());
                element = &target->back();
            }
            skip_whitespace();
            value(element, nullptr, depth + 1);
            skip_whitespace();
            if (pos_ >= size_) fail("Expecting ',' delimiter");
            char c = text_[pos_];
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == ']') {
                ++pos_;
                return;
            }
            fail("Expecting ',' delimiter");
        }
    }

    void object(Json* target, const JsonKeepTree::Node* node, int depth) {
        ++pos_;
        if (target != nullptr) *target = Json::object();
        skip_whitespace();
        if (pos_ < size_ && text_[pos_] == '}') {
            ++pos_;
            return;
        }
        for (;;) {
            skip_whitespace();
            if (pos_ >= size_ || text_[pos_] != '"') fail("Expecting property name enclosed in double quotes");
            std::string key;
            string(&key);
            skip_whitespace();
            if (pos_ >= size_ || text_[pos_] != ':') fail("Expecting ':' delimiter");
            ++pos_;
            skip_whitespace();
            Json* child = nullptr;
            const JsonKeepTree::Node* child_node = nullptr;
            if (target != nullptr) {
                if (node == nullptr) {
                    child = &(*target)[key];
                } else {
                    std::map<std::string, JsonKeepTree::Node>::const_iterator found = node->children.find(key);
                    if (found != node->children.end()) {
                        child = &(*target)[key];
                        if (!found->second.all) child_node = &found->second;
                    }
                }
            }
            value(child, child_node, depth + 1);
            skip_whitespace();
            if (pos_ >= size_) fail("Expecting ',' delimiter");
            char c = text_[pos_];
            if (c == ',') {
                ++pos_;
                continue;
            }
            if (c == '}') {
                ++pos_;
                return;
            }
            fail("Expecting ',' delimiter");
        }
    }

    const char* text_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

}  // namespace

bool py_json_loads(const std::string& text, Json& out, std::string& error, const JsonKeepTree* keep) {
    try {
        Parser parser(text);
        out = Json();
        parser.parse(out, keep);
        return true;
    } catch (const ParseError& failure) {
        error = std::string(failure.message) + ": char " + std::to_string(failure.position);
        return false;
    }
}

}  // namespace dc_determinism
