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
        int result = parseConditional();  // Start parsing with the conditional operator
        skipWhitespace();
        if (pos < expr.size()) {
            // Uncomment the following line for error handling
            // throw std::runtime_error("Unexpected character at position " + std::to_string(pos));
            return 0;
        }
        return result;
    }

   private:
    std::string expr;
    size_t pos;
    uint32_t t;
    int a, b, c;

    // Parse functions corresponding to C++ operator precedence

    // 10. Conditional (?:)
    int parseConditional() {
        int condition = parseBitwiseOR();
        skipWhitespace();
        if (match('?')) {
            int true_expr = parseConditional();  // Right-associative
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
        int left = parseRelational();  // Updated to call parseRelational
        while (true) {
            skipWhitespace();
            if (match('&')) {
                int right = parseRelational();
                left &= right;
            } else {
                break;
            }
        }
        return left;
    }

    // 6. Relational < > <= >=
    int parseRelational() {
        int left = parseShift();
        while (true) {
            skipWhitespace();
            if (match('<')) {
                if (match('<')) {  // Handle '<<' as shift, not relational
                    pos -= 1;      // Roll back one character
                    break;
                }
                int right = parseShift();
                left = (left < right) ? 1 : 0;
            } else if (match('>')) {
                if (match('>')) {  // Handle '>>' as shift, not relational
                    pos -= 1;      // Roll back one character
                    break;
                }
                int right = parseShift();
                left = (left > right) ? 1 : 0;
            } else if (matchString("<=")) {
                int right = parseShift();
                left = (left <= right) ? 1 : 0;
            } else if (matchString(">=")) {
                int right = parseShift();
                left = (left >= right) ? 1 : 0;
            } else {
                break;
            }
        }
        return left;
    }

    // 5. Shift << >>
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

    // 4. Additive + -
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

    // 3. Multiplicative * / %
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
                    left = 0;  // Handle division by zero
                } else {
                    left /= right;
                }
            } else if (match('%')) {
                int right = parseUnary();
                if (right == 0) {
                    left = 0;  // Handle modulo by zero
                } else {
                    left %= right;
                }
            } else {
                break;
            }
        }
        return left;
    }

    // 2. Unary - ~
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

    // 1. Primary expressions: numbers, 't', parenthesis
    int parsePrimary() {
        skipWhitespace();
        if (match('(')) {
            int value = parseConditional();  // Start with conditional inside parentheses
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
                pos = start;  // Roll back if match fails
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

    // std::cout << "Enter bytebeat expression: ";
    // std::getline(std::cin, expression);

    // std::cout << "Enter time variable t: ";
    // std::cin >> t;

    // try {
    //     BytebeatParser parser(expression);
    //     int result = parser.parseAndEvaluate(t);
    //     std::cout << "Result: " << result << std::endl;
    // } catch (const std::exception& e) {
    //     std::cerr << "Error: " << e.what() << std::endl;
    // }

    return 0;
}

// MODEM: 100*((t<<2|t>>5|t^63)&(t<<10|t>>11))