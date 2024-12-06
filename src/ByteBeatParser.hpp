#include <cctype>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

class BytebeatParser {
   public:
    BytebeatParser(const std::string& expr) : expr(expr), pos(0) {}

    int parseAndEvaluate(uint32_t t) {
        this->t = t;
        int result = parseExpression();
        if (pos < expr.size()) {
            return 0;
            // throw std::runtime_error("Unexpected character at position " + std::to_string(pos));
        }
        return result;
    }

   private:
    std::string expr;
    size_t pos;
    uint32_t t;

    int parseExpression() {
        int result = parseTerm();
        while (true) {
            skipWhitespace();
            if (match('+')) {
                result += parseTerm();
            } else if (match('-')) {
                result -= parseTerm();
            } else {
                break;
            }
        }
        return result;
    }

    int parseTerm() {
        uint32_t result = parseFactor();
        while (true) {
            skipWhitespace();
            if (match('*')) {
                result *= parseFactor();
            } else if (match('/')) {
                int divisor = parseFactor();
                if (divisor == 0) {
                    result = 0;
                } else {
                    result /= divisor;
                }
            } else if (match('%')) {
                int divisor = parseFactor();
                if (divisor == 0) {
                    result = 0;
                } else {
                    result %= divisor;
                }
            } else if (match('&')) {
                result &= parseFactor();
            } else if (match('|')) {
                result |= parseFactor();
            } else if (match('^')) {
                result ^= parseFactor();
            } else if (matchString("<<")) {
                result <<= parseFactor();
            } else if (matchString(">>")) {
                result >>= parseFactor();
            } else {
                break;
            }
        }
        return result;
    }

    int parseFactor() {
        skipWhitespace();
        if (match('-')) {
            return -parseFactor();
        } else if (match('~')) {
            return ~parseFactor();
        } else if (isdigit(peek())) {
            return parseNumber();
        } else if (match('t')) {
            return t;
        } else if (match('(')) {
            int result = parseExpression();
            if (!match(')')) {
                throw std::runtime_error("Expected closing parenthesis");
            }
            return result;
        } else {
            throw std::runtime_error("Unexpected character at position " + std::to_string(pos));
        }
    }

    int parseNumber() {
        uint32_t result = 0;
        while (isdigit(peek())) {
            result = result * 10 + (consume() - '0');
        }
        return result;
    }

    char peek() const {
        return pos < expr.size() ? expr[pos] : '\0';
    }

    char consume() {
        return expr[pos++];
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

int main() {
    std::string expression;
    int t;

    std::cout << "Enter bytebeat expression: ";
    std::getline(std::cin, expression);

    std::cout << "Enter time variable t: ";
    std::cin >> t;

    try {
        BytebeatParser parser(expression);
        int result = parser.parseAndEvaluate(t);
        std::cout << "Result: " << result << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}