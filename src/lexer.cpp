#include "lexer.h"

#include <cctype>
#include <sstream>

namespace smush {

const std::unordered_map<std::string, TokenType> Lexer::keywords = {
    {"as", TokenType::AS},
    {"async", TokenType::ASYNC},
    {"await", TokenType::AWAIT},
    {"cast", TokenType::CAST},
    {"pub", TokenType::PUBLIC},
    {"public", TokenType::PUBLIC},
    {"private", TokenType::PRIVATE},
    {"abstract", TokenType::ABSTRACT},
    {"static", TokenType::STATIC},
    {"constructor", TokenType::CONSTRUCTOR},
    {"fn", TokenType::FN},
    {"if", TokenType::IF},
    {"else", TokenType::ELSE},
    {"for", TokenType::FOR},
    {"while", TokenType::WHILE},
    {"match", TokenType::MATCH},
    {"try", TokenType::TRY},
    {"catch", TokenType::CATCH},
    {"throw", TokenType::THROW},
    {"return", TokenType::RETURN},
    {"break", TokenType::BREAK},
    {"continue", TokenType::CONTINUE},
    {"class", TokenType::CLASS},
    {"extends", TokenType::EXTENDS},
    {"data", TokenType::DATA},
    {"interface", TokenType::INTERFACE},
    {"import", TokenType::IMPORT},
    {"implements", TokenType::IMPLEMENTS},
    {"is", TokenType::IS},
    {"type", TokenType::TYPE},
    {"let", TokenType::LET},
    {"var", TokenType::VAR},
    {"const", TokenType::CONST},
    {"in", TokenType::IN},
    {"new", TokenType::NEW},
    {"true", TokenType::TRUE},
    {"false", TokenType::FALSE},
    {"null", TokenType::NULL_KW},
    {"and", TokenType::AND},
    {"or", TokenType::OR},
    {"not", TokenType::NOT},
    {"self", TokenType::SELF},
    {"super", TokenType::SUPER},
};

Lexer::Lexer(std::string sourceText, std::string sourceName)
    : source(std::move(sourceText)), filename(std::move(sourceName)) {}

std::vector<Token> Lexer::tokenize() {
    while (!isAtEnd()) {
        skipWhitespace();
        if (!isAtEnd()) {
            lexToken();
        }
    }
    addToken(TokenType::END_OF_FILE, "", location());
    return tokens;
}

const std::vector<std::string>& Lexer::getErrors() const {
    return errors;
}

char Lexer::peek(size_t offset) const {
    if (current + offset >= source.size()) {
        return '\0';
    }
    return source[current + offset];
}

char Lexer::advance() {
    const char c = peek();
    if (c == '\n') {
        ++line;
        column = 1;
    } else {
        ++column;
    }
    ++current;
    return c;
}

bool Lexer::match(char expected) {
    if (peek() != expected) {
        return false;
    }
    advance();
    return true;
}

bool Lexer::isAtEnd() const {
    return current >= source.size();
}

SourceLocation Lexer::location() const {
    return SourceLocation{line, column, filename};
}

void Lexer::addToken(TokenType type, const std::string& lexeme, const SourceLocation& loc) {
    tokens.push_back(Token{type, lexeme, loc});
}

void Lexer::lexToken() {
    const SourceLocation loc = location();
    const char c = advance();

    switch (c) {
        case '(':
            addToken(TokenType::LPAREN, "(", loc);
            return;
        case ')':
            addToken(TokenType::RPAREN, ")", loc);
            return;
        case '{':
            addToken(TokenType::LBRACE, "{", loc);
            return;
        case '}':
            addToken(TokenType::RBRACE, "}", loc);
            return;
        case '[':
            addToken(TokenType::LBRACKET, "[", loc);
            return;
        case ']':
            addToken(TokenType::RBRACKET, "]", loc);
            return;
        case ',':
            addToken(TokenType::COMMA, ",", loc);
            return;
        case ':':
            addToken(TokenType::COLON, ":", loc);
            return;
        case ';':
            addToken(TokenType::SEMICOLON, ";", loc);
            return;
        case '+':
            addToken(TokenType::PLUS, "+", loc);
            return;
        case '-':
            if (match('>')) {
                addToken(TokenType::ARROW, "->", loc);
            } else {
                addToken(TokenType::MINUS, "-", loc);
            }
            return;
        case '*':
            if (match('*')) {
                addToken(TokenType::STAR_STAR, "**", loc);
            } else {
                addToken(TokenType::STAR, "*", loc);
            }
            return;
        case '/':
            if (match('/')) {
                skipLineComment();
            } else {
                addToken(TokenType::SLASH, "/", loc);
            }
            return;
        case '%':
            addToken(TokenType::PERCENT, "%", loc);
            return;
        case '|':
            addToken(TokenType::PIPE, "|", loc);
            return;
        case '&':
            addToken(TokenType::AMP, "&", loc);
            return;
        case '!':
            if (match('=')) {
                addToken(TokenType::BANG_EQ, "!=", loc);
            } else {
                addToken(TokenType::BANG, "!", loc);
            }
            return;
        case '=':
            if (match('>')) {
                addToken(TokenType::FAT_ARROW, "=>", loc);
            } else if (match('=')) {
                addToken(TokenType::EQ_EQ, "==", loc);
            } else {
                addToken(TokenType::EQ, "=", loc);
            }
            return;
        case '<':
            if (match('=')) {
                addToken(TokenType::LTE, "<=", loc);
            } else {
                addToken(TokenType::LT, "<", loc);
            }
            return;
        case '>':
            if (match('=')) {
                addToken(TokenType::GTE, ">=", loc);
            } else {
                addToken(TokenType::GT, ">", loc);
            }
            return;
        case '?':
            if (match('?')) {
                addToken(TokenType::QMARK_QMARK, "??", loc);
            } else if (match('.')) {
                addToken(TokenType::QUESTION_DOT, "?.", loc);
            } else {
                addToken(TokenType::QMARK, "?", loc);
            }
            return;
        case '.':
            if (match('.')) {
                if (match('.')) {
                    addToken(TokenType::DOT_DOT_DOT, "...", loc);
                } else {
                    addToken(TokenType::DOT_DOT, "..", loc);
                }
            } else {
                addToken(TokenType::DOT, ".", loc);
            }
            return;
        case '"':
        case '\'':
            lexString(c, loc);
            return;
        default:
            break;
    }

    if (std::isdigit(static_cast<unsigned char>(c))) {
        lexNumber(c, loc);
        return;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        lexIdentifier(c, loc);
        return;
    }

    error(loc, std::string("unexpected character '") + c + "'");
}

void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        const char c = peek();
        if (c == ' ' || c == '\r' || c == '\t' || c == '\n') {
            advance();
            continue;
        }
        if (c == '/' && peek(1) == '/') {
            advance();
            advance();
            skipLineComment();
            continue;
        }
        break;
    }
}

void Lexer::skipLineComment() {
    while (!isAtEnd() && peek() != '\n') {
        advance();
    }
}

void Lexer::lexNumber(char first, const SourceLocation& loc) {
    std::string lexeme(1, first);
    bool isFloat = false;

    while (std::isdigit(static_cast<unsigned char>(peek()))) {
        lexeme.push_back(advance());
    }

    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        isFloat = true;
        lexeme.push_back(advance());
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            lexeme.push_back(advance());
        }
    }

    addToken(isFloat ? TokenType::FLOAT : TokenType::INTEGER, lexeme, loc);
}

void Lexer::lexIdentifier(char first, const SourceLocation& loc) {
    std::string lexeme(1, first);
    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
        lexeme.push_back(advance());
    }

    const auto it = keywords.find(lexeme);
    addToken(it == keywords.end() ? TokenType::IDENTIFIER : it->second, lexeme, loc);
}

void Lexer::lexString(char quote, const SourceLocation& loc) {
    std::string value;
    bool escaped = false;

    while (!isAtEnd()) {
        const char c = advance();
        if (escaped) {
            switch (c) {
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case '\\':
                    value.push_back('\\');
                    break;
                case '"':
                    value.push_back('"');
                    break;
                case '\'':
                    value.push_back('\'');
                    break;
                default:
                    value.push_back(c);
                    break;
            }
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == quote) {
            addToken(quote == '"' ? TokenType::STRING : TokenType::CHAR, value, loc);
            return;
        }
        value.push_back(c);
    }

    error(loc, "unterminated string literal");
}

void Lexer::error(const SourceLocation& loc, const std::string& message) {
    std::ostringstream oss;
    oss << loc.filename << ":" << loc.line << ":" << loc.column << ": " << message;
    errors.push_back(oss.str());
}

}  // namespace smush
