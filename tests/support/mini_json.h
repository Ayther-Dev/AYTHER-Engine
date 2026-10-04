// Minimal JSON reader for spec 002 tests: enough to validate the render
// probe's output against the contracts (objects, arrays, strings, numbers,
// booleans, null). Not a general-purpose parser: no \u escapes beyond ASCII.
#pragma once

#include <cctype>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ayther::test::json {

struct Value {
  enum class Kind { null, boolean, number, string, array, object };
  Kind kind = Kind::null;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<Value> items;
  std::map<std::string, Value> fields;

  [[nodiscard]] bool is(Kind k) const { return kind == k; }
  [[nodiscard]] const Value *get(const std::string &key) const {
    const auto it = fields.find(key);
    return it == fields.end() ? nullptr : &it->second;
  }
};

class Parser {
public:
  explicit Parser(std::string_view text) : s_(text) {}

  std::optional<Value> parse() {
    std::optional<Value> v = value();
    skip();
    if (!v || pos_ != s_.size())
      return std::nullopt;
    return v;
  }

private:
  std::string_view s_;
  std::size_t pos_ = 0;

  void skip() {
    while (pos_ < s_.size() &&
           std::isspace(static_cast<unsigned char>(s_[pos_])) != 0)
      ++pos_;
  }
  bool eat(char c) {
    skip();
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  bool literal(std::string_view word) {
    if (s_.substr(pos_, word.size()) != word)
      return false;
    pos_ += word.size();
    return true;
  }
  std::optional<std::string> string() {
    if (!eat('"'))
      return std::nullopt;
    std::string out;
    while (pos_ < s_.size() && s_[pos_] != '"') {
      char c = s_[pos_++];
      if (c == '\\') {
        if (pos_ >= s_.size())
          return std::nullopt;
        const char e = s_[pos_++];
        switch (e) {
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        case 'r':
          c = '\r';
          break;
        case 'u':
          if (pos_ + 4 > s_.size())
            return std::nullopt;
          c = static_cast<char>(
              std::stoi(std::string(s_.substr(pos_, 4)), nullptr, 16));
          pos_ += 4;
          break;
        default:
          c = e;
        }
      }
      out.push_back(c);
    }
    if (pos_ >= s_.size())
      return std::nullopt;
    ++pos_;
    return out;
  }
  std::optional<Value> value() {
    skip();
    if (pos_ >= s_.size())
      return std::nullopt;
    Value v;
    const char c = s_[pos_];
    if (c == '{') {
      ++pos_;
      v.kind = Value::Kind::object;
      if (eat('}'))
        return v;
      do {
        std::optional<std::string> key = string();
        if (!key || !eat(':'))
          return std::nullopt;
        std::optional<Value> item = value();
        if (!item || v.fields.count(*key) != 0)
          return std::nullopt;
        v.fields.emplace(std::move(*key), std::move(*item));
      } while (eat(','));
      return eat('}') ? std::optional<Value>(std::move(v)) : std::nullopt;
    }
    if (c == '[') {
      ++pos_;
      v.kind = Value::Kind::array;
      if (eat(']'))
        return v;
      do {
        std::optional<Value> item = value();
        if (!item)
          return std::nullopt;
        v.items.push_back(std::move(*item));
      } while (eat(','));
      return eat(']') ? std::optional<Value>(std::move(v)) : std::nullopt;
    }
    if (c == '"') {
      std::optional<std::string> text = string();
      if (!text)
        return std::nullopt;
      v.kind = Value::Kind::string;
      v.text = std::move(*text);
      return v;
    }
    if (literal("true") || literal("false")) {
      v.kind = Value::Kind::boolean;
      v.boolean = s_[pos_ - 1] == 'e' && s_[pos_ - 2] == 'u';
      return v;
    }
    if (literal("null"))
      return v;
    const std::size_t start = pos_;
    while (pos_ < s_.size() &&
           (std::isdigit(static_cast<unsigned char>(s_[pos_])) != 0 ||
            s_[pos_] == '-' || s_[pos_] == '+' || s_[pos_] == '.' ||
            s_[pos_] == 'e' || s_[pos_] == 'E'))
      ++pos_;
    if (start == pos_)
      return std::nullopt;
    v.kind = Value::Kind::number;
    v.number = std::stod(std::string(s_.substr(start, pos_ - start)));
    return v;
  }
};

inline std::optional<Value> parse(std::string_view text) {
  return Parser(text).parse();
}

} // namespace ayther::test::json
