#include <blackbox/query.hpp>

#include <blackbox/util/hex.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bb {
namespace {

constexpr std::size_t kMaxQuerySourceBytes = 4096;
constexpr std::size_t kMaxQueryTokens = 1024;
constexpr std::size_t kMaxQueryNodes = 512;
constexpr std::size_t kMaxQueryDepth = 32;
constexpr std::size_t kMaxQueryString = 512;

enum class Op { Eq, Ne, Lt, Le, Gt, Ge, Contains, NotContains };

enum class TokKind {
  End,
  Ident,
  String,
  Number,
  Duration,
  Op,
  LParen,
  RParen,
  LBracket,
  RBracket,
};

struct Token {
  TokKind kind = TokKind::End;
  std::string text;
  Op op = Op::Eq;
  bool is_float = false;
  bool negative = false;
  std::uint64_t magnitude = 0;
  double real = 0.0;
  std::uint64_t duration_ns = 0;
  std::size_t offset = 0;
};

std::string ascii_lower(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

bool ci_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') {
      x = static_cast<char>(x - 'A' + 'a');
    }
    if (y >= 'A' && y <= 'Z') {
      y = static_cast<char>(y - 'A' + 'a');
    }
    if (x != y) {
      return false;
    }
  }
  return true;
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  if (needle.size() > haystack.size()) {
    return false;
  }
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      char a = haystack[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') {
        a = static_cast<char>(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = static_cast<char>(b - 'A' + 'a');
      }
      if (a != b) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_ident_char(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '/' ||
         c == ':' || c == '-';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

class Lexer {
 public:
  explicit Lexer(std::string_view source) : source_(source) {}

  bool run(std::vector<Token>& tokens, std::string& error) {
    while (pos_ < source_.size()) {
      const char c = source_[pos_];
      if (is_space(c)) {
        ++pos_;
        continue;
      }
      if (tokens.size() >= kMaxQueryTokens) {
        return fail("too many tokens", pos_, error);
      }
      Token token;
      token.offset = pos_;
      if (c == '(') {
        token.kind = TokKind::LParen;
        ++pos_;
      } else if (c == ')') {
        token.kind = TokKind::RParen;
        ++pos_;
      } else if (c == '[') {
        token.kind = TokKind::LBracket;
        ++pos_;
      } else if (c == ']') {
        token.kind = TokKind::RBracket;
        ++pos_;
      } else if (c == '"' || c == '\'') {
        if (!lex_string(c, token, error)) {
          return false;
        }
      } else if (is_digit(c) || (c == '-' && pos_ + 1 < source_.size() &&
                                 is_digit(source_[pos_ + 1]))) {
        if (!lex_number(token, error)) {
          return false;
        }
      } else if (is_ident_start(c)) {
        lex_ident(token);
      } else if (!lex_operator(token, error)) {
        return false;
      }
      tokens.push_back(std::move(token));
    }
    Token end;
    end.kind = TokKind::End;
    end.offset = source_.size();
    tokens.push_back(std::move(end));
    return true;
  }

 private:
  bool fail(const std::string& message, std::size_t offset,
            std::string& error) const {
    error = "query: " + message + " at offset " + std::to_string(offset);
    return false;
  }

  bool lex_operator(Token& token, std::string& error) {
    const char c = source_[pos_];
    if (c == '=') {
      token.kind = TokKind::Op;
      token.op = Op::Eq;
      ++pos_;
      if (pos_ < source_.size() && source_[pos_] == '=') {
        ++pos_;
      }
      return true;
    }
    if (c == '!') {
      if (pos_ + 1 < source_.size() && source_[pos_ + 1] == '=') {
        token.kind = TokKind::Op;
        token.op = Op::Ne;
        pos_ += 2;
        return true;
      }
      if (pos_ + 1 < source_.size() && source_[pos_ + 1] == '~') {
        token.kind = TokKind::Op;
        token.op = Op::NotContains;
        pos_ += 2;
        return true;
      }
      return fail("expected '=' or '~' after '!'", pos_, error);
    }
    if (c == '~') {
      token.kind = TokKind::Op;
      token.op = Op::Contains;
      ++pos_;
      return true;
    }
    if (c == '<' || c == '>') {
      token.kind = TokKind::Op;
      const bool less = c == '<';
      ++pos_;
      if (pos_ < source_.size() && source_[pos_] == '=') {
        token.op = less ? Op::Le : Op::Ge;
        ++pos_;
      } else {
        token.op = less ? Op::Lt : Op::Gt;
      }
      return true;
    }
    return fail("unexpected character", pos_, error);
  }

  bool lex_string(char quote, Token& token, std::string& error) {
    ++pos_;
    token.kind = TokKind::String;
    while (pos_ < source_.size()) {
      const char c = source_[pos_++];
      if (c == quote) {
        return true;
      }
      if (c == '\\') {
        if (pos_ >= source_.size()) {
          return fail("unterminated escape", pos_, error);
        }
        const char escaped = source_[pos_++];
        switch (escaped) {
          case '\\':
            token.text.push_back('\\');
            break;
          case '"':
            token.text.push_back('"');
            break;
          case '\'':
            token.text.push_back('\'');
            break;
          case 'n':
            token.text.push_back('\n');
            break;
          case 't':
            token.text.push_back('\t');
            break;
          case 'r':
            token.text.push_back('\r');
            break;
          default:
            return fail("unsupported escape sequence", pos_ - 1, error);
        }
      } else {
        if (c == '\0') {
          return fail("NUL byte in string", pos_ - 1, error);
        }
        token.text.push_back(c);
      }
      if (token.text.size() > kMaxQueryString) {
        return fail("string literal too long", token.offset, error);
      }
    }
    return fail("unterminated string literal", token.offset, error);
  }

  bool lex_number(Token& token, std::string& error) {
    const std::size_t start = pos_;
    if (source_[pos_] == '-') {
      token.negative = true;
      ++pos_;
    }
    std::uint64_t integer_part = 0;
    while (pos_ < source_.size() && is_digit(source_[pos_])) {
      const std::uint64_t digit = static_cast<std::uint64_t>(source_[pos_] - '0');
      if (integer_part > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
        return fail("number out of range", start, error);
      }
      integer_part = integer_part * 10 + digit;
      ++pos_;
    }
    double fraction = 0.0;
    double scale = 0.1;
    if (pos_ < source_.size() && source_[pos_] == '.') {
      token.is_float = true;
      ++pos_;
      if (pos_ >= source_.size() || !is_digit(source_[pos_])) {
        return fail("expected digits after decimal point", pos_, error);
      }
      while (pos_ < source_.size() && is_digit(source_[pos_])) {
        fraction += static_cast<double>(source_[pos_] - '0') * scale;
        scale *= 0.1;
        ++pos_;
      }
    }
    double real = static_cast<double>(integer_part) + fraction;
    if (token.negative) {
      real = -real;
    }
    token.magnitude = integer_part;
    token.real = real;
    token.kind = TokKind::Number;
    token.text = std::string(source_.substr(start, pos_ - start));

    std::uint64_t factor = 0;
    if (pos_ + 1 < source_.size() && source_.substr(pos_, 2) == "ns") {
      factor = 1;
      pos_ += 2;
    } else if (pos_ + 1 < source_.size() && source_.substr(pos_, 2) == "us") {
      factor = 1'000;
      pos_ += 2;
    } else if (pos_ + 1 < source_.size() && source_.substr(pos_, 2) == "ms") {
      factor = 1'000'000;
      pos_ += 2;
    } else if (pos_ < source_.size() && source_[pos_] == 's') {
      factor = 1'000'000'000;
      ++pos_;
    }
    if (factor != 0) {
      if (token.negative) {
        return fail("negative duration is not allowed", start, error);
      }
      const double total = real * static_cast<double>(factor);
      if (!std::isfinite(total) || total < 0.0 ||
          total > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return fail("duration out of range", start, error);
      }
      token.kind = TokKind::Duration;
      token.duration_ns = static_cast<std::uint64_t>(total);
    }
    if (pos_ < source_.size() && is_ident_char(source_[pos_])) {
      if (factor != 0) {
        return fail("unknown numeric suffix", pos_, error);
      }
      // A digit-leading word such as an unquoted hexadecimal trace id. Re-lex
      // it as a bare identifier instead of splitting it into number + word.
      pos_ = start;
      token = Token{};
      token.offset = start;
      while (pos_ < source_.size() && is_ident_char(source_[pos_])) {
        ++pos_;
      }
      token.kind = TokKind::Ident;
      token.text = std::string(source_.substr(start, pos_ - start));
      return true;
    }
    return true;
  }

  void lex_ident(Token& token) {
    const std::size_t start = pos_;
    ++pos_;
    while (pos_ < source_.size() && is_ident_char(source_[pos_])) {
      ++pos_;
    }
    token.kind = TokKind::Ident;
    token.text = std::string(source_.substr(start, pos_ - start));
  }

  std::string_view source_;
  std::size_t pos_ = 0;
};

enum class Field {
  Any,
  Kind,
  Level,
  Status,
  Name,
  Message,
  Trace,
  Span,
  Parent,
  Correlation,
  Sequence,
  Cause,
  Duration,
  StateKey,
  StateOld,
  StateNew,
  Value,
  Attribute,
};

struct Literal {
  enum class Kind { String, Int, Float, Bool } kind = Kind::String;
  std::string text;
  std::int64_t integer = 0;
  double real = 0.0;
  bool boolean = false;
};

struct Predicate {
  Field field = Field::Any;
  Op op = Op::Eq;
  std::string attr_key;
  std::string text;
  Literal literal;
  std::uint64_t number = 0;
  int ordinal = 0;
  TraceId trace;
  bool trace_exact = false;
};

}  // namespace

struct Query::Node {
  enum class Type { And, Or, Not, Predicate } type = Type::Predicate;
  std::vector<std::shared_ptr<const Node>> children;
  Predicate predicate;
};

namespace {

bool is_string_op(Op op) {
  return op == Op::Eq || op == Op::Ne || op == Op::Contains ||
         op == Op::NotContains;
}

bool is_numeric_op(Op op) {
  return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le ||
         op == Op::Gt || op == Op::Ge;
}

class QueryParser {
 public:
  QueryParser(std::vector<Token> tokens, std::string& error)
      : tokens_(std::move(tokens)), error_(error) {}

  std::shared_ptr<const Query::Node> parse() {
    if (peek().kind == TokKind::End) {
      return nullptr;
    }
    auto root = parse_or(0);
    if (root == nullptr || failed_) {
      return nullptr;
    }
    if (peek().kind != TokKind::End) {
      fail("unexpected trailing input");
      return nullptr;
    }
    return root;
  }

  bool failed() const { return failed_; }

 private:
  const Token& peek() const {
    return tokens_[index_ < tokens_.size() ? index_ : tokens_.size() - 1];
  }
  const Token& take() {
    const Token& token = peek();
    if (index_ + 1 < tokens_.size()) {
      ++index_;
    }
    return token;
  }
  bool keyword(std::string_view word) {
    if (peek().kind == TokKind::Ident && ci_equal(peek().text, word)) {
      take();
      return true;
    }
    return false;
  }
  void fail(const std::string& message) {
    if (!failed_) {
      error_ = "query: " + message + " at offset " +
               std::to_string(peek().offset);
    }
    failed_ = true;
  }

  std::shared_ptr<Query::Node> make_node(Query::Node::Type type) {
    if (++nodes_ > kMaxQueryNodes) {
      fail("too many query nodes");
      return nullptr;
    }
    auto node = std::make_shared<Query::Node>();
    node->type = type;
    return node;
  }

  std::shared_ptr<const Query::Node> parse_or(std::size_t depth) {
    auto left = parse_and(depth);
    if (left == nullptr) {
      return nullptr;
    }
    while (keyword("or")) {
      auto right = parse_and(depth);
      if (right == nullptr) {
        return nullptr;
      }
      auto node = make_node(Query::Node::Type::Or);
      if (node == nullptr) {
        return nullptr;
      }
      node->children.push_back(left);
      node->children.push_back(right);
      left = std::move(node);
    }
    return left;
  }

  std::shared_ptr<const Query::Node> parse_and(std::size_t depth) {
    auto left = parse_unary(depth);
    if (left == nullptr) {
      return nullptr;
    }
    while (keyword("and")) {
      auto right = parse_unary(depth);
      if (right == nullptr) {
        return nullptr;
      }
      auto node = make_node(Query::Node::Type::And);
      if (node == nullptr) {
        return nullptr;
      }
      node->children.push_back(left);
      node->children.push_back(right);
      left = std::move(node);
    }
    return left;
  }

  std::shared_ptr<const Query::Node> parse_unary(std::size_t depth) {
    if (depth > kMaxQueryDepth) {
      fail("query nesting is too deep");
      return nullptr;
    }
    if (keyword("not")) {
      auto child = parse_unary(depth + 1);
      if (child == nullptr) {
        return nullptr;
      }
      auto node = make_node(Query::Node::Type::Not);
      if (node == nullptr) {
        return nullptr;
      }
      node->children.push_back(std::move(child));
      return node;
    }
    if (peek().kind == TokKind::LParen) {
      take();
      auto inner = parse_or(depth + 1);
      if (inner == nullptr) {
        return nullptr;
      }
      if (peek().kind != TokKind::RParen) {
        fail("expected ')'");
        return nullptr;
      }
      take();
      return inner;
    }
    return parse_predicate();
  }

  bool parse_field(Field& field, std::string& attr_key) {
    if (peek().kind != TokKind::Ident) {
      fail("expected a field name");
      return false;
    }
    const std::string raw = take().text;
    const std::string name = ascii_lower(raw);
    if (name == "attr") {
      if (peek().kind != TokKind::LBracket) {
        fail("expected '[' after attr");
        return false;
      }
      take();
      if (peek().kind != TokKind::String && peek().kind != TokKind::Ident) {
        fail("expected attribute key");
        return false;
      }
      attr_key = take().text;
      if (peek().kind != TokKind::RBracket) {
        fail("expected ']'");
        return false;
      }
      take();
      if (!valid_attribute_key(attr_key)) {
        fail("invalid attribute key");
        return false;
      }
      field = Field::Attribute;
      return true;
    }
    if (name.rfind("attr.", 0) == 0) {
      attr_key = raw.substr(5);
      if (!valid_attribute_key(attr_key)) {
        fail("invalid attribute key");
        return false;
      }
      field = Field::Attribute;
      return true;
    }
    if (name == "any" || name == "text") {
      field = Field::Any;
    } else if (name == "kind") {
      field = Field::Kind;
    } else if (name == "level") {
      field = Field::Level;
    } else if (name == "status") {
      field = Field::Status;
    } else if (name == "name") {
      field = Field::Name;
    } else if (name == "message" || name == "msg") {
      field = Field::Message;
    } else if (name == "trace" || name == "trace_id" || name == "traceid") {
      field = Field::Trace;
    } else if (name == "span" || name == "span_id" || name == "spanid") {
      field = Field::Span;
    } else if (name == "parent" || name == "parent_span") {
      field = Field::Parent;
    } else if (name == "correlation" || name == "corr" ||
               name == "correlation_id") {
      field = Field::Correlation;
    } else if (name == "seq" || name == "sequence") {
      field = Field::Sequence;
    } else if (name == "cause" || name == "cause_seq") {
      field = Field::Cause;
    } else if (name == "duration" || name == "elapsed") {
      field = Field::Duration;
    } else if (name == "state" || name == "state_key" || name == "statekey") {
      field = Field::StateKey;
    } else if (name == "state.old" || name == "state_old") {
      field = Field::StateOld;
    } else if (name == "state.new" || name == "state_new") {
      field = Field::StateNew;
    } else if (name == "value") {
      field = Field::Value;
    } else {
      fail("unknown field '" + name + "'");
      return false;
    }
    return true;
  }

  bool parse_literal(const Token& token, Literal& literal) {
    if (token.kind == TokKind::String) {
      literal.kind = Literal::Kind::String;
      literal.text = token.text;
      return true;
    }
    if (token.kind == TokKind::Ident) {
      if (ci_equal(token.text, "true")) {
        literal.kind = Literal::Kind::Bool;
        literal.boolean = true;
        return true;
      }
      if (ci_equal(token.text, "false")) {
        literal.kind = Literal::Kind::Bool;
        literal.boolean = false;
        return true;
      }
      literal.kind = Literal::Kind::String;
      literal.text = token.text;
      return true;
    }
    if (token.kind == TokKind::Duration) {
      if (token.duration_ns >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        fail("integer literal out of range");
        return false;
      }
      literal.kind = Literal::Kind::Int;
      literal.integer = static_cast<std::int64_t>(token.duration_ns);
      return true;
    }
    if (token.kind == TokKind::Number) {
      if (token.is_float) {
        if (!std::isfinite(token.real)) {
          fail("floating point literal out of range");
          return false;
        }
        literal.kind = Literal::Kind::Float;
        literal.real = token.real;
        return true;
      }
      if (token.negative) {
        if (token.magnitude >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
                1ull) {
          fail("integer literal out of range");
          return false;
        }
        literal.kind = Literal::Kind::Int;
        if (token.magnitude ==
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
                1ull) {
          literal.integer = std::numeric_limits<std::int64_t>::min();
        } else {
          literal.integer = -static_cast<std::int64_t>(token.magnitude);
        }
        return true;
      }
      if (token.magnitude >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        fail("integer literal out of range");
        return false;
      }
      literal.kind = Literal::Kind::Int;
      literal.integer = static_cast<std::int64_t>(token.magnitude);
      return true;
    }
    fail("expected a value");
    return false;
  }

  std::shared_ptr<const Query::Node> parse_predicate() {
    Predicate predicate;
    if (!parse_field(predicate.field, predicate.attr_key)) {
      return nullptr;
    }
    if (peek().kind != TokKind::Op) {
      fail("expected an operator");
      return nullptr;
    }
    predicate.op = take().op;
    if (peek().kind == TokKind::End) {
      fail("expected a value");
      return nullptr;
    }
    const Token value_token = take();
    predicate.text = value_token.text;

    switch (predicate.field) {
      case Field::Kind:
      case Field::Level:
      case Field::Status: {
        if (!is_numeric_op(predicate.op)) {
          fail("kind, level and status do not support ~");
          return nullptr;
        }
        if (value_token.kind != TokKind::Ident &&
            value_token.kind != TokKind::String) {
          fail("expected a name");
          return nullptr;
        }
        const std::string name = ascii_lower(value_token.text);
        if (predicate.field == Field::Kind) {
          RecordKind kind;
          if (!record_kind_from_string(name, kind)) {
            fail("unknown record kind");
            return nullptr;
          }
          predicate.ordinal = static_cast<int>(kind);
        } else if (predicate.field == Field::Level) {
          Level level;
          if (!level_from_string(name, level)) {
            fail("unknown level");
            return nullptr;
          }
          predicate.ordinal = static_cast<int>(level);
        } else {
          Status status;
          if (!status_from_string(name, status)) {
            fail("unknown status");
            return nullptr;
          }
          predicate.ordinal = static_cast<int>(status);
        }
        break;
      }
      case Field::Trace: {
        if (predicate.op == Op::Contains || predicate.op == Op::NotContains) {
          if (value_token.text.empty() ||
              value_token.text.find_first_not_of("0123456789abcdefABCDEF") !=
                  std::string::npos) {
            fail("trace search must be hexadecimal");
            return nullptr;
          }
          predicate.text = ascii_lower(value_token.text);
        } else if (predicate.op == Op::Eq || predicate.op == Op::Ne) {
          const auto trace = TraceId::from_hex(value_token.text);
          if (!trace.has_value()) {
            fail("trace must be 32 hexadecimal digits and non-zero");
            return nullptr;
          }
          predicate.trace = *trace;
          predicate.trace_exact = true;
        } else {
          fail("trace only supports =, !=, ~ and !~");
          return nullptr;
        }
        break;
      }
      case Field::Span:
      case Field::Parent:
      case Field::Correlation:
      case Field::Sequence:
      case Field::Cause:
      case Field::Duration: {
        if (!is_numeric_op(predicate.op)) {
          fail("numeric fields do not support ~");
          return nullptr;
        }
        if (value_token.kind == TokKind::Number) {
          if (value_token.is_float || value_token.negative) {
            fail("expected a non-negative integer");
            return nullptr;
          }
          predicate.number = value_token.magnitude;
        } else if (value_token.kind == TokKind::Duration &&
                   predicate.field == Field::Duration) {
          predicate.number = value_token.duration_ns;
        } else {
          fail("expected a non-negative integer");
          return nullptr;
        }
        break;
      }
      case Field::Value:
      case Field::Attribute: {
        if (!parse_literal(value_token, predicate.literal)) {
          return nullptr;
        }
        if ((predicate.op == Op::Contains ||
             predicate.op == Op::NotContains) &&
            predicate.literal.kind != Literal::Kind::String) {
          fail("~ requires a string value");
          return nullptr;
        }
        break;
      }
      case Field::Any:
      case Field::Name:
      case Field::Message:
      case Field::StateKey:
      case Field::StateOld:
      case Field::StateNew: {
        if (!is_string_op(predicate.op)) {
          fail("string fields only support =, !=, ~ and !~");
          return nullptr;
        }
        if (value_token.kind != TokKind::String &&
            value_token.kind != TokKind::Ident &&
            value_token.kind != TokKind::Number) {
          fail("expected a string");
          return nullptr;
        }
        break;
      }
    }

    auto node = make_node(Query::Node::Type::Predicate);
    if (node == nullptr) {
      return nullptr;
    }
    node->predicate = std::move(predicate);
    return node;
  }

  std::vector<Token> tokens_;
  std::string& error_;
  std::size_t index_ = 0;
  std::size_t nodes_ = 0;
  bool failed_ = false;
};

bool apply_cmp(int cmp, Op op) {
  switch (op) {
    case Op::Eq:
      return cmp == 0;
    case Op::Ne:
      return cmp != 0;
    case Op::Lt:
      return cmp < 0;
    case Op::Le:
      return cmp <= 0;
    case Op::Gt:
      return cmp > 0;
    case Op::Ge:
      return cmp >= 0;
    case Op::Contains:
    case Op::NotContains:
      return false;
  }
  return false;
}

int compare_u64(std::uint64_t a, std::uint64_t b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}
int compare_i64(std::int64_t a, std::int64_t b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}
int compare_double(double a, double b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

bool compare_string(const std::string_view actual, const Predicate& predicate) {
  if (predicate.op == Op::Contains) {
    return contains_ci(actual, predicate.text);
  }
  if (predicate.op == Op::NotContains) {
    return !contains_ci(actual, predicate.text);
  }
  const int cmp = actual < predicate.text
                      ? -1
                      : (actual > predicate.text ? 1 : 0);
  return apply_cmp(cmp, predicate.op);
}

bool compare_stored(const StoredValue& stored, const Predicate& predicate) {
  const Literal& literal = predicate.literal;
  switch (stored.type) {
    case ValueType::Null:
      return false;
    case ValueType::String:
      if (literal.kind != Literal::Kind::String) {
        return false;
      }
      return compare_string(stored_text(stored), predicate);
    case ValueType::Bool: {
      if (literal.kind != Literal::Kind::Bool) {
        return false;
      }
      const int cmp = compare_u64(stored.boolean ? 1u : 0u,
                                  literal.boolean ? 1u : 0u);
      return apply_cmp(cmp, predicate.op);
    }
    case ValueType::Int:
      if (literal.kind == Literal::Kind::Int) {
        return apply_cmp(compare_i64(stored.integer, literal.integer),
                         predicate.op);
      }
      if (literal.kind == Literal::Kind::Float) {
        return apply_cmp(
            compare_double(static_cast<double>(stored.integer), literal.real),
            predicate.op);
      }
      return false;
    case ValueType::Float:
      if (literal.kind == Literal::Kind::Int) {
        return apply_cmp(
            compare_double(stored.real, static_cast<double>(literal.integer)),
            predicate.op);
      }
      if (literal.kind == Literal::Kind::Float) {
        return apply_cmp(compare_double(stored.real, literal.real),
                         predicate.op);
      }
      return false;
  }
  return false;
}

const StoredAttribute* find_attribute(const Record& record,
                                      std::string_view key) {
  for (std::size_t i = 0; i < record.attr_count; ++i) {
    if (stored_key(record.attrs[i]) == key) {
      return &record.attrs[i];
    }
  }
  return nullptr;
}

bool any_text_matches(const Record& record, const Predicate& predicate) {
  bool any_equal = false;
  bool any_contains = false;
  const auto visit = [&](std::string_view text) {
    if (text == predicate.text) {
      any_equal = true;
    }
    if (contains_ci(text, predicate.text)) {
      any_contains = true;
    }
  };
  visit(record_message(record));
  visit(record_name(record));
  visit(record_state_key(record));
  visit(to_string(record.kind));
  visit(to_string(record.level));
  visit(to_string(record.status));
  for (std::size_t i = 0; i < record.attr_count; ++i) {
    visit(stored_key(record.attrs[i]));
    if (record.attrs[i].value.type == ValueType::String) {
      visit(stored_text(record.attrs[i].value));
    }
  }
  switch (predicate.op) {
    case Op::Eq:
      return any_equal;
    case Op::Ne:
      return !any_equal;
    case Op::Contains:
      return any_contains;
    case Op::NotContains:
      return !any_contains;
    default:
      return false;
  }
}

bool evaluate_predicate(const Predicate& predicate, const Record& record) {
  switch (predicate.field) {
    case Field::Kind:
      return apply_cmp(compare_i64(static_cast<int>(record.kind),
                                   predicate.ordinal),
                       predicate.op);
    case Field::Level:
      return apply_cmp(compare_i64(static_cast<int>(record.level),
                                   predicate.ordinal),
                       predicate.op);
    case Field::Status:
      return apply_cmp(compare_i64(static_cast<int>(record.status),
                                   predicate.ordinal),
                       predicate.op);
    case Field::Name:
      return compare_string(record_name(record), predicate);
    case Field::Message:
      return compare_string(record_message(record), predicate);
    case Field::StateKey:
      return compare_string(record_state_key(record), predicate);
    case Field::StateOld: {
      const std::string value = owned_value(record.state_old).display();
      return compare_string(value, predicate);
    }
    case Field::StateNew: {
      const std::string value = owned_value(record.state_new).display();
      return compare_string(value, predicate);
    }
    case Field::Any:
      return any_text_matches(record, predicate);
    case Field::Trace:
      if (predicate.trace_exact) {
        const bool equal = record.trace == predicate.trace;
        return predicate.op == Op::Eq ? equal : !equal;
      } else {
        const std::string hex = record.trace.to_hex();
        if (predicate.op == Op::Contains) {
          return contains_ci(hex, predicate.text);
        }
        return !contains_ci(hex, predicate.text);
      }
    case Field::Span:
      return apply_cmp(compare_u64(record.span, predicate.number),
                       predicate.op);
    case Field::Parent:
      return apply_cmp(compare_u64(record.parent, predicate.number),
                       predicate.op);
    case Field::Correlation:
      return apply_cmp(compare_u64(record.correlation, predicate.number),
                       predicate.op);
    case Field::Sequence:
      return apply_cmp(compare_u64(record.seq, predicate.number),
                       predicate.op);
    case Field::Cause:
      return apply_cmp(compare_u64(record.cause_seq, predicate.number),
                       predicate.op);
    case Field::Duration:
      return apply_cmp(
          compare_u64(static_cast<std::uint64_t>(record.duration_ns),
                      predicate.number),
          predicate.op);
    case Field::Value:
      return compare_stored(record.value, predicate);
    case Field::Attribute: {
      const StoredAttribute* attribute =
          find_attribute(record, predicate.attr_key);
      if (attribute == nullptr) {
        return false;
      }
      return compare_stored(attribute->value, predicate);
    }
  }
  return false;
}

bool evaluate_node(const Query::Node& node, const Record& record) {
  switch (node.type) {
    case Query::Node::Type::Predicate:
      return evaluate_predicate(node.predicate, record);
    case Query::Node::Type::Not:
      return !evaluate_node(*node.children[0], record);
    case Query::Node::Type::And:
      for (const auto& child : node.children) {
        if (!evaluate_node(*child, record)) {
          return false;
        }
      }
      return true;
    case Query::Node::Type::Or:
      for (const auto& child : node.children) {
        if (evaluate_node(*child, record)) {
          return true;
        }
      }
      return false;
  }
  return false;
}

}  // namespace

std::optional<Query> Query::parse(std::string_view source, std::string* error) {
  std::string local_error;
  std::string& err = error != nullptr ? *error : local_error;
  err.clear();
  if (source.size() > kMaxQuerySourceBytes) {
    err = "query: source exceeds 4096 bytes";
    return std::nullopt;
  }
  Lexer lexer(source);
  std::vector<Token> tokens;
  if (!lexer.run(tokens, err)) {
    return std::nullopt;
  }
  QueryParser parser(std::move(tokens), err);
  auto root = parser.parse();
  if (parser.failed()) {
    return std::nullopt;
  }
  Query query;
  query.root_ = std::move(root);
  query.source_ = std::string(source);
  return query;
}

bool Query::matches(const Record& record) const {
  if (root_ == nullptr) {
    return true;
  }
  return evaluate_node(*root_, record);
}

std::vector<Record> filter_records(std::span<const Record> records,
                                   const Query& query,
                                   std::size_t max_results) {
  std::vector<Record> out;
  const std::size_t cap = std::min(max_results, records.size());
  out.reserve(cap);
  for (const Record& record : records) {
    if (out.size() >= cap) {
      break;
    }
    if (query.matches(record)) {
      out.push_back(record);
    }
  }
  return out;
}

}  // namespace bb
