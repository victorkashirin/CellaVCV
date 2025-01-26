#include <cctype>
#include <cmath>
#include <cstdint>  // For uint32_t
#include <iostream>
#include <sstream>  // For std::ostringstream
#include <stdexcept>
#include <string>

class BytebeatParser {
   public:
    BytebeatParser(const std::string& expr) : expr(expr), pos(0) {}

    int parseAndEvaluate(uint32_t t, int a, int b, int c) {
        this->t = t;
        this->a = a;
        this->b = b;
        this->c = c;
        int result = parseConditional();
        skipWhitespace();
        if (pos < expr.size()) {
            throw std::runtime_error("Unexpected character at position " + std::to_string(pos));
            return 0;
        }
        return result;
    }

   private:
    std::string expr;
    size_t pos;
    uint32_t t;
    int a, b, c;

    // 10. Conditional (?:)
    int parseConditional() {
        int condition = parseBitwiseOR();
        skipWhitespace();
        if (match('?')) {
            int true_expr = parseConditional();
            if (!match(':')) {
                throw std::runtime_error("Expected ':' after '?' at position " + std::to_string(pos));
            }
            int false_expr = parseConditional();
            return condition ? true_expr : false_expr;
        }
        return condition;
    }

    // 9. Bitwise OR |
    int parseBitwiseOR() {
        int left = parseBitwiseXOR();
        while (true) {
            skipWhitespace();
            if (match('|')) {
                int right = parseBitwiseXOR();
                left |= right;
            } else {
                break;
            }
        }
        return left;
    }

    // 8. Bitwise XOR ^
    int parseBitwiseXOR() {
        int left = parseBitwiseAND();
        while (true) {
            skipWhitespace();
            if (match('^')) {
                int right = parseBitwiseAND();
                left ^= right;
            } else {
                break;
            }
        }
        return left;
    }

    // 7. Bitwise AND &
    int parseBitwiseAND() {
        int left = parseEquality();  // upd
        while (true) {
            skipWhitespace();
            if (match('&')) {
                int right = parseEquality();  // upd
                left &= right;
            } else {
                break;
            }
        }
        return left;
    }

    // 6. Equality == !=
    int parseEquality() {              // upd
        int left = parseRelational();  // upd
        while (true) {
            skipWhitespace();
            if (matchString("==")) {  // upd
                int right = parseRelational();
                left = (left == right) ? 1 : 0;
            } else if (matchString("!=")) {  // upd
                int right = parseRelational();
                left = (left != right) ? 1 : 0;
            } else {
                break;
            }
        }
        return left;
    }

    // 5. Relational < > <= >=
    int parseRelational() {
        int left = parseShift();
        while (true) {
            skipWhitespace();
            // Check multi-character operators first
            if (matchString("<=")) {  // upd
                int right = parseShift();
                left = (left <= right) ? 1 : 0;
            } else if (matchString(">=")) {  // upd
                int right = parseShift();
                left = (left >= right) ? 1 : 0;
            }
            // Next, check for possible shift operators (so we can break back to parseShift)
            else if (matchString("<<")) {  // upd
                // We found '<<' - belongs in parseShift, so revert position and break
                pos -= 2;  // Put it back so parseShift() can handle it
                break;
            } else if (matchString(">>")) {  // upd
                pos -= 2;                    // Put it back so parseShift() can handle it
                break;
            }
            // Finally, handle single-character < or >
            else if (match('<')) {
                if (match('<')) {
                    // We found '<<'; revert last consume, break to handle in parseShift
                    pos -= 1;
                    break;
                }
                int right = parseShift();
                left = (left < right) ? 1 : 0;
            } else if (match('>')) {
                if (match('>')) {
                    // We found '>>'; revert last consume, break to handle in parseShift
                    pos -= 1;
                    break;
                }
                int right = parseShift();
                left = (left > right) ? 1 : 0;
            } else {
                break;
            }
        }
        return left;
    }

    // 4. Shift << >>
    int parseShift() {
        int left = parseAdditive();
        while (true) {
            skipWhitespace();
            if (matchString("<<")) {
                int right = parseAdditive();
                left <<= right;
            } else if (matchString(">>")) {
                int right = parseAdditive();
                left >>= right;
            } else {
                break;
            }
        }
        return left;
    }

    // 3. Additive + -
    int parseAdditive() {
        int left = parseMultiplicative();
        while (true) {
            skipWhitespace();
            if (match('+')) {
                int right = parseMultiplicative();
                left += right;
            } else if (match('-')) {
                int right = parseMultiplicative();
                left -= right;
            } else {
                break;
            }
        }
        return left;
    }

    // 2. Multiplicative * / %
    int parseMultiplicative() {
        int left = parseUnary();
        while (true) {
            skipWhitespace();
            if (match('*')) {
                int right = parseUnary();
                left *= right;
            } else if (match('/')) {
                int right = parseUnary();
                if (right == 0) {
                    left = 0;
                } else {
                    left /= right;
                }
            } else if (match('%')) {
                int right = parseUnary();
                if (right == 0) {
                    left = 0;
                } else {
                    left %= right;
                }
            } else {
                break;
            }
        }
        return left;
    }

    // 1. Unary - ~
    int parseUnary() {
        skipWhitespace();
        if (match('-')) {
            return -parseUnary();
        } else if (match('~')) {
            return ~parseUnary();
        } else {
            return parsePrimary();
        }
    }

    // Primary
    int parsePrimary() {
        skipWhitespace();
        if (match('(')) {
            int value = parseConditional();
            if (!match(')')) {
                std::ostringstream oss;
                oss << "Expected closing parenthesis at position " << pos;
                throw std::runtime_error(oss.str());
            }
            return value;
        } else if (match('t')) {
            return static_cast<int>(t);
        } else if (match('a')) {
            return static_cast<int>(a);
        } else if (match('b')) {
            return static_cast<int>(b);
        } else if (match('c')) {
            return static_cast<int>(c);
        } else if (isdigit(peek())) {
            return parseNumber();
        } else {
            std::ostringstream oss;
            oss << "Unexpected character '" << peek() << "' at position " << pos;
            throw std::runtime_error(oss.str());
        }
    }

    int parseNumber() {
        skipWhitespace();
        int result = 0;
        while (isdigit(peek())) {
            result = result * 10 + (consume() - '0');
        }
        return result;
    }

    // Utility functions
    char peek() const {
        return pos < expr.size() ? expr[pos] : '\0';
    }

    char consume() {
        return pos < expr.size() ? expr[pos++] : '\0';
    }

    void skipWhitespace() {
        while (isspace(peek())) {
            consume();
        }
    }

    bool match(char expected) {
        if (peek() == expected) {
            consume();
            return true;
        }
        return false;
    }

    bool matchString(const std::string& expected) {
        size_t start = pos;
        for (char c : expected) {
            if (!match(c)) {
                pos = start;
                return false;
            }
        }
        return true;
    }
};

bool runTest(const std::string& expression, uint32_t t, int expected) {
    BytebeatParser parser(expression);
    int result = parser.parseAndEvaluate(t, 0, 0, 0);
    return (result == expected);
}

int main() {
    std::string expression;
    int t = 1234567;

    int test1 = t % (t >> 10 & t);
    std::cout << "Test 1: "
              << (runTest("t % (t >> 10 & t)", t, test1) ? "PASS" : "FAIL")
              << std::endl;

    return 0;
}

// MODEM: 100*((t<<2|t>>5|t^63)&(t<<10|t>>11))
