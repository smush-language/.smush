#include "parser.h"

#include <sstream>
#include <stdexcept>

namespace smush {

namespace {

StmtPtr makeExpressionBody(ExprPtr expr, const SourceLocation& loc) {
    auto block = std::make_unique<BlockStmt>(loc);
    block->statements.push_back(std::make_unique<ReturnStmt>(std::move(expr), loc));
    return block;
}

}  // namespace

Parser::Parser(std::vector<Token> tokenStream) : tokens(std::move(tokenStream)) {}

std::unique_ptr<Program> Parser::parse() {
    SourceLocation loc = tokens.empty() ? SourceLocation{} : tokens.front().loc;
    auto program = std::make_unique<Program>(loc);

    while (!isAtEnd()) {
        const size_t start = current;
        auto stmt = declaration();
        if (stmt) {
            program->statements.push_back(std::move(stmt));
            continue;
        }
        if (current == start && !isAtEnd()) {
            advance();
        }
    }

    return program;
}

const std::vector<std::string>& Parser::getErrors() const {
    return errors;
}

bool Parser::isAtEnd() const {
    return peek().type == TokenType::END_OF_FILE;
}

const Token& Parser::peek() const {
    return tokens[current];
}

const Token& Parser::previous() const {
    return tokens[current - 1];
}

const Token& Parser::advance() {
    if (!isAtEnd()) {
        ++current;
    }
    return previous();
}

bool Parser::check(TokenType type) const {
    return !isAtEnd() && peek().type == type;
}

bool Parser::match(TokenType type) {
    if (!check(type)) {
        return false;
    }
    advance();
    return true;
}

bool Parser::match(std::initializer_list<TokenType> types) {
    for (TokenType type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) {
        return advance();
    }
    errorAt(peek(), message);
    throw std::runtime_error("parse error");
}

void Parser::synchronize() {
    while (!isAtEnd()) {
        if (previous().type == TokenType::SEMICOLON) {
            return;
        }
        switch (peek().type) {
            case TokenType::FN:
            case TokenType::CLASS:
            case TokenType::DATA:
            case TokenType::INTERFACE:
            case TokenType::IMPORT:
            case TokenType::TYPE:
            case TokenType::ASYNC:
            case TokenType::ABSTRACT:
            case TokenType::LET:
            case TokenType::VAR:
            case TokenType::CONST:
            case TokenType::IF:
            case TokenType::MATCH:
            case TokenType::TRY:
            case TokenType::THROW:
            case TokenType::WHILE:
            case TokenType::FOR:
            case TokenType::RETURN:
                return;
            default:
                advance();
                break;
        }
    }
}

void Parser::errorAt(const Token& token, const std::string& message) {
    std::ostringstream oss;
    oss << token.loc.filename << ":" << token.loc.line << ":" << token.loc.column << ": " << message;
    errors.push_back(oss.str());
}

StmtPtr Parser::declaration() {
    try {
        if (match(TokenType::IMPORT)) {
            return importDeclaration();
        }
        if (match(TokenType::ASYNC)) {
            return asyncFunctionDeclaration();
        }
        if (match(TokenType::ABSTRACT)) {
            consume(TokenType::CLASS, "expected 'class' after 'abstract'");
            return classDeclaration(true);
        }
        if (match(TokenType::FN)) {
            return functionDeclaration();
        }
        if (match(TokenType::CLASS)) {
            return classDeclaration(false);
        }
        if (match(TokenType::DATA)) {
            return dataDeclaration();
        }
        if (match(TokenType::INTERFACE)) {
            return interfaceDeclaration();
        }
        if (match(TokenType::TYPE)) {
            return typeAliasDeclaration();
        }
        if (match(TokenType::LET)) {
            return variableDeclaration(false);
        }
        if (match(TokenType::VAR)) {
            return variableDeclaration(true);
        }
        if (match(TokenType::CONST)) {
            return variableDeclaration(false);
        }
        return statement();
    } catch (const std::runtime_error&) {
        synchronize();
        return nullptr;
    }
}

StmtPtr Parser::asyncFunctionDeclaration() {
    consume(TokenType::FN, "expected 'fn' after 'async'");
    return functionDeclaration(true);
}

StmtPtr Parser::functionDeclaration(bool isAsync) {
    const Token name = consume(TokenType::IDENTIFIER, "expected function name");
    auto generics = parseGenericParameters();
    consume(TokenType::LPAREN, "expected '(' after function name");
    auto params = parseParameters();
    consume(TokenType::RPAREN, "expected ')' after parameters");

    TypePtr returnType;
    if (match(TokenType::ARROW) || match(TokenType::COLON)) {
        returnType = parseType();
    }

    consume(TokenType::FAT_ARROW, "expected '=>' before function body");
    return std::make_unique<FunctionDecl>(
        name.lexeme,
        std::move(generics),
        std::move(params),
        std::move(returnType),
        arrowBodyStatement(),
        isAsync,
        name.loc);
}

StmtPtr Parser::classDeclaration(bool isAbstractClass) {
    const Token name = consume(TokenType::IDENTIFIER, "expected class name");
    auto decl = std::make_unique<ClassDecl>(name.lexeme, name.loc);
    decl->isAbstract = isAbstractClass;
    decl->genericParameters = parseGenericParameters();

    if (match(TokenType::LPAREN)) {
        if (!check(TokenType::RPAREN)) {
            do {
                decl->fields.push_back(parseFieldDeclaration());
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "expected ')' after class fields");
    }

    if (match(TokenType::EXTENDS)) {
        decl->baseClass = consume(TokenType::IDENTIFIER, "expected base class name after 'extends'").lexeme;
    }

    if (match(TokenType::IMPLEMENTS)) {
        do {
            const Token iface = consume(TokenType::IDENTIFIER, "expected interface name after 'implements'");
            decl->implementedInterfaces.push_back(iface.lexeme);
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::LBRACE, "expected '{' before class body");
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        Visibility visibility = Visibility::Public;
        if (match(TokenType::PUBLIC)) {
            visibility = Visibility::Public;
        } else if (match(TokenType::PRIVATE)) {
            visibility = Visibility::Private;
        }
        const bool isAbstract = match(TokenType::ABSTRACT);
        const bool isStatic = match(TokenType::STATIC);
        const bool isAsync = match(TokenType::ASYNC);
        const bool isConstructor = match(TokenType::CONSTRUCTOR);
        if (!isConstructor) {
            consume(TokenType::FN, "expected 'fn' in class member declaration");
        }
        const Token methodName = isConstructor
            ? Token{TokenType::CONSTRUCTOR, "constructor", peek().loc}
            : consume(TokenType::IDENTIFIER, "expected method name");
        auto genericParameters = isConstructor ? std::vector<GenericParamDecl>{} : parseGenericParameters();
        consume(TokenType::LPAREN, isConstructor ? "expected '(' after constructor" : "expected '(' after method name");
        auto params = parseParameters();
        consume(TokenType::RPAREN, "expected ')' after method parameters");

        TypePtr returnType;
        if (!isConstructor && (match(TokenType::ARROW) || match(TokenType::COLON))) {
            returnType = parseType();
        }
        StmtPtr body;
        if (isAbstract) {
            match(TokenType::SEMICOLON);
        } else {
            consume(TokenType::FAT_ARROW, isConstructor ? "expected '=>' before constructor body" : "expected '=>' before method body");
            body = arrowBodyStatement();
        }
        decl->methods.push_back(MethodDecl{
            methodName.lexeme,
            std::move(genericParameters),
            std::move(params),
            std::move(returnType),
            std::move(body),
            isAsync,
            isStatic,
            isConstructor,
            isAbstract,
            visibility,
        });
    }
    consume(TokenType::RBRACE, "expected '}' after class body");
    return decl;
}

StmtPtr Parser::dataDeclaration() {
    const Token name = consume(TokenType::IDENTIFIER, "expected data type name");
    auto decl = std::make_unique<DataDecl>(name.lexeme, name.loc);
    decl->genericParameters = parseGenericParameters();

    consume(TokenType::LPAREN, "expected '(' after data type name");
    if (!check(TokenType::RPAREN)) {
        do {
            decl->fields.push_back(parseFieldDeclaration());
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "expected ')' after data fields");
    match(TokenType::SEMICOLON);
    return decl;
}

StmtPtr Parser::interfaceDeclaration() {
    const Token name = consume(TokenType::IDENTIFIER, "expected interface name");
    auto decl = std::make_unique<InterfaceDecl>(name.lexeme, name.loc);
    decl->genericParameters = parseGenericParameters();
    if (match(TokenType::EXTENDS)) {
        do {
            const Token iface = consume(TokenType::IDENTIFIER, "expected interface name after 'extends'");
            decl->baseInterfaces.push_back(iface.lexeme);
        } while (match(TokenType::COMMA));
    }

    if (match(TokenType::LBRACE)) {
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            Visibility visibility = Visibility::Public;
            if (match(TokenType::PUBLIC)) {
                visibility = Visibility::Public;
            } else if (match(TokenType::PRIVATE)) {
                visibility = Visibility::Private;
            }
            match(TokenType::ABSTRACT);
            const bool isStatic = match(TokenType::STATIC);
            const bool isAsync = match(TokenType::ASYNC);
            consume(TokenType::FN, "expected 'fn' in interface declaration");
            const Token methodName = consume(TokenType::IDENTIFIER, "expected interface method name");
            auto genericParameters = parseGenericParameters();
            consume(TokenType::LPAREN, "expected '(' after interface method name");
            auto params = parseParameters();
            consume(TokenType::RPAREN, "expected ')' after interface method parameters");

            TypePtr returnType = std::make_shared<Type>("Void");
            if (match(TokenType::ARROW) || match(TokenType::COLON) || match(TokenType::FAT_ARROW)) {
                returnType = parseType();
            }
            match(TokenType::SEMICOLON);
            decl->methods.push_back(InterfaceMethodSig{
                methodName.lexeme,
                std::move(genericParameters),
                std::move(params),
                std::move(returnType),
                isAsync,
                isStatic,
                true,
                visibility,
            });
        }
        consume(TokenType::RBRACE, "expected '}' after interface body");
    }
    match(TokenType::SEMICOLON);
    return decl;
}

StmtPtr Parser::typeAliasDeclaration() {
    const Token name = consume(TokenType::IDENTIFIER, "expected type alias name");
    auto generics = parseGenericParameters();
    consume(TokenType::EQ, "expected '=' after type alias name");
    auto aliasedType = parseType();
    match(TokenType::SEMICOLON);
    return std::make_unique<TypeAliasDecl>(name.lexeme, std::move(generics), std::move(aliasedType), name.loc);
}

StmtPtr Parser::importDeclaration() {
    const Token path = consume(TokenType::STRING, "expected import path string");
    match(TokenType::SEMICOLON);
    return std::make_unique<ImportDecl>(path.lexeme, path.loc);
}

StmtPtr Parser::variableDeclaration(bool mutableBinding) {
    const Token name = consume(TokenType::IDENTIFIER, "expected variable name");
    TypePtr type;
    if (match(TokenType::COLON)) {
        type = parseType();
    }

    ExprPtr initializer;
    if (match(TokenType::EQ)) {
        initializer = expression();
    }
    match(TokenType::SEMICOLON);
    return std::make_unique<VarDeclStmt>(name.lexeme, std::move(type), std::move(initializer), mutableBinding, name.loc);
}

StmtPtr Parser::statement() {
    if (match(TokenType::LBRACE)) {
        return blockStatement();
    }
    if (match(TokenType::IF)) {
        return ifStatement();
    }
    if (match(TokenType::MATCH)) {
        return matchStatement();
    }
    if (match(TokenType::TRY)) {
        return tryStatement();
    }
    if (match(TokenType::THROW)) {
        return throwStatement();
    }
    if (match(TokenType::WHILE)) {
        return whileStatement();
    }
    if (match(TokenType::FOR)) {
        return forStatement();
    }
    if (match(TokenType::RETURN)) {
        return returnStatement();
    }
    if (match(TokenType::BREAK)) {
        return breakStatement();
    }
    if (match(TokenType::CONTINUE)) {
        return continueStatement();
    }
    return expressionStatement();
}

StmtPtr Parser::blockStatement() {
    auto block = std::make_unique<BlockStmt>(previous().loc);
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        auto stmt = declaration();
        if (stmt) {
            block->statements.push_back(std::move(stmt));
        }
    }
    consume(TokenType::RBRACE, "expected '}' after block");
    return block;
}

StmtPtr Parser::ifStatement() {
    auto condition = expression();
    StmtPtr thenBranch = match(TokenType::FAT_ARROW) ? arrowBodyStatement() : statement();
    StmtPtr elseBranch;
    if (match(TokenType::ELSE)) {
        elseBranch = match(TokenType::FAT_ARROW) ? arrowBodyStatement() : statement();
    }
    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch), previous().loc);
}

StmtPtr Parser::whileStatement() {
    auto condition = expression();
    StmtPtr body = match(TokenType::FAT_ARROW) ? arrowBodyStatement() : statement();
    return std::make_unique<WhileStmt>(std::move(condition), std::move(body), previous().loc);
}

StmtPtr Parser::matchStatement() {
    const SourceLocation loc = previous().loc;
    auto value = expression();
    consume(TokenType::LBRACE, "expected '{' after match value");
    std::vector<MatchCase> cases;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        MatchCase matchCase;
        matchCase.loc = peek().loc;
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
            advance();
            matchCase.kind = MatchPatternKind::Wildcard;
        } else if (check(TokenType::IDENTIFIER)) {
            matchCase.kind = MatchPatternKind::Type;
            matchCase.typeName = advance().lexeme;
        } else {
            matchCase.kind = MatchPatternKind::Literal;
            matchCase.pattern = primary();
        }
        consume(TokenType::FAT_ARROW, "expected '=>' after match pattern");
        matchCase.body = arrowBodyStatement();
        cases.push_back(std::move(matchCase));
    }
    consume(TokenType::RBRACE, "expected '}' after match cases");
    return std::make_unique<MatchStmt>(std::move(value), std::move(cases), loc);
}

StmtPtr Parser::tryStatement() {
    const SourceLocation loc = previous().loc;
    StmtPtr tryBranch = match(TokenType::FAT_ARROW) ? arrowBodyStatement() : statement();
    consume(TokenType::CATCH, "expected 'catch' after try block");
    const Token catchName = consume(TokenType::IDENTIFIER, "expected catch binding name");
    StmtPtr catchBranch = match(TokenType::FAT_ARROW) ? arrowBodyStatement() : statement();
    return std::make_unique<TryStmt>(std::move(tryBranch), catchName.lexeme, std::move(catchBranch), loc);
}

StmtPtr Parser::throwStatement() {
    const SourceLocation loc = previous().loc;
    auto value = expression();
    match(TokenType::SEMICOLON);
    return std::make_unique<ThrowStmt>(std::move(value), loc);
}

StmtPtr Parser::forStatement() {
    const Token name = consume(TokenType::IDENTIFIER, "expected loop variable");
    consume(TokenType::IN, "expected 'in' after loop variable");
    auto iterable = expression();
    StmtPtr body = match(TokenType::FAT_ARROW) ? arrowBodyStatement() : statement();
    return std::make_unique<ForStmt>(name.lexeme, std::move(iterable), std::move(body), name.loc);
}

StmtPtr Parser::returnStatement() {
    ExprPtr value;
    if (!check(TokenType::SEMICOLON) && !check(TokenType::RBRACE) && !isAtEnd()) {
        value = expression();
    }
    match(TokenType::SEMICOLON);
    return std::make_unique<ReturnStmt>(std::move(value), previous().loc);
}

StmtPtr Parser::breakStatement() {
    match(TokenType::SEMICOLON);
    return std::make_unique<BreakStmt>(previous().loc);
}

StmtPtr Parser::continueStatement() {
    match(TokenType::SEMICOLON);
    return std::make_unique<ContinueStmt>(previous().loc);
}

StmtPtr Parser::expressionStatement() {
    auto expr = expression();
    SourceLocation loc = expr->loc;
    match(TokenType::SEMICOLON);
    return std::make_unique<ExpressionStmt>(std::move(expr), loc);
}

StmtPtr Parser::arrowBodyStatement() {
    if (match(TokenType::LBRACE)) {
        return blockStatement();
    }
    auto expr = expression();
    SourceLocation loc = expr->loc;
    match(TokenType::SEMICOLON);
    return makeExpressionBody(std::move(expr), loc);
}

ExprPtr Parser::expression() {
    return assignment();
}

ExprPtr Parser::assignment() {
    auto expr = nullCoalesce();
    if (match(TokenType::EQ)) {
        auto value = assignment();
        return std::make_unique<AssignExpr>(std::move(expr), std::move(value), previous().loc);
    }
    return expr;
}

ExprPtr Parser::nullCoalesce() {
    auto expr = logicOr();
    while (match(TokenType::QMARK_QMARK)) {
        auto right = logicOr();
        expr = std::make_unique<NullCoalesceExpr>(std::move(expr), std::move(right), previous().loc);
    }
    return expr;
}

ExprPtr Parser::logicOr() {
    auto expr = logicAnd();
    while (match(TokenType::OR)) {
        Token op = previous();
        auto right = logicAnd();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::logicAnd() {
    auto expr = equality();
    while (match(TokenType::AND)) {
        Token op = previous();
        auto right = equality();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::equality() {
    auto expr = typeCheck();
    while (match({TokenType::EQ_EQ, TokenType::BANG_EQ})) {
        Token op = previous();
        auto right = typeCheck();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::typeCheck() {
    auto expr = comparison();
    while (match({TokenType::IS, TokenType::AS})) {
        Token op = previous();
        TypePtr targetType = parseType();
        if (op.type == TokenType::IS) {
            expr = std::make_unique<TypeCheckExpr>(std::move(expr), std::move(targetType), op.loc);
        } else {
            expr = std::make_unique<SafeCastExpr>(std::move(expr), std::move(targetType), op.loc);
        }
    }
    return expr;
}

ExprPtr Parser::comparison() {
    auto expr = range();
    while (match({TokenType::LT, TokenType::LTE, TokenType::GT, TokenType::GTE})) {
        Token op = previous();
        auto right = range();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::range() {
    auto expr = term();
    while (match({TokenType::DOT_DOT, TokenType::DOT_DOT_DOT})) {
        const bool inclusive = previous().type == TokenType::DOT_DOT_DOT;
        auto right = term();
        expr = std::make_unique<RangeExpr>(std::move(expr), std::move(right), inclusive, previous().loc);
    }
    return expr;
}

ExprPtr Parser::term() {
    auto expr = factor();
    while (match({TokenType::PLUS, TokenType::MINUS})) {
        Token op = previous();
        auto right = factor();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::factor() {
    auto expr = unary();
    while (match({TokenType::STAR, TokenType::SLASH, TokenType::PERCENT, TokenType::STAR_STAR})) {
        Token op = previous();
        auto right = unary();
        expr = std::make_unique<BinaryExpr>(std::move(expr), op, std::move(right));
    }
    return expr;
}

ExprPtr Parser::unary() {
    if (match(TokenType::AWAIT)) {
        Token keyword = previous();
        return std::make_unique<AwaitExpr>(unary(), keyword.loc);
    }
    if (match({TokenType::BANG, TokenType::NOT, TokenType::MINUS})) {
        return std::make_unique<UnaryExpr>(previous(), unary());
    }
    return call();
}

ExprPtr Parser::call() {
    auto expr = primary();

    while (true) {
        if (match(TokenType::LPAREN)) {
            SourceLocation loc = expr->loc;
            auto args = parseArguments();
            consume(TokenType::RPAREN, "expected ')' after arguments");
            expr = std::make_unique<CallExpr>(std::move(expr), std::move(args), loc);
        } else if (match(TokenType::DOT) || match(TokenType::QUESTION_DOT)) {
            const bool safe = previous().type == TokenType::QUESTION_DOT;
            Token member = consume(TokenType::IDENTIFIER, "expected member name");
            expr = std::make_unique<MemberExpr>(std::move(expr), member.lexeme, member.loc, safe);
        } else if (match(TokenType::LBRACKET)) {
            SourceLocation loc = expr->loc;
            auto index = expression();
            consume(TokenType::RBRACKET, "expected ']' after index");
            expr = std::make_unique<IndexExpr>(std::move(expr), std::move(index), loc);
        } else {
            break;
        }
    }

    return expr;
}

ExprPtr Parser::primary() {
    if (match(TokenType::CAST)) {
        const SourceLocation loc = previous().loc;
        consume(TokenType::LT, "expected '<' after 'cast'");
        TypePtr targetType = parseType();
        consume(TokenType::GT, "expected '>' after cast target type");
        consume(TokenType::LPAREN, "expected '(' after cast target type");
        auto expr = expression();
        consume(TokenType::RPAREN, "expected ')' after cast expression");
        return std::make_unique<CheckedCastExpr>(std::move(expr), std::move(targetType), loc);
    }
    if (match(TokenType::FN)) {
        const SourceLocation loc = previous().loc;
        consume(TokenType::LPAREN, "expected '(' after 'fn' in lambda expression");
        auto params = parseParameters();
        consume(TokenType::RPAREN, "expected ')' after lambda parameters");
        TypePtr returnType;
        if (match(TokenType::ARROW) || match(TokenType::COLON)) {
            returnType = parseType();
        }
        consume(TokenType::FAT_ARROW, "expected '=>' before lambda body");
        return std::make_unique<LambdaExpr>(std::move(params), std::move(returnType), arrowBodyStatement(), loc);
    }
    if (match(TokenType::FALSE)) {
        return std::make_unique<LiteralExpr>("false", TokenType::FALSE, previous().loc);
    }
    if (match(TokenType::TRUE)) {
        return std::make_unique<LiteralExpr>("true", TokenType::TRUE, previous().loc);
    }
    if (match(TokenType::NULL_KW)) {
        return std::make_unique<LiteralExpr>("null", TokenType::NULL_KW, previous().loc);
    }
    if (match({TokenType::INTEGER, TokenType::FLOAT, TokenType::STRING, TokenType::CHAR})) {
        return std::make_unique<LiteralExpr>(previous().lexeme, previous().type, previous().loc);
    }
    if (match(TokenType::SELF)) {
        return std::make_unique<IdentifierExpr>("self", previous().loc);
    }
    if (match(TokenType::SUPER)) {
        return std::make_unique<IdentifierExpr>("super", previous().loc);
    }
    if (match(TokenType::IDENTIFIER) || match(TokenType::IMPLEMENTS)) {
        return std::make_unique<IdentifierExpr>(previous().lexeme, previous().loc);
    }
    if (match(TokenType::NEW)) {
        Token name = consume(TokenType::IDENTIFIER, "expected type name after 'new'");
        consume(TokenType::LPAREN, "expected '(' after type name");
        auto args = parseArguments();
        consume(TokenType::RPAREN, "expected ')' after constructor arguments");
        return std::make_unique<NewExpr>(name.lexeme, std::move(args), name.loc);
    }
    if (match(TokenType::LPAREN)) {
        auto expr = expression();
        consume(TokenType::RPAREN, "expected ')' after expression");
        return expr;
    }
    if (match(TokenType::LBRACKET)) {
        std::vector<ExprPtr> elements;
        if (!check(TokenType::RBRACKET)) {
            do {
                elements.push_back(expression());
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACKET, "expected ']' after list literal");
        return std::make_unique<ListExpr>(std::move(elements), previous().loc);
    }

    errorAt(peek(), "expected expression");
    throw std::runtime_error("parse error");
}

TypePtr Parser::parseType() {
    return parseUnionType();
}

TypePtr Parser::parseUnionType() {
    std::vector<TypePtr> members;
    members.push_back(parseIntersectionType());
    while (match(TokenType::PIPE)) {
        members.push_back(parseIntersectionType());
    }
    if (members.size() == 1) {
        return std::move(members.front());
    }
    return std::make_shared<Type>(TypeKind::Union, std::move(members));
}

TypePtr Parser::parseIntersectionType() {
    std::vector<TypePtr> members;
    members.push_back(parseNullableType());
    while (match(TokenType::AMP)) {
        members.push_back(parseNullableType());
    }
    if (members.size() == 1) {
        return std::move(members.front());
    }
    return std::make_shared<Type>(TypeKind::Intersection, std::move(members));
}

TypePtr Parser::parseNullableType() {
    auto type = parsePrimaryType();
    if (match(TokenType::QMARK)) {
        type->nullable = true;
    }
    return type;
}

TypePtr Parser::parsePrimaryType() {
    if (match(TokenType::LPAREN)) {
        std::vector<TypePtr> parameterTypes;
        if (!check(TokenType::RPAREN)) {
            do {
                parameterTypes.push_back(parseType());
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "expected ')' after function type parameters");
        consume(TokenType::ARROW, "expected '->' in function type");
        TypePtr resultType = parseType();
        return std::make_shared<Type>(std::move(parameterTypes), std::move(resultType));
    }
    Token name = consume(TokenType::IDENTIFIER, "expected type name");
    std::vector<TypePtr> genericArguments;
    if (match(TokenType::LT)) {
        do {
            genericArguments.push_back(parseType());
        } while (match(TokenType::COMMA));
        consume(TokenType::GT, "expected '>' after generic type arguments");
    }
    return std::make_shared<Type>(name.lexeme, std::move(genericArguments));
}

FieldDecl Parser::parseFieldDeclaration(bool allowVisibility) {
    Visibility visibility = Visibility::Public;
    if (allowVisibility) {
        if (match(TokenType::PUBLIC)) {
            visibility = Visibility::Public;
        } else if (match(TokenType::PRIVATE)) {
            visibility = Visibility::Private;
        }
    }

    Token name = consume(TokenType::IDENTIFIER, "expected field name");
    TypePtr declaredType = std::make_shared<Type>("Any");
    if (match(TokenType::COLON)) {
        declaredType = parseType();
    }

    ExprPtr defaultValue;
    if (match(TokenType::EQ)) {
        defaultValue = expression();
    }

    FieldDecl field;
    field.name = name.lexeme;
    field.declaredType = std::move(declaredType);
    field.defaultValue = std::move(defaultValue);
    field.visibility = visibility;
    field.loc = name.loc;
    return field;
}

std::vector<GenericParamDecl> Parser::parseGenericParameters() {
    std::vector<GenericParamDecl> parameters;
    if (!match(TokenType::LT)) {
        return parameters;
    }
    do {
        Token name = consume(TokenType::IDENTIFIER, "expected generic parameter name");
        GenericParamDecl parameter;
        parameter.name = name.lexeme;
        parameter.loc = name.loc;
        if (match(TokenType::COLON)) {
            do {
                parameter.bounds.push_back(parseType());
            } while (match(TokenType::PLUS));
        }
        parameters.push_back(std::move(parameter));
    } while (match(TokenType::COMMA));
    consume(TokenType::GT, "expected '>' after generic parameters");
    return parameters;
}

std::pair<std::string, TypePtr> Parser::parseParameter() {
    Token name = consume(TokenType::IDENTIFIER, "expected parameter name");
    TypePtr type;
    if (match(TokenType::COLON)) {
        type = parseType();
    }
    return {name.lexeme, std::move(type)};
}

std::vector<std::pair<std::string, TypePtr>> Parser::parseParameters() {
    std::vector<std::pair<std::string, TypePtr>> params;
    if (check(TokenType::RPAREN)) {
        return params;
    }
    do {
        params.push_back(parseParameter());
    } while (match(TokenType::COMMA));
    return params;
}

std::vector<ExprPtr> Parser::parseArguments() {
    std::vector<ExprPtr> args;
    if (check(TokenType::RPAREN)) {
        return args;
    }
    do {
        args.push_back(expression());
    } while (match(TokenType::COMMA));
    return args;
}

}  // namespace smush
