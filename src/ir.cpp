#include "ir.h"

#include <cmath>
#include <sstream>

namespace smush {

namespace {

IRType identifierTypeFallback(const std::string& name) {
    if (name == "true" || name == "false") {
        return IRType(IRTypeKind::Bool, "Bool");
    }
    if (name == "null") {
        return IRType(IRTypeKind::Null, "Null");
    }
    return IRType(IRTypeKind::Any, "Any");
}

std::string locString(const SourceLocation& loc) {
    return loc.filename + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column);
}

std::vector<std::string> genericParamNames(const std::vector<GenericParamDecl>& parameters) {
    std::vector<std::string> names;
    names.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        names.push_back(parameter.name);
    }
    return names;
}

TypePtr cloneTypeExpr(const TypePtr& type) {
    if (!type) return nullptr;
    if (type->kind == TypeKind::Function) {
        std::vector<TypePtr> parameters;
        for (const auto& parameter : type->parameterTypes) {
            parameters.push_back(cloneTypeExpr(parameter));
        }
        return std::make_shared<Type>(std::move(parameters), cloneTypeExpr(type->returnType), type->nullable);
    }
    if (type->kind == TypeKind::Union || type->kind == TypeKind::Intersection) {
        std::vector<TypePtr> members;
        for (const auto& member : type->members) {
            members.push_back(cloneTypeExpr(member));
        }
        return std::make_shared<Type>(type->kind, std::move(members), type->nullable);
    }
    std::vector<TypePtr> arguments;
    for (const auto& argument : type->genericArguments) {
        arguments.push_back(cloneTypeExpr(argument));
    }
    return std::make_shared<Type>(type->name, std::move(arguments), type->nullable);
}

TypePtr substituteTypeExpr(const TypePtr& type, const std::unordered_map<std::string, TypePtr>& mapping) {
    if (!type) return nullptr;
    if (type->kind == TypeKind::Function) {
        std::vector<TypePtr> parameters;
        for (const auto& parameter : type->parameterTypes) {
            parameters.push_back(substituteTypeExpr(parameter, mapping));
        }
        return std::make_shared<Type>(std::move(parameters), substituteTypeExpr(type->returnType, mapping), type->nullable);
    }
    if (type->kind == TypeKind::Union || type->kind == TypeKind::Intersection) {
        std::vector<TypePtr> members;
        for (const auto& member : type->members) {
            members.push_back(substituteTypeExpr(member, mapping));
        }
        return std::make_shared<Type>(type->kind, std::move(members), type->nullable);
    }
    auto mapped = mapping.find(type->name);
    if (mapped != mapping.end() && type->genericArguments.empty()) {
        auto result = cloneTypeExpr(mapped->second);
        if (result) {
            result->nullable = result->nullable || type->nullable;
        }
        return result;
    }
    std::vector<TypePtr> arguments;
    for (const auto& argument : type->genericArguments) {
        arguments.push_back(substituteTypeExpr(argument, mapping));
    }
    return std::make_shared<Type>(type->name, std::move(arguments), type->nullable);
}

const IRExpression* extractInlineBody(const IRFunction& function) {
    if (!function.body) return nullptr;
    if (function.body->kind == IRStmtKind::Return) {
        return static_cast<const IRReturnStmt*>(function.body.get())->value.get();
    }
    if (function.body->kind != IRStmtKind::Block) return nullptr;
    const auto* block = static_cast<const IRBlockStmt*>(function.body.get());
    if (block->statements.size() != 1) return nullptr;
    const auto* only = block->statements.front().get();
    if (only->kind == IRStmtKind::Return) {
        return static_cast<const IRReturnStmt*>(only)->value.get();
    }
    if (only->kind == IRStmtKind::Expr) {
        return static_cast<const IRExprStmt*>(only)->expression.get();
    }
    return nullptr;
}

bool isTerminator(const IRStatement* statement) {
    return statement && (statement->kind == IRStmtKind::Return ||
                         statement->kind == IRStmtKind::Throw ||
                         statement->kind == IRStmtKind::Break ||
                         statement->kind == IRStmtKind::Continue);
}

bool isLiteralNull(const IRExpression* expression) {
    return expression &&
           expression->kind == IRExprKind::Literal &&
           expression->type.kind == IRTypeKind::Null;
}

bool isLiteralBool(const IRExpression* expression, bool& value) {
    if (!expression ||
        expression->kind != IRExprKind::Literal ||
        expression->type.kind != IRTypeKind::Bool) {
        return false;
    }
    value = static_cast<const IRLiteralExpr*>(expression)->value == "true";
    return true;
}

bool blockCanBeFlattened(const IRBlockStmt* block) {
    if (!block) return false;
    for (const auto& stmt : block->statements) {
        if (stmt && stmt->kind == IRStmtKind::VarDecl) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string IRType::toString() const {
    std::string rendered;
    if (!name.empty()) {
        rendered = name;
    } else {
        switch (kind) {
            case IRTypeKind::Void: rendered = "Void"; break;
            case IRTypeKind::Null: rendered = "Null"; break;
            case IRTypeKind::Bool: rendered = "Bool"; break;
            case IRTypeKind::Int: rendered = "Int"; break;
            case IRTypeKind::Float: rendered = "Float"; break;
            case IRTypeKind::String: rendered = "String"; break;
            case IRTypeKind::Future: rendered = "Future"; break;
            case IRTypeKind::List: rendered = "List"; break;
            case IRTypeKind::Tuple: rendered = "Tuple"; break;
            case IRTypeKind::Map: rendered = "Map"; break;
            case IRTypeKind::Set: rendered = "Set"; break;
            case IRTypeKind::Range: rendered = "Range"; break;
            case IRTypeKind::Object: rendered = "Object"; break;
            case IRTypeKind::Any: rendered = "Any"; break;
        }
    }
    if (nullable && rendered != "Null" && rendered != "Any") {
        rendered += "?";
    }
    return rendered.empty() ? "Any" : rendered;
}

IRIdentifierExpr::IRIdentifierExpr(std::string symbol, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Identifier, std::move(exprType), std::move(location)), name(std::move(symbol)) {}
IRLiteralExpr::IRLiteralExpr(std::string literal, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Literal, std::move(exprType), std::move(location)), value(std::move(literal)) {}
IRLambdaExpr::IRLambdaExpr(std::string loweredFunctionName, std::vector<std::string> capturedNames, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Lambda, std::move(exprType), std::move(location)),
      functionName(std::move(loweredFunctionName)),
      captures(std::move(capturedNames)) {}
IRUnaryExpr::IRUnaryExpr(std::string operation, IRExprPtr value, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Unary, std::move(exprType), std::move(location)), op(std::move(operation)), operand(std::move(value)) {}
IRAwaitExpr::IRAwaitExpr(IRExprPtr value, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Await, std::move(exprType), std::move(location)), operand(std::move(value)) {}
IRTypeCheckExpr::IRTypeCheckExpr(IRExprPtr value, IRType checkedType, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::TypeCheck, std::move(exprType), std::move(location)),
      operand(std::move(value)),
      targetType(std::move(checkedType)) {}
IRSafeCastExpr::IRSafeCastExpr(IRExprPtr value, IRType castType, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::SafeCast, std::move(exprType), std::move(location)),
      operand(std::move(value)),
      targetType(std::move(castType)) {}
IRCheckedCastExpr::IRCheckedCastExpr(IRExprPtr value, IRType castType, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::CheckedCast, std::move(exprType), std::move(location)),
      operand(std::move(value)),
      targetType(std::move(castType)) {}
IRBinaryExpr::IRBinaryExpr(std::string operation, IRExprPtr lhs, IRExprPtr rhs, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Binary, std::move(exprType), std::move(location)), op(std::move(operation)), left(std::move(lhs)), right(std::move(rhs)) {}
IRCallExpr::IRCallExpr(IRExprPtr target, std::vector<IRExprPtr> args, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Call, std::move(exprType), std::move(location)), callee(std::move(target)), arguments(std::move(args)) {}
IRMemberExpr::IRMemberExpr(IRExprPtr target, std::string memberName, bool safeAccess, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Member, std::move(exprType), std::move(location)), object(std::move(target)), member(std::move(memberName)), safe(safeAccess) {}
IRIndexExpr::IRIndexExpr(IRExprPtr target, IRExprPtr offset, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Index, std::move(exprType), std::move(location)), object(std::move(target)), index(std::move(offset)) {}
IRListExpr::IRListExpr(std::vector<IRExprPtr> values, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::List, std::move(exprType), std::move(location)), elements(std::move(values)) {}
IRAssignExpr::IRAssignExpr(IRExprPtr lhs, IRExprPtr rhs, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Assign, std::move(exprType), std::move(location)), target(std::move(lhs)), value(std::move(rhs)) {}
IRNullCoalesceExpr::IRNullCoalesceExpr(IRExprPtr lhs, IRExprPtr rhs, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::NullCoalesce, std::move(exprType), std::move(location)), left(std::move(lhs)), right(std::move(rhs)) {}
IRRangeExpr::IRRangeExpr(IRExprPtr from, IRExprPtr to, bool includeEnd, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Range, std::move(exprType), std::move(location)), start(std::move(from)), end(std::move(to)), inclusive(includeEnd) {}
IRConstructExpr::IRConstructExpr(std::string constructedType, std::vector<IRExprPtr> args, IRType exprType, SourceLocation location)
    : IRExpression(IRExprKind::Construct, std::move(exprType), std::move(location)), typeName(std::move(constructedType)), arguments(std::move(args)) {}
IRBlockStmt::IRBlockStmt(SourceLocation location) : IRStatement(IRStmtKind::Block, std::move(location)) {}
IRVarDeclStmt::IRVarDeclStmt(std::string symbol, IRType type, IRExprPtr init, bool mutableFlag, SourceLocation location)
    : IRStatement(IRStmtKind::VarDecl, std::move(location)), name(std::move(symbol)), declaredType(std::move(type)), initializer(std::move(init)), mutableBinding(mutableFlag) {}
IRExprStmt::IRExprStmt(IRExprPtr expr, SourceLocation location)
    : IRStatement(IRStmtKind::Expr, std::move(location)), expression(std::move(expr)) {}
IRIfStmt::IRIfStmt(IRExprPtr cond, IRStmtPtr thenStmt, IRStmtPtr elseStmt, SourceLocation location)
    : IRStatement(IRStmtKind::If, std::move(location)), condition(std::move(cond)), thenBranch(std::move(thenStmt)), elseBranch(std::move(elseStmt)) {}
IRWhileStmt::IRWhileStmt(IRExprPtr cond, IRStmtPtr loopBody, SourceLocation location)
    : IRStatement(IRStmtKind::While, std::move(location)), condition(std::move(cond)), body(std::move(loopBody)) {}
IRForStmt::IRForStmt(std::string variable, IRExprPtr source, IRStmtPtr loopBody, SourceLocation location)
    : IRStatement(IRStmtKind::For, std::move(location)), name(std::move(variable)), iterable(std::move(source)), body(std::move(loopBody)) {}
IRTryStmt::IRTryStmt(IRStmtPtr tryStmt, std::string name, IRStmtPtr catchStmt, SourceLocation location)
    : IRStatement(IRStmtKind::Try, std::move(location)),
      tryBranch(std::move(tryStmt)),
      catchName(std::move(name)),
      catchBranch(std::move(catchStmt)) {}
IRThrowStmt::IRThrowStmt(IRExprPtr expr, SourceLocation location)
    : IRStatement(IRStmtKind::Throw, std::move(location)), value(std::move(expr)) {}
IRReturnStmt::IRReturnStmt(IRExprPtr expr, SourceLocation location)
    : IRStatement(IRStmtKind::Return, std::move(location)), value(std::move(expr)) {}
IRBreakStmt::IRBreakStmt(SourceLocation location) : IRStatement(IRStmtKind::Break, std::move(location)) {}
IRContinueStmt::IRContinueStmt(SourceLocation location) : IRStatement(IRStmtKind::Continue, std::move(location)) {}

std::unique_ptr<IRProgram> IRLowerer::lower(Program* program) {
    errors.clear();
    scopes.clear();
    genericTypeScopes.clear();
    knownConstructors.clear();
    typeAliases.clear();
    currentClassName.clear();
    nextLambdaId = 0;
    nextMatchId = 0;
    lambdaContexts.clear();
    loweredLambdas.clear();

    auto ir = std::make_unique<IRProgram>();
    ir->loc = program ? program->loc : SourceLocation{};
    if (!program) {
        errors.push_back("internal error: null AST program");
        return ir;
    }

    for (const auto& stmt : program->statements) {
        if (stmt->kind == StatementKind::ClassDecl) {
            const auto* klass = static_cast<const ClassDecl*>(stmt.get());
            knownConstructors[klass->name] = IRType(IRTypeKind::Object, klass->name);
        } else if (stmt->kind == StatementKind::DataDecl) {
            const auto* data = static_cast<const DataDecl*>(stmt.get());
            knownConstructors[data->name] = IRType(IRTypeKind::Object, data->name);
        } else if (stmt->kind == StatementKind::TypeAliasDecl) {
            const auto* alias = static_cast<const TypeAliasDecl*>(stmt.get());
            typeAliases[alias->name] = {alias->genericParameters, cloneTypeExpr(alias->aliasedType)};
        }
    }

    pushScope();
    for (const auto& stmt : program->statements) {
        switch (stmt->kind) {
            case StatementKind::Function: {
                const auto* fn = static_cast<const FunctionDecl*>(stmt.get());
                bind(fn->name, IRType(IRTypeKind::Any, "Function"));
                ir->functions.push_back(lowerFunction(fn));
                break;
            }
            case StatementKind::InterfaceDecl:
                ir->interfaces.push_back(lowerInterface(static_cast<const InterfaceDecl*>(stmt.get())));
                break;
            case StatementKind::ClassDecl:
                ir->classes.push_back(lowerClass(static_cast<const ClassDecl*>(stmt.get())));
                break;
            case StatementKind::DataDecl:
                ir->dataTypes.push_back(lowerData(static_cast<const DataDecl*>(stmt.get())));
                break;
            case StatementKind::TypeAliasDecl:
            case StatementKind::ImportDecl:
                break;
            default: {
                auto lowered = lowerStatement(stmt.get());
                if (lowered) {
                    ir->globals.push_back(std::move(lowered));
                }
                break;
            }
        }
    }
    for (auto& lambda : loweredLambdas) {
        ir->functions.push_back(std::move(lambda));
    }
    popScope();

    return ir;
}

const std::vector<std::string>& IRLowerer::getErrors() const {
    return errors;
}

TypePtr IRLowerer::expandAliases(const TypePtr& type) const {
    if (!type) return nullptr;
    if (type->kind == TypeKind::Function) {
        std::vector<TypePtr> parameters;
        for (const auto& parameter : type->parameterTypes) {
            auto expandedParameter = expandAliases(parameter);
            if (!expandedParameter) {
                return nullptr;
            }
            parameters.push_back(expandedParameter);
        }
        auto expandedReturn = expandAliases(type->returnType);
        if (!expandedReturn) {
            return nullptr;
        }
        return std::make_shared<Type>(std::move(parameters), expandedReturn, type->nullable);
    }
    if (type->kind == TypeKind::Union || type->kind == TypeKind::Intersection) {
        std::vector<TypePtr> members;
        for (const auto& member : type->members) {
            auto expandedMember = expandAliases(member);
            if (!expandedMember) {
                return nullptr;
            }
            members.push_back(expandedMember);
        }
        return std::make_shared<Type>(type->kind, std::move(members), type->nullable);
    }

    auto aliasIt = typeAliases.find(type->name);
    if (aliasIt == typeAliases.end()) {
        std::vector<TypePtr> arguments;
        for (const auto& argument : type->genericArguments) {
            auto expandedArgument = expandAliases(argument);
            if (!expandedArgument) {
                return nullptr;
            }
            arguments.push_back(expandedArgument);
        }
        return std::make_shared<Type>(type->name, std::move(arguments), type->nullable);
    }

    std::unordered_map<std::string, TypePtr> mapping;
    for (size_t i = 0; i < aliasIt->second.first.size() && i < type->genericArguments.size(); ++i) {
        mapping.emplace(aliasIt->second.first[i].name, cloneTypeExpr(type->genericArguments[i]));
    }
    auto expanded = substituteTypeExpr(aliasIt->second.second, mapping);
    expanded = expandAliases(expanded);
    if (expanded) {
        expanded->nullable = expanded->nullable || type->nullable;
    }
    return expanded ? expanded : cloneTypeExpr(type);
}

IRType IRLowerer::lowerType(const TypePtr& type) const {
    TypePtr loweredTypeExpr = expandAliases(type);
    if (!loweredTypeExpr) {
        loweredTypeExpr = type;
    }
    if (!loweredTypeExpr) return IRType(IRTypeKind::Any, "Any");
    for (auto it = genericTypeScopes.rbegin(); it != genericTypeScopes.rend(); ++it) {
        auto found = it->find(loweredTypeExpr->name);
        if (found != it->end() && loweredTypeExpr->genericArguments.empty()) {
            return found->second;
        }
    }
    if (loweredTypeExpr->kind == TypeKind::Function) return IRType(IRTypeKind::Object, loweredTypeExpr->toString(), loweredTypeExpr->nullable);
    if (loweredTypeExpr->kind == TypeKind::Union || loweredTypeExpr->kind == TypeKind::Intersection) {
        return IRType(IRTypeKind::Any, "Any", loweredTypeExpr->nullable);
    }
    if (loweredTypeExpr->name == "Any") return IRType(IRTypeKind::Any, "Any", loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "Void") return IRType(IRTypeKind::Void, "Void", loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "Null") return IRType(IRTypeKind::Null, "Null", true);
    if (loweredTypeExpr->name == "Bool") return IRType(IRTypeKind::Bool, "Bool", loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "Int") return IRType(IRTypeKind::Int, "Int", loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "Float" || loweredTypeExpr->name == "Float64") return IRType(IRTypeKind::Float, loweredTypeExpr->name, loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "String") return IRType(IRTypeKind::String, "String", loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "List") return IRType(IRTypeKind::List, loweredTypeExpr->toString(), loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "Tuple") return IRType(IRTypeKind::Tuple, loweredTypeExpr->toString(), loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "Map") return IRType(IRTypeKind::Map, loweredTypeExpr->toString(), loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "Set") return IRType(IRTypeKind::Set, loweredTypeExpr->toString(), loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "Range") return IRType(IRTypeKind::Range, "Range", loweredTypeExpr->nullable);
    if (loweredTypeExpr->name == "Future") return IRType(IRTypeKind::Future, loweredTypeExpr->toString(), loweredTypeExpr->nullable);
    return IRType(IRTypeKind::Object, loweredTypeExpr->toString(), loweredTypeExpr->nullable);
}

IRType IRLowerer::inferExpressionType(const Expression* expression) {
    if (!expression) return IRType(IRTypeKind::Void, "Void");
    switch (expression->kind) {
        case ExpressionKind::Identifier: {
            const auto* ident = static_cast<const IdentifierExpr*>(expression);
            IRType resolvedType = resolveWithCapture(ident->name);
            if (resolvedType.kind != IRTypeKind::Any || !resolvedType.name.empty()) return resolvedType;
            auto ctor = knownConstructors.find(ident->name);
            if (ctor != knownConstructors.end()) return ctor->second;
            return identifierTypeFallback(ident->name);
        }
        case ExpressionKind::Literal: {
            const auto* lit = static_cast<const LiteralExpr*>(expression);
            switch (lit->tokenType) {
                case TokenType::TRUE:
                case TokenType::FALSE: return IRType(IRTypeKind::Bool, "Bool");
                case TokenType::INTEGER: return IRType(IRTypeKind::Int, "Int");
                case TokenType::FLOAT: return IRType(IRTypeKind::Float, "Float");
                case TokenType::STRING:
                case TokenType::CHAR: return IRType(IRTypeKind::String, "String");
                case TokenType::NULL_KW: return IRType(IRTypeKind::Null, "Null");
                default: return IRType(IRTypeKind::Any, "Any");
            }
        }
        case ExpressionKind::Lambda: {
            const auto* lambda = static_cast<const LambdaExpr*>(expression);
            std::vector<TypePtr> parameterTypes;
            parameterTypes.reserve(lambda->parameters.size());
            for (const auto& parameter : lambda->parameters) {
                parameterTypes.push_back(parameter.second ? parameter.second : std::make_shared<Type>("Any"));
            }
            return lowerType(std::make_shared<Type>(std::move(parameterTypes),
                                                   lambda->returnType ? lambda->returnType : std::make_shared<Type>("Any")));
        }
        case ExpressionKind::Unary: {
            const auto* unary = static_cast<const UnaryExpr*>(expression);
            if (unary->op.type == TokenType::BANG || unary->op.type == TokenType::NOT) return IRType(IRTypeKind::Bool, "Bool");
            return inferExpressionType(unary->operand.get());
        }
        case ExpressionKind::Await: {
            const auto* awaitExpr = static_cast<const AwaitExpr*>(expression);
            IRType operandType = inferExpressionType(awaitExpr->operand.get());
            return operandType.kind == IRTypeKind::Future ? IRType(IRTypeKind::Any, "Any") : operandType;
        }
        case ExpressionKind::TypeCheck:
            return IRType(IRTypeKind::Bool, "Bool");
        case ExpressionKind::SafeCast: {
            const auto* castExpr = static_cast<const SafeCastExpr*>(expression);
            IRType result = lowerType(castExpr->targetType);
            if (result.kind != IRTypeKind::Null && result.kind != IRTypeKind::Any) {
                result.nullable = true;
            }
            return result;
        }
        case ExpressionKind::CheckedCast: {
            const auto* castExpr = static_cast<const CheckedCastExpr*>(expression);
            return lowerType(castExpr->targetType);
        }
        case ExpressionKind::Binary: {
            const auto* binary = static_cast<const BinaryExpr*>(expression);
            switch (binary->op.type) {
                case TokenType::EQ_EQ:
                case TokenType::BANG_EQ:
                case TokenType::LT:
                case TokenType::LTE:
                case TokenType::GT:
                case TokenType::GTE:
                case TokenType::AND:
                case TokenType::OR:
                    return IRType(IRTypeKind::Bool, "Bool");
                default:
                    return inferExpressionType(binary->left.get());
            }
        }
        case ExpressionKind::Call:
        case ExpressionKind::Member:
        case ExpressionKind::Index:
            return IRType(IRTypeKind::Any, "Any");
        case ExpressionKind::ListLiteral:
            return IRType(IRTypeKind::List, "List");
        case ExpressionKind::Assign:
            return inferExpressionType(static_cast<const AssignExpr*>(expression)->value.get());
        case ExpressionKind::NullCoalesce: {
            const auto* coalesce = static_cast<const NullCoalesceExpr*>(expression);
            IRType leftType = inferExpressionType(coalesce->left.get());
            return leftType.kind == IRTypeKind::Null ? inferExpressionType(coalesce->right.get()) : leftType;
        }
        case ExpressionKind::Range:
            return IRType(IRTypeKind::Range, "Range");
        case ExpressionKind::NewObject:
            return IRType(IRTypeKind::Object, static_cast<const NewExpr*>(expression)->typeName);
    }
    return IRType(IRTypeKind::Any, "Any");
}

IRExprPtr IRLowerer::lowerExpression(const Expression* expression) {
    if (!expression) return nullptr;
    IRType exprType = inferExpressionType(expression);
    switch (expression->kind) {
        case ExpressionKind::Identifier: {
            const auto* ident = static_cast<const IdentifierExpr*>(expression);
            return std::make_unique<IRIdentifierExpr>(ident->name, exprType, ident->loc);
        }
        case ExpressionKind::Literal: {
            const auto* lit = static_cast<const LiteralExpr*>(expression);
            return std::make_unique<IRLiteralExpr>(lit->lexeme, exprType, lit->loc);
        }
        case ExpressionKind::Lambda: {
            const auto* lambda = static_cast<const LambdaExpr*>(expression);
            IRFunction lowered;
            lowered.name = "__lambda_" + std::to_string(nextLambdaId++);
            lowered.returnType = lowerType(lambda->returnType ? lambda->returnType : std::make_shared<Type>("Any"));
            lowered.loc = lambda->loc;
            lowered.method = false;

            lambdaContexts.push_back(LambdaContext{scopes.size(), {}});
            pushScope();
            for (const auto& param : lambda->parameters) {
                IRType type = lowerType(param.second ? param.second : std::make_shared<Type>("Any"));
                lowered.parameters.push_back({param.first, type});
                bind(param.first, type);
            }
            lowered.body = lowerStatement(lambda->body.get());
            popScope();
            lowered.captures = std::move(lambdaContexts.back().captures);
            lambdaContexts.pop_back();

            std::vector<std::string> captures;
            captures.reserve(lowered.captures.size());
            for (const auto& capture : lowered.captures) {
                captures.push_back(capture.first);
            }
            loweredLambdas.push_back(std::move(lowered));
            auto lambdaExpr = std::make_unique<IRLambdaExpr>(loweredLambdas.back().name, std::move(captures), exprType, lambda->loc);
            return lambdaExpr;
        }
        case ExpressionKind::Unary: {
            const auto* unary = static_cast<const UnaryExpr*>(expression);
            return std::make_unique<IRUnaryExpr>(unary->op.lexeme, lowerExpression(unary->operand.get()), exprType, unary->loc);
        }
        case ExpressionKind::Await: {
            const auto* awaitExpr = static_cast<const AwaitExpr*>(expression);
            return std::make_unique<IRAwaitExpr>(lowerExpression(awaitExpr->operand.get()), exprType, awaitExpr->loc);
        }
        case ExpressionKind::TypeCheck: {
            const auto* typeCheck = static_cast<const TypeCheckExpr*>(expression);
            return std::make_unique<IRTypeCheckExpr>(
                lowerExpression(typeCheck->operand.get()),
                lowerType(typeCheck->targetType),
                exprType,
                typeCheck->loc);
        }
        case ExpressionKind::SafeCast: {
            const auto* castExpr = static_cast<const SafeCastExpr*>(expression);
            return std::make_unique<IRSafeCastExpr>(
                lowerExpression(castExpr->operand.get()),
                lowerType(castExpr->targetType),
                exprType,
                castExpr->loc);
        }
        case ExpressionKind::CheckedCast: {
            const auto* castExpr = static_cast<const CheckedCastExpr*>(expression);
            return std::make_unique<IRCheckedCastExpr>(
                lowerExpression(castExpr->operand.get()),
                lowerType(castExpr->targetType),
                exprType,
                castExpr->loc);
        }
        case ExpressionKind::Binary: {
            const auto* binary = static_cast<const BinaryExpr*>(expression);
            return std::make_unique<IRBinaryExpr>(binary->op.lexeme, lowerExpression(binary->left.get()), lowerExpression(binary->right.get()), exprType, binary->loc);
        }
        case ExpressionKind::Call: {
            const auto* call = static_cast<const CallExpr*>(expression);
            std::vector<IRExprPtr> args;
            for (const auto& arg : call->arguments) args.push_back(lowerExpression(arg.get()));
            return std::make_unique<IRCallExpr>(lowerExpression(call->callee.get()), std::move(args), exprType, call->loc);
        }
        case ExpressionKind::Member: {
            const auto* member = static_cast<const MemberExpr*>(expression);
            return std::make_unique<IRMemberExpr>(lowerExpression(member->object.get()), member->member, member->safe, exprType, member->loc);
        }
        case ExpressionKind::Index: {
            const auto* index = static_cast<const IndexExpr*>(expression);
            return std::make_unique<IRIndexExpr>(lowerExpression(index->object.get()), lowerExpression(index->index.get()), exprType, index->loc);
        }
        case ExpressionKind::ListLiteral: {
            const auto* list = static_cast<const ListExpr*>(expression);
            std::vector<IRExprPtr> elements;
            for (const auto& element : list->elements) elements.push_back(lowerExpression(element.get()));
            return std::make_unique<IRListExpr>(std::move(elements), exprType, list->loc);
        }
        case ExpressionKind::Assign: {
            const auto* assign = static_cast<const AssignExpr*>(expression);
            return std::make_unique<IRAssignExpr>(lowerExpression(assign->target.get()), lowerExpression(assign->value.get()), exprType, assign->loc);
        }
        case ExpressionKind::NullCoalesce: {
            const auto* coalesce = static_cast<const NullCoalesceExpr*>(expression);
            return std::make_unique<IRNullCoalesceExpr>(lowerExpression(coalesce->left.get()), lowerExpression(coalesce->right.get()), exprType, coalesce->loc);
        }
        case ExpressionKind::Range: {
            const auto* range = static_cast<const RangeExpr*>(expression);
            return std::make_unique<IRRangeExpr>(lowerExpression(range->start.get()), lowerExpression(range->end.get()), range->inclusive, exprType, range->loc);
        }
        case ExpressionKind::NewObject: {
            const auto* created = static_cast<const NewExpr*>(expression);
            std::vector<IRExprPtr> args;
            for (const auto& arg : created->arguments) args.push_back(lowerExpression(arg.get()));
            return std::make_unique<IRConstructExpr>(created->typeName, std::move(args), exprType, created->loc);
        }
    }
    error(expression->loc, "unsupported AST expression during IR lowering");
    return nullptr;
}

IRStmtPtr IRLowerer::lowerStatement(const Statement* statement) {
    switch (statement->kind) {
        case StatementKind::Block:
            return lowerBlockLike(statement);
        case StatementKind::VarDecl: {
            const auto* var = static_cast<const VarDeclStmt*>(statement);
            IRType declared = var->declaredType ? lowerType(var->declaredType) : (var->initializer ? inferExpressionType(var->initializer.get()) : IRType(IRTypeKind::Any, "Any"));
            bind(var->name, declared);
            return std::make_unique<IRVarDeclStmt>(var->name, declared, lowerExpression(var->initializer.get()), var->mutableBinding, var->loc);
        }
        case StatementKind::ExpressionStmt:
            return std::make_unique<IRExprStmt>(lowerExpression(static_cast<const ExpressionStmt*>(statement)->expression.get()), statement->loc);
        case StatementKind::IfStmt: {
            const auto* ifStmt = static_cast<const IfStmt*>(statement);
            return std::make_unique<IRIfStmt>(
                lowerExpression(ifStmt->condition.get()),
                lowerStatement(ifStmt->thenBranch.get()),
                ifStmt->elseBranch ? lowerStatement(ifStmt->elseBranch.get()) : nullptr,
                ifStmt->loc);
        }
        case StatementKind::WhileStmt: {
            const auto* whileStmt = static_cast<const WhileStmt*>(statement);
            return std::make_unique<IRWhileStmt>(lowerExpression(whileStmt->condition.get()), lowerStatement(whileStmt->body.get()), whileStmt->loc);
        }
        case StatementKind::ForStmt: {
            const auto* forStmt = static_cast<const ForStmt*>(statement);
            pushScope();
            bind(forStmt->name, IRType(IRTypeKind::Any, "Any"));
            auto lowered = std::make_unique<IRForStmt>(forStmt->name, lowerExpression(forStmt->iterable.get()), lowerStatement(forStmt->body.get()), forStmt->loc);
            popScope();
            return lowered;
        }
        case StatementKind::MatchStmt: {
            const auto* matchStmt = static_cast<const MatchStmt*>(statement);
            auto lowered = std::make_unique<IRBlockStmt>(matchStmt->loc);

            const std::string tempName = "$match_" + std::to_string(nextMatchId++);
            IRExprPtr matchedValue = lowerExpression(matchStmt->value.get());
            IRType matchedType = matchedValue ? matchedValue->type : IRType(IRTypeKind::Any, "Any");
            lowered->statements.push_back(std::make_unique<IRVarDeclStmt>(
                tempName,
                matchedType,
                std::move(matchedValue),
                false,
                matchStmt->loc));

            pushScope();
            bind(tempName, matchedType);
            IRStmtPtr elseBranch;
            for (auto caseIt = matchStmt->cases.rbegin(); caseIt != matchStmt->cases.rend(); ++caseIt) {
                const auto& matchCase = *caseIt;
                IRStmtPtr caseBody = lowerStatement(matchCase.body.get());
                if (matchCase.kind == MatchPatternKind::Wildcard) {
                    elseBranch = std::move(caseBody);
                    continue;
                }

                IRExprPtr condition;
                if (matchCase.kind == MatchPatternKind::Type) {
                    condition = std::make_unique<IRTypeCheckExpr>(
                        std::make_unique<IRIdentifierExpr>(tempName, matchedType, matchCase.loc),
                        lowerType(std::make_shared<Type>(matchCase.typeName)),
                        IRType(IRTypeKind::Bool, "Bool"),
                        matchCase.loc);
                } else {
                    condition = std::make_unique<IRBinaryExpr>(
                        "==",
                        std::make_unique<IRIdentifierExpr>(tempName, matchedType, matchCase.loc),
                        lowerExpression(matchCase.pattern.get()),
                        IRType(IRTypeKind::Bool, "Bool"),
                        matchCase.loc);
                }

                elseBranch = std::make_unique<IRIfStmt>(
                    std::move(condition),
                    std::move(caseBody),
                    std::move(elseBranch),
                    matchCase.loc);
            }
            popScope();

            if (elseBranch) {
                lowered->statements.push_back(std::move(elseBranch));
            }
            return lowered;
        }
        case StatementKind::TryStmt: {
            const auto* tryStmt = static_cast<const TryStmt*>(statement);
            pushScope();
            bind(tryStmt->catchName, IRType(IRTypeKind::Any, "Any"));
            auto loweredCatch = lowerStatement(tryStmt->catchBranch.get());
            popScope();
            return std::make_unique<IRTryStmt>(
                lowerStatement(tryStmt->tryBranch.get()),
                tryStmt->catchName,
                std::move(loweredCatch),
                tryStmt->loc);
        }
        case StatementKind::ThrowStmt:
            return std::make_unique<IRThrowStmt>(
                lowerExpression(static_cast<const ThrowStmt*>(statement)->value.get()),
                statement->loc);
        case StatementKind::ReturnStmt:
            return std::make_unique<IRReturnStmt>(lowerExpression(static_cast<const ReturnStmt*>(statement)->value.get()), statement->loc);
        case StatementKind::BreakStmt:
            return std::make_unique<IRBreakStmt>(statement->loc);
        case StatementKind::ContinueStmt:
            return std::make_unique<IRContinueStmt>(statement->loc);
        default:
            error(statement->loc, "unsupported AST statement during IR lowering");
            return nullptr;
    }
}

IRStmtPtr IRLowerer::lowerBlockLike(const Statement* statement) {
    const auto* block = static_cast<const BlockStmt*>(statement);
    auto lowered = std::make_unique<IRBlockStmt>(block->loc);
    pushScope();
    for (const auto& stmt : block->statements) {
        auto irStmt = lowerStatement(stmt.get());
        if (irStmt) lowered->statements.push_back(std::move(irStmt));
    }
    popScope();
    return lowered;
}

IRFunction IRLowerer::lowerFunction(const FunctionDecl* function, bool method) {
    IRFunction lowered;
    lowered.name = function->name;
    lowered.returnType = lowerType(function->returnType);
    lowered.genericParameters = genericParamNames(function->genericParameters);
    lowered.async = function->isAsync;
    lowered.loc = function->loc;
    lowered.method = method;
    lowered.ownerType = currentClassName;

    pushGenericTypeScope(function->genericParameters);
    pushScope();
    if (method) bind("self", IRType(IRTypeKind::Object, currentClassName));
    for (const auto& param : function->parameters) {
        IRType type = lowerType(param.second);
        lowered.parameters.push_back({param.first, type});
        bind(param.first, type);
    }
    lowered.body = lowerStatement(function->body.get());
    popScope();
    popGenericTypeScope();
    return lowered;
}

IRInterfaceType IRLowerer::lowerInterface(const InterfaceDecl* iface) {
    IRInterfaceType lowered;
    lowered.name = iface->name;
    lowered.baseInterfaces = iface->baseInterfaces;
    lowered.loc = iface->loc;
    pushGenericTypeScope(iface->genericParameters);
    for (const auto& method : iface->methods) {
        pushGenericTypeScope(method.genericParameters);
        IRInterfaceMethod loweredMethod;
        loweredMethod.name = method.name;
        loweredMethod.returnType = lowerType(method.returnType);
        loweredMethod.async = method.isAsync;
        loweredMethod.staticMethod = method.isStatic;
        loweredMethod.abstractMethod = method.isAbstract;
        loweredMethod.visibility = method.visibility;
        loweredMethod.loc = iface->loc;
        for (const auto& param : method.parameters) {
            loweredMethod.parameters.push_back({param.first, lowerType(param.second)});
        }
        lowered.methods.push_back(std::move(loweredMethod));
        popGenericTypeScope();
    }
    popGenericTypeScope();
    return lowered;
}

IRClassType IRLowerer::lowerClass(const ClassDecl* klass) {
    IRClassType lowered;
    lowered.name = klass->name;
    lowered.baseClass = klass->baseClass;
    pushGenericTypeScope(klass->genericParameters);
    lowered.abstractClass = klass->isAbstract;
    pushScope();
    bind("self", IRType(IRTypeKind::Object, klass->name));
    for (const auto& field : klass->fields) {
        bind(field.name, field.declaredType ? lowerType(field.declaredType) : IRType(IRTypeKind::Any, "Any"));
    }
    for (const auto& field : klass->fields) {
        IRFieldDecl loweredField;
        loweredField.name = field.name;
        loweredField.type = lowerType(field.declaredType);
        loweredField.visibility = field.visibility;
        loweredField.loc = field.loc;
        if (field.defaultValue) {
            loweredField.defaultValue = lowerExpression(field.defaultValue.get());
        }
        lowered.fields.push_back(std::move(loweredField));
    }
    popScope();
    lowered.implementedInterfaces = klass->implementedInterfaces;
    lowered.loc = klass->loc;

    std::string previousClass = currentClassName;
    currentClassName = klass->name;
    for (const auto& method : klass->methods) {
        IRFunction loweredMethod;
        loweredMethod.name = method.isConstructor ? klass->name : method.name;
        loweredMethod.returnType = method.isConstructor ? IRType(IRTypeKind::Void, "Void") : lowerType(method.returnType);
        loweredMethod.genericParameters = genericParamNames(method.genericParameters);
        loweredMethod.async = method.isAsync;
        loweredMethod.method = true;
        loweredMethod.staticMethod = method.isStatic;
        loweredMethod.constructor = method.isConstructor;
        loweredMethod.abstractMethod = method.isAbstract;
        loweredMethod.visibility = method.visibility;
        loweredMethod.ownerType = klass->name;
        loweredMethod.loc = klass->loc;
        pushGenericTypeScope(method.genericParameters);
        pushScope();
        if (!method.isStatic) {
            bind("self", IRType(IRTypeKind::Object, klass->name));
            if (!klass->baseClass.empty()) {
                bind("super", IRType(IRTypeKind::Object, klass->baseClass));
            }
            for (const auto& field : klass->fields) bind(field.name, field.declaredType ? lowerType(field.declaredType) : IRType(IRTypeKind::Any, "Any"));
        }
        for (const auto& param : method.parameters) {
            IRType type = lowerType(param.second);
            loweredMethod.parameters.push_back({param.first, type});
            bind(param.first, type);
        }
        if (method.body) {
            loweredMethod.body = lowerStatement(method.body.get());
        }
        popScope();
        popGenericTypeScope();
        lowered.methods.push_back(std::move(loweredMethod));
    }
    currentClassName = previousClass;
    popGenericTypeScope();
    return lowered;
}

IRDataType IRLowerer::lowerData(const DataDecl* data) {
    IRDataType lowered;
    lowered.name = data->name;
    pushGenericTypeScope(data->genericParameters);
    pushScope();
    bind("self", IRType(IRTypeKind::Object, data->name));
    for (const auto& field : data->fields) {
        bind(field.name, field.declaredType ? lowerType(field.declaredType) : IRType(IRTypeKind::Any, "Any"));
    }
    for (const auto& field : data->fields) {
        IRFieldDecl loweredField;
        loweredField.name = field.name;
        loweredField.type = lowerType(field.declaredType);
        loweredField.visibility = field.visibility;
        loweredField.loc = field.loc;
        if (field.defaultValue) {
            loweredField.defaultValue = lowerExpression(field.defaultValue.get());
        }
        lowered.fields.push_back(std::move(loweredField));
    }
    popScope();
    popGenericTypeScope();
    lowered.loc = data->loc;
    return lowered;
}

void IRLowerer::pushScope() { scopes.emplace_back(); }
void IRLowerer::popScope() { if (!scopes.empty()) scopes.pop_back(); }
void IRLowerer::pushGenericTypeScope(const std::vector<GenericParamDecl>& genericParameters) {
    genericTypeScopes.emplace_back();
    for (const auto& parameter : genericParameters) {
        if (!parameter.bounds.empty()) {
            genericTypeScopes.back()[parameter.name] = lowerType(parameter.bounds.front());
        } else {
            genericTypeScopes.back()[parameter.name] = IRType(IRTypeKind::Any, "Any");
        }
    }
}
void IRLowerer::popGenericTypeScope() { if (!genericTypeScopes.empty()) genericTypeScopes.pop_back(); }
void IRLowerer::bind(const std::string& name, const IRType& type) {
    if (scopes.empty()) pushScope();
    scopes.back()[name] = type;
}

IRType IRLowerer::resolve(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return IRType(IRTypeKind::Any, "");
}

IRType IRLowerer::resolveWithCapture(const std::string& name) {
    for (size_t i = scopes.size(); i > 0; --i) {
        auto found = scopes[i - 1].find(name);
        if (found == scopes[i - 1].end()) {
            continue;
        }
        if (!lambdaContexts.empty() && (i - 1) < lambdaContexts.back().outerScopeDepth) {
            auto& captures = lambdaContexts.back().captures;
            auto captureIt = std::find_if(captures.begin(), captures.end(),
                [&](const auto& capture) { return capture.first == name; });
            if (captureIt == captures.end()) {
                captures.push_back({name, found->second});
            }
        }
        return found->second;
    }
    return IRType(IRTypeKind::Any, "");
}

void IRLowerer::error(const SourceLocation& loc, const std::string& message) {
    errors.push_back(locString(loc) + ": " + message);
}

bool IRValidator::validate(const IRProgram* program) {
    errors.clear();
    loopDepth = 0;
    if (!program) {
        errors.push_back("internal error: null IR program");
        return false;
    }
    bool hasMain = false;
    for (const auto& function : program->functions) {
        if (function.name == "main") {
            hasMain = true;
            if (!function.parameters.empty()) error(function.loc, "'main' must not take parameters in IR");
        }
        validateFunction(function);
    }
    for (const auto& klass : program->classes) {
        for (const auto& method : klass.methods) {
            validateFunction(method);
        }
    }
    for (const auto& global : program->globals) validateStatement(global.get(), false);
    if (!hasMain) error(program->loc, "IR program is missing 'main'");
    return errors.empty();
}

const std::vector<std::string>& IRValidator::getErrors() const { return errors; }

void IRValidator::validateFunction(const IRFunction& function) {
    if (!function.body) {
        if (function.abstractMethod) {
            return;
        }
        error(function.loc, "function '" + function.name + "' has no IR body");
        return;
    }
    validateStatement(function.body.get(), true);
}

void IRValidator::validateStatement(const IRStatement* statement, bool insideFunction) {
    if (!statement) {
        errors.push_back("IR validation error: null statement");
        return;
    }
    switch (statement->kind) {
        case IRStmtKind::Block: {
            const auto* block = static_cast<const IRBlockStmt*>(statement);
            for (const auto& stmt : block->statements) validateStatement(stmt.get(), insideFunction);
            return;
        }
        case IRStmtKind::VarDecl: {
            const auto* var = static_cast<const IRVarDeclStmt*>(statement);
            if (var->initializer) validateExpression(var->initializer.get());
            return;
        }
        case IRStmtKind::Expr:
            validateExpression(static_cast<const IRExprStmt*>(statement)->expression.get());
            return;
        case IRStmtKind::If: {
            const auto* ifStmt = static_cast<const IRIfStmt*>(statement);
            validateExpression(ifStmt->condition.get());
            validateStatement(ifStmt->thenBranch.get(), insideFunction);
            if (ifStmt->elseBranch) validateStatement(ifStmt->elseBranch.get(), insideFunction);
            return;
        }
        case IRStmtKind::While: {
            const auto* whileStmt = static_cast<const IRWhileStmt*>(statement);
            validateExpression(whileStmt->condition.get());
            ++loopDepth;
            validateStatement(whileStmt->body.get(), insideFunction);
            --loopDepth;
            return;
        }
        case IRStmtKind::For: {
            const auto* forStmt = static_cast<const IRForStmt*>(statement);
            validateExpression(forStmt->iterable.get());
            ++loopDepth;
            validateStatement(forStmt->body.get(), insideFunction);
            --loopDepth;
            return;
        }
        case IRStmtKind::Try: {
            const auto* tryStmt = static_cast<const IRTryStmt*>(statement);
            validateStatement(tryStmt->tryBranch.get(), insideFunction);
            validateStatement(tryStmt->catchBranch.get(), insideFunction);
            return;
        }
        case IRStmtKind::Throw: {
            const auto* throwStmt = static_cast<const IRThrowStmt*>(statement);
            if (throwStmt->value) validateExpression(throwStmt->value.get());
            return;
        }
        case IRStmtKind::Return: {
            const auto* ret = static_cast<const IRReturnStmt*>(statement);
            if (!insideFunction) error(ret->loc, "IR return outside function");
            if (ret->value) validateExpression(ret->value.get());
            return;
        }
        case IRStmtKind::Break:
            if (loopDepth == 0) error(statement->loc, "IR break outside loop");
            return;
        case IRStmtKind::Continue:
            if (loopDepth == 0) error(statement->loc, "IR continue outside loop");
            return;
    }
}

void IRValidator::validateExpression(const IRExpression* expression) {
    if (!expression) {
        errors.push_back("IR validation error: null expression");
        return;
    }
    if (expression->type.toString().empty()) error(expression->loc, "IR expression has empty type");
    switch (expression->kind) {
        case IRExprKind::Identifier:
        case IRExprKind::Literal:
            return;
        case IRExprKind::Lambda:
            return;
        case IRExprKind::Unary:
            validateExpression(static_cast<const IRUnaryExpr*>(expression)->operand.get());
            return;
        case IRExprKind::Await:
            validateExpression(static_cast<const IRAwaitExpr*>(expression)->operand.get());
            return;
        case IRExprKind::TypeCheck:
            validateExpression(static_cast<const IRTypeCheckExpr*>(expression)->operand.get());
            return;
        case IRExprKind::SafeCast:
            validateExpression(static_cast<const IRSafeCastExpr*>(expression)->operand.get());
            return;
        case IRExprKind::CheckedCast:
            validateExpression(static_cast<const IRCheckedCastExpr*>(expression)->operand.get());
            return;
        case IRExprKind::Binary: {
            const auto* binary = static_cast<const IRBinaryExpr*>(expression);
            validateExpression(binary->left.get());
            validateExpression(binary->right.get());
            return;
        }
        case IRExprKind::Call: {
            const auto* call = static_cast<const IRCallExpr*>(expression);
            validateExpression(call->callee.get());
            for (const auto& arg : call->arguments) validateExpression(arg.get());
            return;
        }
        case IRExprKind::Member:
            validateExpression(static_cast<const IRMemberExpr*>(expression)->object.get());
            return;
        case IRExprKind::Index: {
            const auto* index = static_cast<const IRIndexExpr*>(expression);
            validateExpression(index->object.get());
            validateExpression(index->index.get());
            return;
        }
        case IRExprKind::List: {
            const auto* list = static_cast<const IRListExpr*>(expression);
            for (const auto& element : list->elements) validateExpression(element.get());
            return;
        }
        case IRExprKind::Assign: {
            const auto* assign = static_cast<const IRAssignExpr*>(expression);
            validateExpression(assign->target.get());
            validateExpression(assign->value.get());
            return;
        }
        case IRExprKind::NullCoalesce: {
            const auto* coalesce = static_cast<const IRNullCoalesceExpr*>(expression);
            validateExpression(coalesce->left.get());
            validateExpression(coalesce->right.get());
            return;
        }
        case IRExprKind::Range: {
            const auto* range = static_cast<const IRRangeExpr*>(expression);
            validateExpression(range->start.get());
            validateExpression(range->end.get());
            return;
        }
        case IRExprKind::Construct: {
            const auto* construct = static_cast<const IRConstructExpr*>(expression);
            for (const auto& arg : construct->arguments) validateExpression(arg.get());
            return;
        }
    }
}

void IRValidator::error(const SourceLocation& loc, const std::string& message) {
    errors.push_back(locString(loc) + ": " + message);
}

std::string IRPrinter::print(const IRProgram* program) const {
    if (!program) return "ir <null>\n";
    std::string out = "ir-program {\n";
    for (const auto& data : program->dataTypes) {
        indent(out, 1);
        out += "data " + data.name + "(";
        for (size_t i = 0; i < data.fields.size(); ++i) {
            if (i > 0) out += ", ";
            out += data.fields[i].name + ": " + data.fields[i].type.toString();
        }
        out += ")\n";
    }
    for (const auto& iface : program->interfaces) {
        indent(out, 1);
        out += "interface " + iface.name;
        if (!iface.baseInterfaces.empty()) {
            out += " extends ";
            for (size_t i = 0; i < iface.baseInterfaces.size(); ++i) {
                if (i > 0) out += ", ";
                out += iface.baseInterfaces[i];
            }
        }
        out += " {\n";
        for (const auto& method : iface.methods) {
            indent(out, 2);
            out += "fn " + method.name + "(";
            for (size_t i = 0; i < method.parameters.size(); ++i) {
                if (i > 0) out += ", ";
                out += method.parameters[i].first + ": " + method.parameters[i].second.toString();
            }
            out += ") -> " + method.returnType.toString() + "\n";
        }
        indent(out, 1);
        out += "}\n";
    }
    for (const auto& klass : program->classes) {
        indent(out, 1);
        out += "class " + klass.name + "(";
        for (size_t i = 0; i < klass.fields.size(); ++i) {
            if (i > 0) out += ", ";
            out += klass.fields[i].name + ": " + klass.fields[i].type.toString();
        }
        out += ") {\n";
        for (const auto& method : klass.methods) {
            indent(out, 2);
            out += std::string(method.async ? "async method " : "method ") + method.name + "(";
            for (size_t i = 0; i < method.parameters.size(); ++i) {
                if (i > 0) out += ", ";
                out += method.parameters[i].first + ": " + method.parameters[i].second.toString();
            }
            out += ") -> " + method.returnType.toString() + "\n";
            printStatement(method.body.get(), out, 2);
        }
        indent(out, 1);
        out += "}\n";
    }
    for (const auto& global : program->globals) printStatement(global.get(), out, 1);
    for (const auto& function : program->functions) {
        indent(out, 1);
        out += std::string(function.async ? "async fn " : "fn ") + function.name + "(";
        for (size_t i = 0; i < function.parameters.size(); ++i) {
            if (i > 0) out += ", ";
            out += function.parameters[i].first + ": " + function.parameters[i].second.toString();
        }
        out += ") -> " + function.returnType.toString() + "\n";
        if (!function.captures.empty()) {
            indent(out, 2);
            out += "captures ";
            for (size_t i = 0; i < function.captures.size(); ++i) {
                if (i > 0) out += ", ";
                out += function.captures[i].first + ": " + function.captures[i].second.toString();
            }
            out += "\n";
        }
        printStatement(function.body.get(), out, 1);
    }
    out += "}\n";
    return out;
}

void IRPrinter::printStatement(const IRStatement* statement, std::string& out, int indentDepth) const {
    if (!statement) {
        indent(out, indentDepth);
        out += "<null-stmt>\n";
        return;
    }
    switch (statement->kind) {
        case IRStmtKind::Block: {
            const auto* block = static_cast<const IRBlockStmt*>(statement);
            indent(out, indentDepth);
            out += "{\n";
            for (const auto& stmt : block->statements) printStatement(stmt.get(), out, indentDepth + 1);
            indent(out, indentDepth);
            out += "}\n";
            return;
        }
        case IRStmtKind::VarDecl: {
            const auto* var = static_cast<const IRVarDeclStmt*>(statement);
            indent(out, indentDepth);
            out += std::string(var->mutableBinding ? "var " : "let ") + var->name + ": " + var->declaredType.toString();
            if (var->initializer) {
                out += " = ";
                printExpression(var->initializer.get(), out);
            }
            out += "\n";
            return;
        }
        case IRStmtKind::Expr:
            indent(out, indentDepth);
            printExpression(static_cast<const IRExprStmt*>(statement)->expression.get(), out);
            out += "\n";
            return;
        case IRStmtKind::If: {
            const auto* ifStmt = static_cast<const IRIfStmt*>(statement);
            indent(out, indentDepth);
            out += "if ";
            printExpression(ifStmt->condition.get(), out);
            out += "\n";
            printStatement(ifStmt->thenBranch.get(), out, indentDepth + 1);
            if (ifStmt->elseBranch) {
                indent(out, indentDepth);
                out += "else\n";
                printStatement(ifStmt->elseBranch.get(), out, indentDepth + 1);
            }
            return;
        }
        case IRStmtKind::While: {
            const auto* whileStmt = static_cast<const IRWhileStmt*>(statement);
            indent(out, indentDepth);
            out += "while ";
            printExpression(whileStmt->condition.get(), out);
            out += "\n";
            printStatement(whileStmt->body.get(), out, indentDepth + 1);
            return;
        }
        case IRStmtKind::For: {
            const auto* forStmt = static_cast<const IRForStmt*>(statement);
            indent(out, indentDepth);
            out += "for " + forStmt->name + " in ";
            printExpression(forStmt->iterable.get(), out);
            out += "\n";
            printStatement(forStmt->body.get(), out, indentDepth + 1);
            return;
        }
        case IRStmtKind::Try: {
            const auto* tryStmt = static_cast<const IRTryStmt*>(statement);
            indent(out, indentDepth);
            out += "try\n";
            printStatement(tryStmt->tryBranch.get(), out, indentDepth + 1);
            indent(out, indentDepth);
            out += "catch " + tryStmt->catchName + "\n";
            printStatement(tryStmt->catchBranch.get(), out, indentDepth + 1);
            return;
        }
        case IRStmtKind::Throw:
            indent(out, indentDepth);
            out += "throw";
            if (static_cast<const IRThrowStmt*>(statement)->value) {
                out += " ";
                printExpression(static_cast<const IRThrowStmt*>(statement)->value.get(), out);
            }
            out += "\n";
            return;
        case IRStmtKind::Return:
            indent(out, indentDepth);
            out += "return";
            if (static_cast<const IRReturnStmt*>(statement)->value) {
                out += " ";
                printExpression(static_cast<const IRReturnStmt*>(statement)->value.get(), out);
            }
            out += "\n";
            return;
        case IRStmtKind::Break:
            indent(out, indentDepth);
            out += "break\n";
            return;
        case IRStmtKind::Continue:
            indent(out, indentDepth);
            out += "continue\n";
            return;
    }
}

void IRPrinter::printExpression(const IRExpression* expression, std::string& out) const {
    if (!expression) {
        out += "<null-expr>";
        return;
    }
    switch (expression->kind) {
        case IRExprKind::Identifier:
            out += static_cast<const IRIdentifierExpr*>(expression)->name;
            break;
        case IRExprKind::Literal:
            out += static_cast<const IRLiteralExpr*>(expression)->value;
            break;
        case IRExprKind::Lambda: {
            const auto* lambda = static_cast<const IRLambdaExpr*>(expression);
            out += "<lambda " + lambda->functionName;
            if (!lambda->captures.empty()) {
                out += " captures(";
                for (size_t i = 0; i < lambda->captures.size(); ++i) {
                    if (i > 0) out += ", ";
                    out += lambda->captures[i];
                }
                out += ")";
            }
            out += ">";
            break;
        }
        case IRExprKind::Unary: {
            const auto* unary = static_cast<const IRUnaryExpr*>(expression);
            out += "(" + unary->op;
            printExpression(unary->operand.get(), out);
            out += ")";
            break;
        }
        case IRExprKind::Await: {
            const auto* awaitExpr = static_cast<const IRAwaitExpr*>(expression);
            out += "await ";
            printExpression(awaitExpr->operand.get(), out);
            break;
        }
        case IRExprKind::TypeCheck: {
            const auto* typeCheck = static_cast<const IRTypeCheckExpr*>(expression);
            out += "(";
            printExpression(typeCheck->operand.get(), out);
            out += " is " + typeCheck->targetType.toString() + ")";
            break;
        }
        case IRExprKind::SafeCast: {
            const auto* castExpr = static_cast<const IRSafeCastExpr*>(expression);
            out += "(";
            printExpression(castExpr->operand.get(), out);
            out += " as " + castExpr->targetType.toString() + ")";
            break;
        }
        case IRExprKind::CheckedCast: {
            const auto* castExpr = static_cast<const IRCheckedCastExpr*>(expression);
            out += "cast<" + castExpr->targetType.toString() + ">(";
            printExpression(castExpr->operand.get(), out);
            out += ")";
            break;
        }
        case IRExprKind::Binary: {
            const auto* binary = static_cast<const IRBinaryExpr*>(expression);
            out += "(";
            printExpression(binary->left.get(), out);
            out += " " + binary->op + " ";
            printExpression(binary->right.get(), out);
            out += ")";
            break;
        }
        case IRExprKind::Call: {
            const auto* call = static_cast<const IRCallExpr*>(expression);
            printExpression(call->callee.get(), out);
            out += "(";
            for (size_t i = 0; i < call->arguments.size(); ++i) {
                if (i > 0) out += ", ";
                printExpression(call->arguments[i].get(), out);
            }
            out += ")";
            break;
        }
        case IRExprKind::Member: {
            const auto* member = static_cast<const IRMemberExpr*>(expression);
            printExpression(member->object.get(), out);
            out += member->safe ? "?." : ".";
            out += member->member;
            break;
        }
        case IRExprKind::Index: {
            const auto* index = static_cast<const IRIndexExpr*>(expression);
            printExpression(index->object.get(), out);
            out += "[";
            printExpression(index->index.get(), out);
            out += "]";
            break;
        }
        case IRExprKind::List: {
            const auto* list = static_cast<const IRListExpr*>(expression);
            out += "[";
            for (size_t i = 0; i < list->elements.size(); ++i) {
                if (i > 0) out += ", ";
                printExpression(list->elements[i].get(), out);
            }
            out += "]";
            break;
        }
        case IRExprKind::Assign: {
            const auto* assign = static_cast<const IRAssignExpr*>(expression);
            printExpression(assign->target.get(), out);
            out += " = ";
            printExpression(assign->value.get(), out);
            break;
        }
        case IRExprKind::NullCoalesce: {
            const auto* coalesce = static_cast<const IRNullCoalesceExpr*>(expression);
            printExpression(coalesce->left.get(), out);
            out += " ?? ";
            printExpression(coalesce->right.get(), out);
            break;
        }
        case IRExprKind::Range: {
            const auto* range = static_cast<const IRRangeExpr*>(expression);
            printExpression(range->start.get(), out);
            out += range->inclusive ? "..." : "..";
            printExpression(range->end.get(), out);
            break;
        }
        case IRExprKind::Construct: {
            const auto* construct = static_cast<const IRConstructExpr*>(expression);
            out += "new " + construct->typeName + "(";
            for (size_t i = 0; i < construct->arguments.size(); ++i) {
                if (i > 0) out += ", ";
                printExpression(construct->arguments[i].get(), out);
            }
            out += ")";
            break;
        }
    }
    out += ":" + expression->type.toString();
}

void IRPrinter::indent(std::string& out, int depth) {
    out.append(static_cast<size_t>(depth) * 2, ' ');
}

std::unique_ptr<IRProgram> IROptimizer::optimize(std::unique_ptr<IRProgram> program) {
    if (!program) return nullptr;

    collectInlineCandidates(program.get());

    for (auto& global : program->globals) {
        optimizeStatement(global);
    }
    for (auto& function : program->functions) {
        optimizeFunction(function);
    }
    for (auto& klass : program->classes) {
        for (auto& method : klass.methods) {
            optimizeFunction(method);
        }
    }
    return program;
}

void IROptimizer::collectInlineCandidates(const IRProgram* program) {
    inlineCandidates.clear();
    if (!program) return;
    for (const auto& function : program->functions) {
        if (function.async) continue;
        const IRExpression* body = extractInlineBody(function);
        if (!body || !isPureExpression(body)) continue;
        if (body->kind == IRExprKind::Lambda) continue;
        if (function.name == "main") continue;
        inlineCandidates[function.name] = InlineCandidate{&function, body};
    }
}

void IROptimizer::optimizeFunction(IRFunction& function) {
    propagationScopes.clear();
    pushPropagationScope();
    optimizeStatement(function.body);
    popPropagationScope();
}

void IROptimizer::optimizeStatement(IRStmtPtr& statement) {
    if (!statement) return;

    switch (statement->kind) {
        case IRStmtKind::Block:
            optimizeBlock(static_cast<IRBlockStmt*>(statement.get()));
            break;
        case IRStmtKind::VarDecl: {
            auto* var = static_cast<IRVarDeclStmt*>(statement.get());
            optimizeExpression(var->initializer);
            if (!var->mutableBinding && var->initializer && isSimpleExpression(var->initializer.get())) {
                bindPropagation(var->name, var->initializer.get());
            } else {
                killPropagation(var->name);
            }
            break;
        }
        case IRStmtKind::Expr: {
            auto* expr = static_cast<IRExprStmt*>(statement.get());
            optimizeExpression(expr->expression);
            break;
        }
        case IRStmtKind::If: {
            auto* ifStmt = static_cast<IRIfStmt*>(statement.get());
            optimizeExpression(ifStmt->condition);
            pushPropagationScope();
            optimizeStatement(ifStmt->thenBranch);
            popPropagationScope();
            if (ifStmt->elseBranch) {
                pushPropagationScope();
                optimizeStatement(ifStmt->elseBranch);
                popPropagationScope();
            }
            bool condition = false;
            if (isConstantTruthy(ifStmt->condition.get(), condition)) {
                if (condition) {
                    statement = std::move(ifStmt->thenBranch);
                } else if (ifStmt->elseBranch) {
                    statement = std::move(ifStmt->elseBranch);
                } else {
                    auto empty = std::make_unique<IRBlockStmt>(ifStmt->loc);
                    statement = std::move(empty);
                }
                optimizeStatement(statement);
            }
            propagationScopes.clear();
            pushPropagationScope();
            break;
        }
        case IRStmtKind::While: {
            auto* whileStmt = static_cast<IRWhileStmt*>(statement.get());
            optimizeExpression(whileStmt->condition);
            pushPropagationScope();
            optimizeStatement(whileStmt->body);
            popPropagationScope();
            bool condition = false;
            if (isConstantTruthy(whileStmt->condition.get(), condition) && !condition) {
                auto empty = std::make_unique<IRBlockStmt>(whileStmt->loc);
                statement = std::move(empty);
            }
            propagationScopes.clear();
            pushPropagationScope();
            break;
        }
        case IRStmtKind::For: {
            auto* forStmt = static_cast<IRForStmt*>(statement.get());
            optimizeExpression(forStmt->iterable);
            pushPropagationScope();
            killPropagation(forStmt->name);
            optimizeStatement(forStmt->body);
            popPropagationScope();
            propagationScopes.clear();
            pushPropagationScope();
            break;
        }
        case IRStmtKind::Try: {
            auto* tryStmt = static_cast<IRTryStmt*>(statement.get());
            pushPropagationScope();
            optimizeStatement(tryStmt->tryBranch);
            popPropagationScope();
            pushPropagationScope();
            killPropagation(tryStmt->catchName);
            optimizeStatement(tryStmt->catchBranch);
            popPropagationScope();
            propagationScopes.clear();
            pushPropagationScope();
            break;
        }
        case IRStmtKind::Throw: {
            auto* throwStmt = static_cast<IRThrowStmt*>(statement.get());
            optimizeExpression(throwStmt->value);
            break;
        }
        case IRStmtKind::Return: {
            auto* ret = static_cast<IRReturnStmt*>(statement.get());
            optimizeExpression(ret->value);
            break;
        }
        case IRStmtKind::Break:
        case IRStmtKind::Continue:
            break;
    }
}

void IROptimizer::optimizeExpression(IRExprPtr& expression) {
    if (!expression) return;

    switch (expression->kind) {
        case IRExprKind::Identifier: {
            auto* ident = static_cast<IRIdentifierExpr*>(expression.get());
            if (const IRExpression* replacement = lookupPropagation(ident->name)) {
                expression = cloneExpression(replacement);
            }
            break;
        }
        case IRExprKind::Unary: {
            auto* unary = static_cast<IRUnaryExpr*>(expression.get());
            optimizeExpression(unary->operand);
            break;
        }
        case IRExprKind::Await: {
            auto* awaitExpr = static_cast<IRAwaitExpr*>(expression.get());
            optimizeExpression(awaitExpr->operand);
            break;
        }
        case IRExprKind::TypeCheck: {
            auto* typeCheck = static_cast<IRTypeCheckExpr*>(expression.get());
            optimizeExpression(typeCheck->operand);
            break;
        }
        case IRExprKind::SafeCast: {
            auto* castExpr = static_cast<IRSafeCastExpr*>(expression.get());
            optimizeExpression(castExpr->operand);
            break;
        }
        case IRExprKind::CheckedCast: {
            auto* castExpr = static_cast<IRCheckedCastExpr*>(expression.get());
            optimizeExpression(castExpr->operand);
            break;
        }
        case IRExprKind::Binary: {
            auto* binary = static_cast<IRBinaryExpr*>(expression.get());
            optimizeExpression(binary->left);
            optimizeExpression(binary->right);
            break;
        }
        case IRExprKind::Call: {
            auto* call = static_cast<IRCallExpr*>(expression.get());
            optimizeExpression(call->callee);
            for (auto& arg : call->arguments) optimizeExpression(arg);
            if (IRExprPtr inlined = tryInlineCall(call)) {
                expression = std::move(inlined);
                optimizeExpression(expression);
                return;
            }
            break;
        }
        case IRExprKind::Member: {
            auto* member = static_cast<IRMemberExpr*>(expression.get());
            optimizeExpression(member->object);
            break;
        }
        case IRExprKind::Index: {
            auto* index = static_cast<IRIndexExpr*>(expression.get());
            optimizeExpression(index->object);
            optimizeExpression(index->index);
            break;
        }
        case IRExprKind::List: {
            auto* list = static_cast<IRListExpr*>(expression.get());
            for (auto& element : list->elements) optimizeExpression(element);
            break;
        }
        case IRExprKind::Assign: {
            auto* assign = static_cast<IRAssignExpr*>(expression.get());
            optimizeExpression(assign->value);
            optimizeExpression(assign->target);
            if (assign->target && assign->target->kind == IRExprKind::Identifier) {
                killPropagation(static_cast<const IRIdentifierExpr*>(assign->target.get())->name);
            } else {
                propagationScopes.clear();
                pushPropagationScope();
            }
            break;
        }
        case IRExprKind::NullCoalesce: {
            auto* coalesce = static_cast<IRNullCoalesceExpr*>(expression.get());
            optimizeExpression(coalesce->left);
            optimizeExpression(coalesce->right);
            break;
        }
        case IRExprKind::Range: {
            auto* range = static_cast<IRRangeExpr*>(expression.get());
            optimizeExpression(range->start);
            optimizeExpression(range->end);
            break;
        }
        case IRExprKind::Construct: {
            auto* construct = static_cast<IRConstructExpr*>(expression.get());
            for (auto& arg : construct->arguments) optimizeExpression(arg);
            break;
        }
        case IRExprKind::Lambda:
        case IRExprKind::Literal:
            break;
    }

    expression = foldExpression(std::move(expression));
}

void IROptimizer::optimizeBlock(IRBlockStmt* block) {
    if (!block) return;

    pushPropagationScope();
    std::vector<IRStmtPtr> rewritten;
    rewritten.reserve(block->statements.size());

    bool terminated = false;
    for (auto& stmt : block->statements) {
        if (terminated) {
            continue;
        }
        optimizeStatement(stmt);
        if (!stmt) continue;

        if (stmt->kind == IRStmtKind::Block && blockCanBeFlattened(static_cast<const IRBlockStmt*>(stmt.get()))) {
            auto* nested = static_cast<IRBlockStmt*>(stmt.get());
            if (nested->statements.empty()) {
                continue;
            }
            for (auto& inner : nested->statements) {
                rewritten.push_back(std::move(inner));
                if (rewritten.back() && isTerminator(rewritten.back().get())) {
                    terminated = true;
                    break;
                }
            }
            continue;
        }

        if (stmt->kind == IRStmtKind::Expr &&
            static_cast<IRExprStmt*>(stmt.get())->expression &&
            static_cast<IRExprStmt*>(stmt.get())->expression->kind == IRExprKind::Literal) {
            continue;
        }

        terminated = isTerminator(stmt.get());
        rewritten.push_back(std::move(stmt));
    }

    block->statements = std::move(rewritten);
    popPropagationScope();
}

IRExprPtr IROptimizer::cloneExpression(const IRExpression* expression) const {
    if (!expression) return nullptr;
    switch (expression->kind) {
        case IRExprKind::Identifier: {
            const auto* ident = static_cast<const IRIdentifierExpr*>(expression);
            return std::make_unique<IRIdentifierExpr>(ident->name, ident->type, ident->loc);
        }
        case IRExprKind::Literal: {
            const auto* lit = static_cast<const IRLiteralExpr*>(expression);
            return std::make_unique<IRLiteralExpr>(lit->value, lit->type, lit->loc);
        }
        case IRExprKind::Lambda: {
            const auto* lambda = static_cast<const IRLambdaExpr*>(expression);
            return std::make_unique<IRLambdaExpr>(lambda->functionName, lambda->captures, lambda->type, lambda->loc);
        }
        case IRExprKind::Unary: {
            const auto* unary = static_cast<const IRUnaryExpr*>(expression);
            return std::make_unique<IRUnaryExpr>(unary->op, cloneExpression(unary->operand.get()), unary->type, unary->loc);
        }
        case IRExprKind::Await: {
            const auto* awaitExpr = static_cast<const IRAwaitExpr*>(expression);
            return std::make_unique<IRAwaitExpr>(cloneExpression(awaitExpr->operand.get()), awaitExpr->type, awaitExpr->loc);
        }
        case IRExprKind::TypeCheck: {
            const auto* typeCheck = static_cast<const IRTypeCheckExpr*>(expression);
            return std::make_unique<IRTypeCheckExpr>(
                cloneExpression(typeCheck->operand.get()),
                typeCheck->targetType,
                typeCheck->type,
                typeCheck->loc);
        }
        case IRExprKind::SafeCast: {
            const auto* castExpr = static_cast<const IRSafeCastExpr*>(expression);
            return std::make_unique<IRSafeCastExpr>(
                cloneExpression(castExpr->operand.get()),
                castExpr->targetType,
                castExpr->type,
                castExpr->loc);
        }
        case IRExprKind::CheckedCast: {
            const auto* castExpr = static_cast<const IRCheckedCastExpr*>(expression);
            return std::make_unique<IRCheckedCastExpr>(
                cloneExpression(castExpr->operand.get()),
                castExpr->targetType,
                castExpr->type,
                castExpr->loc);
        }
        case IRExprKind::Binary: {
            const auto* binary = static_cast<const IRBinaryExpr*>(expression);
            return std::make_unique<IRBinaryExpr>(
                binary->op,
                cloneExpression(binary->left.get()),
                cloneExpression(binary->right.get()),
                binary->type,
                binary->loc);
        }
        case IRExprKind::Call: {
            const auto* call = static_cast<const IRCallExpr*>(expression);
            std::vector<IRExprPtr> arguments;
            for (const auto& arg : call->arguments) arguments.push_back(cloneExpression(arg.get()));
            return std::make_unique<IRCallExpr>(cloneExpression(call->callee.get()), std::move(arguments), call->type, call->loc);
        }
        case IRExprKind::Member: {
            const auto* member = static_cast<const IRMemberExpr*>(expression);
            return std::make_unique<IRMemberExpr>(cloneExpression(member->object.get()), member->member, member->safe, member->type, member->loc);
        }
        case IRExprKind::Index: {
            const auto* index = static_cast<const IRIndexExpr*>(expression);
            return std::make_unique<IRIndexExpr>(cloneExpression(index->object.get()), cloneExpression(index->index.get()), index->type, index->loc);
        }
        case IRExprKind::List: {
            const auto* list = static_cast<const IRListExpr*>(expression);
            std::vector<IRExprPtr> elements;
            for (const auto& element : list->elements) elements.push_back(cloneExpression(element.get()));
            return std::make_unique<IRListExpr>(std::move(elements), list->type, list->loc);
        }
        case IRExprKind::Assign: {
            const auto* assign = static_cast<const IRAssignExpr*>(expression);
            return std::make_unique<IRAssignExpr>(cloneExpression(assign->target.get()), cloneExpression(assign->value.get()), assign->type, assign->loc);
        }
        case IRExprKind::NullCoalesce: {
            const auto* coalesce = static_cast<const IRNullCoalesceExpr*>(expression);
            return std::make_unique<IRNullCoalesceExpr>(cloneExpression(coalesce->left.get()), cloneExpression(coalesce->right.get()), coalesce->type, coalesce->loc);
        }
        case IRExprKind::Range: {
            const auto* range = static_cast<const IRRangeExpr*>(expression);
            return std::make_unique<IRRangeExpr>(cloneExpression(range->start.get()), cloneExpression(range->end.get()), range->inclusive, range->type, range->loc);
        }
        case IRExprKind::Construct: {
            const auto* construct = static_cast<const IRConstructExpr*>(expression);
            std::vector<IRExprPtr> arguments;
            for (const auto& arg : construct->arguments) arguments.push_back(cloneExpression(arg.get()));
            return std::make_unique<IRConstructExpr>(construct->typeName, std::move(arguments), construct->type, construct->loc);
        }
    }
    return nullptr;
}

IRExprPtr IROptimizer::substituteExpression(
    const IRExpression* expression,
    const std::unordered_map<std::string, const IRExpression*>& substitutions) const {
    if (!expression) return nullptr;
    if (expression->kind == IRExprKind::Identifier) {
        const auto* ident = static_cast<const IRIdentifierExpr*>(expression);
        auto it = substitutions.find(ident->name);
        if (it != substitutions.end()) {
            return cloneExpression(it->second);
        }
    }

    IRExprPtr result = cloneExpression(expression);
    if (!result) return nullptr;

    switch (result->kind) {
        case IRExprKind::Unary: {
            auto* unary = static_cast<IRUnaryExpr*>(result.get());
            unary->operand = substituteExpression(unary->operand.get(), substitutions);
            break;
        }
        case IRExprKind::Await: {
            auto* awaitExpr = static_cast<IRAwaitExpr*>(result.get());
            awaitExpr->operand = substituteExpression(awaitExpr->operand.get(), substitutions);
            break;
        }
        case IRExprKind::TypeCheck: {
            auto* typeCheck = static_cast<IRTypeCheckExpr*>(result.get());
            typeCheck->operand = substituteExpression(typeCheck->operand.get(), substitutions);
            break;
        }
        case IRExprKind::SafeCast: {
            auto* castExpr = static_cast<IRSafeCastExpr*>(result.get());
            castExpr->operand = substituteExpression(castExpr->operand.get(), substitutions);
            break;
        }
        case IRExprKind::CheckedCast: {
            auto* castExpr = static_cast<IRCheckedCastExpr*>(result.get());
            castExpr->operand = substituteExpression(castExpr->operand.get(), substitutions);
            break;
        }
        case IRExprKind::Binary: {
            auto* binary = static_cast<IRBinaryExpr*>(result.get());
            binary->left = substituteExpression(binary->left.get(), substitutions);
            binary->right = substituteExpression(binary->right.get(), substitutions);
            break;
        }
        case IRExprKind::Call: {
            auto* call = static_cast<IRCallExpr*>(result.get());
            call->callee = substituteExpression(call->callee.get(), substitutions);
            for (auto& arg : call->arguments) arg = substituteExpression(arg.get(), substitutions);
            break;
        }
        case IRExprKind::Member: {
            auto* member = static_cast<IRMemberExpr*>(result.get());
            member->object = substituteExpression(member->object.get(), substitutions);
            break;
        }
        case IRExprKind::Index: {
            auto* index = static_cast<IRIndexExpr*>(result.get());
            index->object = substituteExpression(index->object.get(), substitutions);
            index->index = substituteExpression(index->index.get(), substitutions);
            break;
        }
        case IRExprKind::List: {
            auto* list = static_cast<IRListExpr*>(result.get());
            for (auto& element : list->elements) element = substituteExpression(element.get(), substitutions);
            break;
        }
        case IRExprKind::Assign: {
            auto* assign = static_cast<IRAssignExpr*>(result.get());
            assign->target = substituteExpression(assign->target.get(), substitutions);
            assign->value = substituteExpression(assign->value.get(), substitutions);
            break;
        }
        case IRExprKind::NullCoalesce: {
            auto* coalesce = static_cast<IRNullCoalesceExpr*>(result.get());
            coalesce->left = substituteExpression(coalesce->left.get(), substitutions);
            coalesce->right = substituteExpression(coalesce->right.get(), substitutions);
            break;
        }
        case IRExprKind::Range: {
            auto* range = static_cast<IRRangeExpr*>(result.get());
            range->start = substituteExpression(range->start.get(), substitutions);
            range->end = substituteExpression(range->end.get(), substitutions);
            break;
        }
        case IRExprKind::Construct: {
            auto* construct = static_cast<IRConstructExpr*>(result.get());
            for (auto& arg : construct->arguments) arg = substituteExpression(arg.get(), substitutions);
            break;
        }
        case IRExprKind::Identifier:
        case IRExprKind::Literal:
        case IRExprKind::Lambda:
            break;
    }
    return result;
}

IRExprPtr IROptimizer::tryInlineCall(const IRCallExpr* call) {
    if (!call || call->callee->kind != IRExprKind::Identifier) return nullptr;
    const auto* ident = static_cast<const IRIdentifierExpr*>(call->callee.get());
    auto candidateIt = inlineCandidates.find(ident->name);
    if (candidateIt == inlineCandidates.end()) return nullptr;
    const InlineCandidate& candidate = candidateIt->second;
    if (!candidate.body || !candidate.function) return nullptr;
    if (candidate.function->parameters.size() != call->arguments.size()) return nullptr;
    if (candidate.function->name == ident->name && !candidate.function->parameters.empty() &&
        candidate.body->kind == IRExprKind::Call) {
        const auto* innerCall = static_cast<const IRCallExpr*>(candidate.body);
        if (innerCall->callee->kind == IRExprKind::Identifier &&
            static_cast<const IRIdentifierExpr*>(innerCall->callee.get())->name == candidate.function->name) {
            return nullptr;
        }
    }

    std::unordered_map<std::string, const IRExpression*> substitutions;
    for (size_t i = 0; i < call->arguments.size(); ++i) {
        if (!isSimpleExpression(call->arguments[i].get())) {
            return nullptr;
        }
        substitutions[candidate.function->parameters[i].first] = call->arguments[i].get();
    }
    return substituteExpression(candidate.body, substitutions);
}

IRExprPtr IROptimizer::foldExpression(IRExprPtr expression) {
    if (!expression) return nullptr;

    switch (expression->kind) {
        case IRExprKind::Unary: {
            auto* unary = static_cast<IRUnaryExpr*>(expression.get());
            if (unary->operand && unary->operand->kind == IRExprKind::Literal) {
                const auto* lit = static_cast<const IRLiteralExpr*>(unary->operand.get());
                if ((unary->op == "!" || unary->op == "not") && lit->type.kind == IRTypeKind::Bool) {
                    return std::make_unique<IRLiteralExpr>(
                        lit->value == "true" ? "false" : "true",
                        IRType(IRTypeKind::Bool, "Bool"),
                        unary->loc);
                }
                if (unary->op == "-" && lit->type.kind == IRTypeKind::Int) {
                    return std::make_unique<IRLiteralExpr>(
                        std::to_string(-std::stoll(lit->value)),
                        IRType(IRTypeKind::Int, "Int"),
                        unary->loc);
                }
                if (unary->op == "-" && lit->type.kind == IRTypeKind::Float) {
                    return std::make_unique<IRLiteralExpr>(
                        std::to_string(-std::stod(lit->value)),
                        IRType(IRTypeKind::Float, "Float"),
                        unary->loc);
                }
            }
            break;
        }
        case IRExprKind::Await: {
            auto* awaitExpr = static_cast<IRAwaitExpr*>(expression.get());
            if (awaitExpr->operand &&
                awaitExpr->operand->type.kind != IRTypeKind::Future &&
                awaitExpr->operand->type.kind != IRTypeKind::Any &&
                awaitExpr->operand->type.kind != IRTypeKind::Object) {
                return std::move(awaitExpr->operand);
            }
            break;
        }
        case IRExprKind::TypeCheck:
        case IRExprKind::SafeCast:
        case IRExprKind::CheckedCast:
            break;
        case IRExprKind::Binary: {
            auto* binary = static_cast<IRBinaryExpr*>(expression.get());
            if (binary->left && binary->right &&
                binary->left->kind == IRExprKind::Literal &&
                binary->right->kind == IRExprKind::Literal) {
                const auto* left = static_cast<const IRLiteralExpr*>(binary->left.get());
                const auto* right = static_cast<const IRLiteralExpr*>(binary->right.get());
                if (left->type.kind == IRTypeKind::Int && right->type.kind == IRTypeKind::Int) {
                    const int64_t lhs = std::stoll(left->value);
                    const int64_t rhs = std::stoll(right->value);
                    if (binary->op == "+") return std::make_unique<IRLiteralExpr>(std::to_string(lhs + rhs), IRType(IRTypeKind::Int, "Int"), binary->loc);
                    if (binary->op == "-") return std::make_unique<IRLiteralExpr>(std::to_string(lhs - rhs), IRType(IRTypeKind::Int, "Int"), binary->loc);
                    if (binary->op == "*") return std::make_unique<IRLiteralExpr>(std::to_string(lhs * rhs), IRType(IRTypeKind::Int, "Int"), binary->loc);
                    if (binary->op == "%" && rhs != 0) return std::make_unique<IRLiteralExpr>(std::to_string(lhs % rhs), IRType(IRTypeKind::Int, "Int"), binary->loc);
                    if (binary->op == "==") return std::make_unique<IRLiteralExpr>(lhs == rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                    if (binary->op == "!=") return std::make_unique<IRLiteralExpr>(lhs != rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                    if (binary->op == "<") return std::make_unique<IRLiteralExpr>(lhs < rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                    if (binary->op == "<=") return std::make_unique<IRLiteralExpr>(lhs <= rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                    if (binary->op == ">") return std::make_unique<IRLiteralExpr>(lhs > rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                    if (binary->op == ">=") return std::make_unique<IRLiteralExpr>(lhs >= rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                }
                if ((left->type.kind == IRTypeKind::Int || left->type.kind == IRTypeKind::Float) &&
                    (right->type.kind == IRTypeKind::Int || right->type.kind == IRTypeKind::Float)) {
                    const double lhs = left->type.kind == IRTypeKind::Int ? static_cast<double>(std::stoll(left->value)) : std::stod(left->value);
                    const double rhs = right->type.kind == IRTypeKind::Int ? static_cast<double>(std::stoll(right->value)) : std::stod(right->value);
                    if (binary->op == "/") return std::make_unique<IRLiteralExpr>(std::to_string(lhs / rhs), IRType(IRTypeKind::Float, "Float"), binary->loc);
                    if (binary->op == "**") return std::make_unique<IRLiteralExpr>(std::to_string(std::pow(lhs, rhs)), IRType(IRTypeKind::Float, "Float"), binary->loc);
                }
                if (left->type.kind == IRTypeKind::Bool && right->type.kind == IRTypeKind::Bool) {
                    const bool lhs = left->value == "true";
                    const bool rhs = right->value == "true";
                    if (binary->op == "and") return std::make_unique<IRLiteralExpr>(lhs && rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                    if (binary->op == "or") return std::make_unique<IRLiteralExpr>(lhs || rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                    if (binary->op == "==") return std::make_unique<IRLiteralExpr>(lhs == rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                    if (binary->op == "!=") return std::make_unique<IRLiteralExpr>(lhs != rhs ? "true" : "false", IRType(IRTypeKind::Bool, "Bool"), binary->loc);
                }
                if (binary->op == "+" &&
                    left->type.kind == IRTypeKind::String &&
                    right->type.kind == IRTypeKind::String) {
                    return std::make_unique<IRLiteralExpr>(left->value + right->value, IRType(IRTypeKind::String, "String"), binary->loc);
                }
            }

            bool truthy = false;
            if (binary->op == "and" && isConstantTruthy(binary->left.get(), truthy)) {
                return truthy ? std::move(binary->right) : std::move(binary->left);
            }
            if (binary->op == "or" && isConstantTruthy(binary->left.get(), truthy)) {
                return truthy ? std::move(binary->left) : std::move(binary->right);
            }
            break;
        }
        case IRExprKind::NullCoalesce: {
            auto* coalesce = static_cast<IRNullCoalesceExpr*>(expression.get());
            if (isLiteralNull(coalesce->left.get())) {
                return std::move(coalesce->right);
            }
            if (coalesce->left && coalesce->left->kind == IRExprKind::Literal &&
                coalesce->left->type.kind != IRTypeKind::Null) {
                return std::move(coalesce->left);
            }
            break;
        }
        default:
            break;
    }

    return expression;
}

bool IROptimizer::isPureExpression(const IRExpression* expression) const {
    if (!expression) return true;
    switch (expression->kind) {
        case IRExprKind::Identifier:
        case IRExprKind::Literal:
        case IRExprKind::Lambda:
            return true;
        case IRExprKind::Unary:
            return isPureExpression(static_cast<const IRUnaryExpr*>(expression)->operand.get());
        case IRExprKind::Await:
            return isPureExpression(static_cast<const IRAwaitExpr*>(expression)->operand.get());
        case IRExprKind::TypeCheck:
            return isPureExpression(static_cast<const IRTypeCheckExpr*>(expression)->operand.get());
        case IRExprKind::SafeCast:
            return isPureExpression(static_cast<const IRSafeCastExpr*>(expression)->operand.get());
        case IRExprKind::CheckedCast:
            return isPureExpression(static_cast<const IRCheckedCastExpr*>(expression)->operand.get());
        case IRExprKind::Binary: {
            const auto* binary = static_cast<const IRBinaryExpr*>(expression);
            return isPureExpression(binary->left.get()) && isPureExpression(binary->right.get());
        }
        case IRExprKind::List: {
            const auto* list = static_cast<const IRListExpr*>(expression);
            for (const auto& element : list->elements) {
                if (!isPureExpression(element.get())) return false;
            }
            return true;
        }
        case IRExprKind::NullCoalesce: {
            const auto* coalesce = static_cast<const IRNullCoalesceExpr*>(expression);
            return isPureExpression(coalesce->left.get()) && isPureExpression(coalesce->right.get());
        }
        case IRExprKind::Range: {
            const auto* range = static_cast<const IRRangeExpr*>(expression);
            return isPureExpression(range->start.get()) && isPureExpression(range->end.get());
        }
        case IRExprKind::Member: {
            const auto* member = static_cast<const IRMemberExpr*>(expression);
            return isPureExpression(member->object.get());
        }
        case IRExprKind::Index: {
            const auto* index = static_cast<const IRIndexExpr*>(expression);
            return isPureExpression(index->object.get()) && isPureExpression(index->index.get());
        }
        case IRExprKind::Construct: {
            const auto* construct = static_cast<const IRConstructExpr*>(expression);
            for (const auto& arg : construct->arguments) {
                if (!isPureExpression(arg.get())) return false;
            }
            return true;
        }
        case IRExprKind::Call:
        case IRExprKind::Assign:
            return false;
    }
    return false;
}

bool IROptimizer::isSimpleExpression(const IRExpression* expression) const {
    if (!expression) return false;
    if (expression->kind == IRExprKind::Identifier || expression->kind == IRExprKind::Literal) {
        return true;
    }
    if (expression->kind == IRExprKind::Unary) {
        return isSimpleExpression(static_cast<const IRUnaryExpr*>(expression)->operand.get());
    }
    if (expression->kind == IRExprKind::Await) {
        return isSimpleExpression(static_cast<const IRAwaitExpr*>(expression)->operand.get());
    }
    if (expression->kind == IRExprKind::TypeCheck) {
        return isSimpleExpression(static_cast<const IRTypeCheckExpr*>(expression)->operand.get());
    }
    if (expression->kind == IRExprKind::SafeCast) {
        return isSimpleExpression(static_cast<const IRSafeCastExpr*>(expression)->operand.get());
    }
    if (expression->kind == IRExprKind::CheckedCast) {
        return isSimpleExpression(static_cast<const IRCheckedCastExpr*>(expression)->operand.get());
    }
    return false;
}

bool IROptimizer::isConstantTruthy(const IRExpression* expression, bool& value) const {
    if (!expression) return false;
    if (isLiteralBool(expression, value)) return true;
    if (isLiteralNull(expression)) {
        value = false;
        return true;
    }
    if (expression->kind == IRExprKind::Literal) {
        const auto* lit = static_cast<const IRLiteralExpr*>(expression);
        if (lit->type.kind == IRTypeKind::Int) {
            value = std::stoll(lit->value) != 0;
            return true;
        }
        if (lit->type.kind == IRTypeKind::Float) {
            value = std::stod(lit->value) != 0.0;
            return true;
        }
        if (lit->type.kind == IRTypeKind::String) {
            value = !lit->value.empty();
            return true;
        }
    }
    return false;
}

const IRExpression* IROptimizer::lookupPropagation(const std::string& name) const {
    for (auto it = propagationScopes.rbegin(); it != propagationScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return nullptr;
}

void IROptimizer::bindPropagation(const std::string& name, const IRExpression* expression) {
    if (propagationScopes.empty()) pushPropagationScope();
    propagationScopes.back()[name] = expression;
}

void IROptimizer::killPropagation(const std::string& name) {
    for (auto& scope : propagationScopes) {
        scope.erase(name);
    }
}

void IROptimizer::pushPropagationScope() {
    propagationScopes.emplace_back();
}

void IROptimizer::popPropagationScope() {
    if (!propagationScopes.empty()) propagationScopes.pop_back();
}

}  // namespace smush
