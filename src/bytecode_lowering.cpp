#include "bytecode_lowering.h"

#include <algorithm>
#include <functional>

namespace smush {

namespace {

std::string locString(const SourceLocation& loc) {
    return loc.filename + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column);
}

bool isComparisonOp(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

OpCode comparisonOpcode(const std::string& op) {
    if (op == "==") return OpCode::Eq;
    if (op == "!=") return OpCode::Ne;
    if (op == "<") return OpCode::Lt;
    if (op == "<=") return OpCode::Le;
    if (op == ">") return OpCode::Gt;
    return OpCode::Ge;
}

int64_t encodeHandlerOperand(uint32_t catchPc, uint32_t catchSlot) {
    return (static_cast<int64_t>(catchPc) << 32) | static_cast<int64_t>(catchSlot);
}

}  // namespace

std::shared_ptr<BytecodeModule> BytecodeLowerer::lower(const IRProgram* program) {
    errors.clear();
    scopes.clear();
    loopStartStack.clear();
    loopBreakPatchStack.clear();
    loopContinuePatchStack.clear();
    activeHandlerDepth = 0;
    currentFunction = nullptr;
    functionBodies.clear();
    fieldDefaultBodies.clear();

    module = std::make_shared<BytecodeModule>();
    if (!program) {
        errors.push_back("internal error: null IR program");
        return module;
    }

    module->sourceFile = program->loc.filename;
    buildTypes(program);
    buildFunctions(program);
    return module;
}

const std::vector<std::string>& BytecodeLowerer::getErrors() const {
    return errors;
}

void BytecodeLowerer::buildTypes(const IRProgram* program) {
    for (const auto& iface : program->interfaces) {
        BytecodeInterfaceLayout layout;
        layout.name = iface.name;
        layout.baseInterfaces = iface.baseInterfaces;
        layout.loc = iface.loc;
        for (const auto& method : iface.methods) {
            BytecodeInterfaceMethod signature;
            signature.name = method.name;
            signature.returnType = method.returnType;
            signature.async = method.async;
            signature.staticMethod = method.staticMethod;
            signature.abstractMethod = method.abstractMethod;
            signature.visibility = method.visibility;
            for (const auto& param : method.parameters) {
                signature.parameterTypes.push_back(param.second);
            }
            layout.methods.push_back(std::move(signature));
        }
        module->interfaceMap[layout.name] = static_cast<uint32_t>(module->interfaces.size());
        module->interfaces.push_back(std::move(layout));
    }

    std::unordered_map<std::string, const IRClassType*> classesByName;
    for (const auto& klass : program->classes) {
        classesByName[klass.name] = &klass;
    }
    std::function<std::vector<const IRFieldDecl*>(const IRClassType&)> collectFields =
        [&](const IRClassType& klass) -> std::vector<const IRFieldDecl*> {
            std::vector<const IRFieldDecl*> fields;
            if (!klass.baseClass.empty()) {
                auto baseIt = classesByName.find(klass.baseClass);
                if (baseIt != classesByName.end()) {
                    fields = collectFields(*baseIt->second);
                }
            }
            for (const auto& field : klass.fields) {
                fields.push_back(&field);
            }
            return fields;
        };

    for (const auto& data : program->dataTypes) {
        BytecodeTypeLayout layout;
        layout.name = data.name;
        layout.classType = false;
        layout.loc = data.loc;
        for (size_t i = 0; i < data.fields.size(); ++i) {
            layout.fields.push_back(BytecodeFieldLayout{
                data.fields[i].name,
                data.fields[i].type,
                static_cast<uint32_t>(i),
                UINT32_MAX});
        }
        module->typeMap[layout.name] = static_cast<uint32_t>(module->types.size());
        module->types.push_back(std::move(layout));
    }

    for (const auto& klass : program->classes) {
        BytecodeTypeLayout layout;
        layout.name = klass.name;
        layout.baseType = klass.baseClass;
        layout.classType = true;
        layout.abstractClass = klass.abstractClass;
        layout.interfaces = klass.implementedInterfaces;
        layout.loc = klass.loc;
        const auto flattenedFields = collectFields(klass);
        for (size_t i = 0; i < flattenedFields.size(); ++i) {
            layout.fields.push_back(BytecodeFieldLayout{
                flattenedFields[i]->name,
                flattenedFields[i]->type,
                static_cast<uint32_t>(i),
                UINT32_MAX});
        }
        module->typeMap[layout.name] = static_cast<uint32_t>(module->types.size());
        module->types.push_back(std::move(layout));
    }
}

void BytecodeLowerer::buildFunctions(const IRProgram* program) {
    for (const auto& global : program->globals) {
        if (global->kind == IRStmtKind::VarDecl) {
            module->globals.push_back(static_cast<const IRVarDeclStmt*>(global.get())->name);
        }
    }

    if (!program->globals.empty()) {
        BytecodeFunction init;
        init.name = "__module_init";
        init.qualifiedName = "__module_init";
        init.returnType = IRType(IRTypeKind::Void, "Void");
        init.method = false;
        init.loc = program->loc;
        module->functionMap[init.qualifiedName] = static_cast<uint32_t>(module->functions.size());
        module->functions.push_back(std::move(init));
    }

    for (const auto& data : program->dataTypes) {
        uint32_t typeIndex = module->typeMap[data.name];
        for (size_t i = 0; i < data.fields.size(); ++i) {
            if (!data.fields[i].defaultValue) {
                continue;
            }
            BytecodeFunction bc;
            bc.name = "__field_default__" + data.fields[i].name;
            bc.qualifiedName = data.name + "::" + bc.name;
            bc.returnType = data.fields[i].type;
            bc.method = true;
            bc.ownerType = data.name;
            bc.loc = data.fields[i].loc;
            bc.parameters.push_back(LocalInfo{"self", IRType(IRTypeKind::Object, data.name), false, 0});
            uint32_t functionIndex = static_cast<uint32_t>(module->functions.size());
            module->functionMap[bc.qualifiedName] = functionIndex;
            module->functions.push_back(std::move(bc));
            module->types[typeIndex].fields[i].defaultFunctionIndex = functionIndex;
            fieldDefaultBodies[functionIndex] = FieldDefaultBody{data.fields[i].defaultValue.get(), data.fields[i].type};
        }
    }

    for (const auto& function : program->functions) {
        BytecodeFunction bc;
        bc.name = function.name;
        bc.qualifiedName = function.name;
        bc.returnType = function.returnType;
        bc.async = function.async;
        bc.method = false;
        bc.loc = function.loc;
        for (size_t i = 0; i < function.captures.size(); ++i) {
            bc.captures.push_back(LocalInfo{
                function.captures[i].first,
                function.captures[i].second,
                false,
                static_cast<uint32_t>(i)});
        }
        for (size_t i = 0; i < function.parameters.size(); ++i) {
            bc.parameters.push_back(LocalInfo{
                function.parameters[i].first,
                function.parameters[i].second,
                true,
                static_cast<uint32_t>(bc.captures.size() + i)});
        }
        module->functionMap[bc.qualifiedName] = static_cast<uint32_t>(module->functions.size());
        functionBodies[static_cast<uint32_t>(module->functions.size())] = &function;
        module->functions.push_back(std::move(bc));
    }

    for (size_t typeIndex = 0; typeIndex < program->classes.size(); ++typeIndex) {
        const auto& klass = program->classes[typeIndex];
        uint32_t layoutIndex = module->typeMap[klass.name];
        const auto& typeLayout = module->types[layoutIndex];
        std::unordered_map<std::string, size_t> fieldSlots;
        for (size_t i = 0; i < typeLayout.fields.size(); ++i) {
            fieldSlots[typeLayout.fields[i].name] = i;
        }
        for (const auto& field : klass.fields) {
            if (!field.defaultValue) {
                continue;
            }
            BytecodeFunction bc;
            bc.name = "__field_default__" + field.name;
            bc.qualifiedName = klass.name + "::" + bc.name;
            bc.returnType = field.type;
            bc.method = true;
            bc.ownerType = klass.name;
            bc.loc = field.loc;
            bc.parameters.push_back(LocalInfo{"self", IRType(IRTypeKind::Object, klass.name), false, 0});
            uint32_t functionIndex = static_cast<uint32_t>(module->functions.size());
            module->functionMap[bc.qualifiedName] = functionIndex;
            module->functions.push_back(std::move(bc));
            module->types[layoutIndex].fields[fieldSlots[field.name]].defaultFunctionIndex = functionIndex;
            fieldDefaultBodies[functionIndex] = FieldDefaultBody{field.defaultValue.get(), field.type};
        }
        std::unordered_map<std::string, uint32_t> methodOrdinals;
        for (const auto& method : klass.methods) {
            const uint32_t ordinal = methodOrdinals[method.name]++;
            BytecodeFunction bc;
            bc.name = method.name;
            bc.qualifiedName = klass.name + "::" + method.name + "#" + std::to_string(ordinal);
            bc.returnType = method.returnType;
            bc.async = method.async;
            bc.method = !method.staticMethod;
            bc.staticMethod = method.staticMethod;
            bc.constructor = method.constructor;
            bc.abstractMethod = method.abstractMethod;
            bc.visibility = method.visibility;
            bc.ownerType = klass.name;
            bc.loc = method.loc;
            uint32_t parameterOffset = 0;
            if (!method.staticMethod) {
                bc.parameters.push_back(LocalInfo{"self", IRType(IRTypeKind::Object, klass.name), false, 0});
                parameterOffset = 1;
            }
            for (size_t i = 0; i < method.parameters.size(); ++i) {
                bc.parameters.push_back(LocalInfo{
                    method.parameters[i].first,
                    method.parameters[i].second,
                    true,
                    static_cast<uint32_t>(i + parameterOffset)});
            }
            uint32_t functionIndex = static_cast<uint32_t>(module->functions.size());
            module->functionMap[bc.qualifiedName] = functionIndex;
            if (!method.abstractMethod) {
                if (method.constructor) {
                    module->types[layoutIndex].constructorIndices.push_back(functionIndex);
                } else if (method.staticMethod) {
                    module->types[layoutIndex].staticMethodIndices.push_back(functionIndex);
                } else {
                    module->types[layoutIndex].methodIndices.push_back(functionIndex);
                }
            }
            if (method.body) {
                functionBodies[functionIndex] = &method;
            }
            module->functions.push_back(std::move(bc));
        }
    }

    for (const auto& type : program->classes) {
        uint32_t layoutIndex = module->typeMap[type.name];
        auto currentBase = type.baseClass;
        while (!currentBase.empty()) {
            auto baseLayoutIt = module->typeMap.find(currentBase);
            if (baseLayoutIt == module->typeMap.end()) {
                break;
            }
            const auto& baseLayout = module->types[baseLayoutIt->second];
            for (const auto& baseField : baseLayout.fields) {
                for (auto& derivedField : module->types[layoutIndex].fields) {
                    if (derivedField.name == baseField.name && derivedField.defaultFunctionIndex == UINT32_MAX) {
                        derivedField.defaultFunctionIndex = baseField.defaultFunctionIndex;
                    }
                }
            }
            currentBase = baseLayout.baseType;
        }
    }

    for (uint32_t i = 0; i < module->functions.size(); ++i) {
        const BytecodeFunction& signature = module->functions[i];
        if (signature.name == "__module_init") {
            currentFunction = &module->functions[i];
            currentFunction->instructions.clear();
            currentFunction->locals.clear();
            currentFunction->maxStack = 0;
            scopes.clear();
            pushScope();
            for (const auto& global : program->globals) {
                lowerStatement(global.get());
            }
            emit(OpCode::LoadConst, addConstant(ConstantValue::null()), program->loc);
            emit(OpCode::Return, program->loc);
            popScope();
            currentFunction = nullptr;
            continue;
        }
        if (!signature.method) {
            auto bodyIt = functionBodies.find(i);
            if (bodyIt != functionBodies.end()) {
                lowerFunctionBody(*bodyIt->second, i);
                if (signature.name == "main") {
                    module->entry = BytecodeEntryPoint{i, "main"};
                }
            }
            continue;
        }
        auto fieldDefaultIt = fieldDefaultBodies.find(i);
        if (fieldDefaultIt != fieldDefaultBodies.end()) {
            lowerFieldDefaultBody(i, fieldDefaultIt->second, signature.loc);
            continue;
        }
        auto bodyIt = functionBodies.find(i);
        if (bodyIt != functionBodies.end()) {
            lowerFunctionBody(*bodyIt->second, i);
        }
    }
}

void BytecodeLowerer::lowerFunctionBody(const IRFunction& function, uint32_t functionIndex) {
    currentFunction = &module->functions[functionIndex];
    currentFunction->instructions.clear();
    currentFunction->locals.clear();
    currentFunction->maxStack = 0;
    scopes.clear();
    pushScope();

    for (const auto& capture : currentFunction->captures) {
        scopes.back().locals[capture.name] = capture.slot;
        currentFunction->locals.push_back(capture);
    }
    for (const auto& parameter : currentFunction->parameters) {
        scopes.back().locals[parameter.name] = parameter.slot;
        currentFunction->locals.push_back(parameter);
    }

    lowerStatement(function.body.get());
    emit(OpCode::Return, function.loc);
    popScope();
    currentFunction = nullptr;
}

void BytecodeLowerer::lowerFieldDefaultBody(uint32_t functionIndex, const FieldDefaultBody& body, const SourceLocation& loc) {
    currentFunction = &module->functions[functionIndex];
    currentFunction->instructions.clear();
    currentFunction->locals.clear();
    currentFunction->maxStack = 0;
    scopes.clear();
    pushScope();

    for (const auto& parameter : currentFunction->parameters) {
        scopes.back().locals[parameter.name] = parameter.slot;
        currentFunction->locals.push_back(parameter);
    }

    if (body.expression) {
        lowerExpression(body.expression);
    } else {
        emit(OpCode::LoadConst, addConstant(ConstantValue::null()), loc);
    }
    emit(OpCode::Return, loc);
    popScope();
    currentFunction = nullptr;
}

void BytecodeLowerer::pushScope() {
    scopes.emplace_back();
}

void BytecodeLowerer::popScope() {
    if (!scopes.empty()) scopes.pop_back();
}

uint32_t BytecodeLowerer::defineLocal(const std::string& name, const IRType& type, bool mutableBinding) {
    if (scopes.empty()) pushScope();
    uint32_t slot = static_cast<uint32_t>(currentFunction->locals.size());
    scopes.back().locals[name] = slot;
    currentFunction->locals.push_back(LocalInfo{name, type, mutableBinding, slot});
    return slot;
}

uint32_t BytecodeLowerer::resolveLocal(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->locals.find(name);
        if (found != it->locals.end()) return found->second;
    }
    return UINT32_MAX;
}

uint32_t BytecodeLowerer::resolveCapture(const std::string& name) const {
    if (!currentFunction) {
        return UINT32_MAX;
    }
    for (const auto& capture : currentFunction->captures) {
        if (capture.name == name) {
            return capture.slot;
        }
    }
    return UINT32_MAX;
}

uint32_t BytecodeLowerer::resolveGlobal(const std::string& name) const {
    auto it = std::find(module->globals.begin(), module->globals.end(), name);
    if (it == module->globals.end()) return UINT32_MAX;
    return static_cast<uint32_t>(std::distance(module->globals.begin(), it));
}

bool BytecodeLowerer::currentMethodHasField(const std::string& name) const {
    if (!currentFunction || !currentFunction->method || currentFunction->staticMethod) {
        return false;
    }
    auto typeIt = module->typeMap.find(currentFunction->ownerType);
    if (typeIt == module->typeMap.end()) {
        return false;
    }
    const auto& type = module->types[typeIt->second];
    return std::any_of(type.fields.begin(), type.fields.end(),
        [&](const BytecodeFieldLayout& field) { return field.name == name; });
}

bool BytecodeLowerer::currentMethodHasInstanceMethod(const std::string& name) const {
    if (!currentFunction || !currentFunction->method || currentFunction->staticMethod) {
        return false;
    }
    auto typeIt = module->typeMap.find(currentFunction->ownerType);
    if (typeIt == module->typeMap.end()) {
        return false;
    }
    uint32_t currentTypeIndex = typeIt->second;
    while (true) {
        const auto& type = module->types[currentTypeIndex];
        const bool found = std::any_of(type.methodIndices.begin(), type.methodIndices.end(),
            [&](uint32_t methodIndex) {
                const auto& function = module->functions[methodIndex];
                return function.name == name;
            });
        if (found) {
            return true;
        }
        if (type.baseType.empty()) {
            break;
        }
        auto baseIt = module->typeMap.find(type.baseType);
        if (baseIt == module->typeMap.end()) {
            break;
        }
        currentTypeIndex = baseIt->second;
    }
    return false;
}

uint32_t BytecodeLowerer::addConstant(const ConstantValue& constant) {
    module->constants.push_back(constant);
    return static_cast<uint32_t>(module->constants.size() - 1);
}

uint32_t BytecodeLowerer::addTypeConstant(const IRType& type) {
    module->typeConstants.push_back(type);
    return static_cast<uint32_t>(module->typeConstants.size() - 1);
}

uint32_t BytecodeLowerer::addStringConstant(const std::string& value) {
    return addConstant(ConstantValue(value));
}

void BytecodeLowerer::emit(OpCode opcode, const SourceLocation& loc) {
    currentFunction->instructions.push_back(Instruction(opcode, loc));
}

void BytecodeLowerer::emit(OpCode opcode, int64_t operand, const SourceLocation& loc) {
    currentFunction->instructions.push_back(Instruction(opcode, operand, loc));
}

size_t BytecodeLowerer::emitJump(OpCode opcode, const SourceLocation& loc) {
    currentFunction->instructions.push_back(Instruction(opcode, 0, loc));
    return currentFunction->instructions.size() - 1;
}

void BytecodeLowerer::patchJump(size_t instructionIndex, uint32_t target) {
    currentFunction->instructions[instructionIndex].operand = static_cast<int64_t>(target);
}

uint32_t BytecodeLowerer::currentOffset() const {
    return static_cast<uint32_t>(currentFunction->instructions.size());
}

void BytecodeLowerer::lowerStatement(const IRStatement* statement) {
    if (!statement) return;
    switch (statement->kind) {
        case IRStmtKind::Block: {
            pushScope();
            const auto* block = static_cast<const IRBlockStmt*>(statement);
            for (const auto& stmt : block->statements) lowerStatement(stmt.get());
            popScope();
            return;
        }
        case IRStmtKind::VarDecl: {
            const auto* var = static_cast<const IRVarDeclStmt*>(statement);
            if (var->initializer) {
                lowerExpression(var->initializer.get());
            } else {
                emit(OpCode::LoadConst, addConstant(ConstantValue::null()), var->loc);
            }
            if (currentFunction && currentFunction->name == "__module_init" && scopes.size() == 1) {
                uint32_t global = resolveGlobal(var->name);
                if (global == UINT32_MAX) {
                    error(var->loc, "unknown global binding '" + var->name + "'");
                    emit(OpCode::Pop, var->loc);
                    return;
                }
                emit(OpCode::StoreGlobal, global, var->loc);
                return;
            }
            uint32_t slot = defineLocal(var->name, var->declaredType, var->mutableBinding);
            emit(OpCode::StoreLocal, slot, var->loc);
            return;
        }
        case IRStmtKind::Expr:
            lowerExpression(static_cast<const IRExprStmt*>(statement)->expression.get());
            emit(OpCode::Pop, statement->loc);
            return;
        case IRStmtKind::If: {
            const auto* ifStmt = static_cast<const IRIfStmt*>(statement);
            lowerCondition(ifStmt->condition.get());
            size_t elseJump = emitJump(OpCode::JumpIfFalse, ifStmt->loc);
            lowerStatement(ifStmt->thenBranch.get());
            size_t exitJump = emitJump(OpCode::Jump, ifStmt->loc);
            patchJump(elseJump, currentOffset());
            if (ifStmt->elseBranch) lowerStatement(ifStmt->elseBranch.get());
            patchJump(exitJump, currentOffset());
            return;
        }
        case IRStmtKind::While: {
            const auto* whileStmt = static_cast<const IRWhileStmt*>(statement);
            uint32_t loopStart = currentOffset();
            loopStartStack.push_back(loopStart);
            loopBreakPatchStack.emplace_back();
            loopContinuePatchStack.emplace_back();
            lowerCondition(whileStmt->condition.get());
            size_t exitJump = emitJump(OpCode::JumpIfFalse, whileStmt->loc);
            lowerStatement(whileStmt->body.get());
            emit(OpCode::Jump, loopStart, whileStmt->loc);
            patchJump(exitJump, currentOffset());
            for (size_t patch : loopBreakPatchStack.back()) patchJump(patch, currentOffset());
            for (size_t patch : loopContinuePatchStack.back()) patchJump(patch, loopStart);
            loopStartStack.pop_back();
            loopBreakPatchStack.pop_back();
            loopContinuePatchStack.pop_back();
            return;
        }
        case IRStmtKind::For: {
            const auto* forStmt = static_cast<const IRForStmt*>(statement);
            lowerExpression(forStmt->iterable.get());
            uint32_t iterableSlot = defineLocal("$iterable_" + forStmt->name, IRType(IRTypeKind::Any, "Any"), false);
            emit(OpCode::StoreLocal, iterableSlot, forStmt->loc);
            emit(OpCode::LoadConst, addConstant(ConstantValue(int64_t{0})), forStmt->loc);
            uint32_t indexSlot = defineLocal("$index_" + forStmt->name, IRType(IRTypeKind::Int, "Int"), true);
            emit(OpCode::StoreLocal, indexSlot, forStmt->loc);
            uint32_t valueSlot = defineLocal(forStmt->name, IRType(IRTypeKind::Any, "Any"), true);

            uint32_t loopStart = currentOffset();
            loopStartStack.push_back(loopStart);
            loopBreakPatchStack.emplace_back();
            loopContinuePatchStack.emplace_back();

            emit(OpCode::LoadLocal, indexSlot, forStmt->loc);
            emit(OpCode::LoadLocal, iterableSlot, forStmt->loc);
            emit(OpCode::GetMember, addStringConstant("length"), forStmt->loc);
            emit(OpCode::Lt, forStmt->loc);
            size_t exitJump = emitJump(OpCode::JumpIfFalse, forStmt->loc);

            emit(OpCode::LoadLocal, iterableSlot, forStmt->loc);
            emit(OpCode::LoadLocal, indexSlot, forStmt->loc);
            emit(OpCode::GetIndex, forStmt->loc);
            emit(OpCode::StoreLocal, valueSlot, forStmt->loc);

            lowerStatement(forStmt->body.get());

            uint32_t continueTarget = currentOffset();
            emit(OpCode::LoadLocal, indexSlot, forStmt->loc);
            emit(OpCode::LoadConst, addConstant(ConstantValue(int64_t{1})), forStmt->loc);
            emit(OpCode::Add, forStmt->loc);
            emit(OpCode::StoreLocal, indexSlot, forStmt->loc);
            emit(OpCode::Jump, loopStart, forStmt->loc);

            patchJump(exitJump, currentOffset());
            for (size_t patch : loopBreakPatchStack.back()) patchJump(patch, currentOffset());
            for (size_t patch : loopContinuePatchStack.back()) patchJump(patch, continueTarget);
            loopStartStack.pop_back();
            loopBreakPatchStack.pop_back();
            loopContinuePatchStack.pop_back();
            return;
        }
        case IRStmtKind::Try: {
            const auto* tryStmt = static_cast<const IRTryStmt*>(statement);
            const size_t handlerInstruction = currentFunction->instructions.size();
            emit(OpCode::PushHandler, int64_t{0}, tryStmt->loc);
            ++activeHandlerDepth;
            lowerStatement(tryStmt->tryBranch.get());
            --activeHandlerDepth;
            emit(OpCode::PopHandler, tryStmt->loc);
            size_t exitJump = emitJump(OpCode::Jump, tryStmt->loc);

            pushScope();
            uint32_t catchSlot = defineLocal(tryStmt->catchName, IRType(IRTypeKind::Any, "Any"), true);
            const uint32_t catchPc = currentOffset();
            currentFunction->instructions[handlerInstruction].operand = encodeHandlerOperand(catchPc, catchSlot);
            lowerStatement(tryStmt->catchBranch.get());
            popScope();

            patchJump(exitJump, currentOffset());
            return;
        }
        case IRStmtKind::Throw: {
            const auto* throwStmt = static_cast<const IRThrowStmt*>(statement);
            if (throwStmt->value) {
                lowerExpression(throwStmt->value.get());
            } else {
                emit(OpCode::LoadConst, addConstant(ConstantValue::null()), throwStmt->loc);
            }
            emit(OpCode::Throw, throwStmt->loc);
            return;
        }
        case IRStmtKind::Return: {
            const auto* ret = static_cast<const IRReturnStmt*>(statement);
            if (ret->value) {
                lowerExpression(ret->value.get());
            } else {
                emit(OpCode::LoadConst, addConstant(ConstantValue::null()), ret->loc);
            }
            for (size_t i = 0; i < activeHandlerDepth; ++i) {
                emit(OpCode::PopHandler, ret->loc);
            }
            emit(OpCode::Return, ret->loc);
            return;
        }
        case IRStmtKind::Break:
            if (loopBreakPatchStack.empty()) {
                error(statement->loc, "break outside loop in bytecode lowering");
                return;
            }
            for (size_t i = 0; i < activeHandlerDepth; ++i) {
                emit(OpCode::PopHandler, statement->loc);
            }
            loopBreakPatchStack.back().push_back(emitJump(OpCode::Jump, statement->loc));
            return;
        case IRStmtKind::Continue:
            if (loopContinuePatchStack.empty()) {
                error(statement->loc, "continue outside loop in bytecode lowering");
                return;
            }
            for (size_t i = 0; i < activeHandlerDepth; ++i) {
                emit(OpCode::PopHandler, statement->loc);
            }
            loopContinuePatchStack.back().push_back(emitJump(OpCode::Jump, statement->loc));
            return;
    }
}

void BytecodeLowerer::lowerExpression(const IRExpression* expression) {
    if (!expression) {
        emit(OpCode::LoadConst, addConstant(ConstantValue::null()));
        return;
    }
    switch (expression->kind) {
        case IRExprKind::Identifier: {
            const auto* ident = static_cast<const IRIdentifierExpr*>(expression);
            if (ident->name == "super" && currentFunction && currentFunction->method && !currentFunction->staticMethod) {
                emit(OpCode::LoadLocal, 0, ident->loc);
                return;
            }
            uint32_t local = resolveLocal(ident->name);
            if (local != UINT32_MAX) {
                emit(OpCode::LoadLocal, local, ident->loc);
                return;
            }
            uint32_t capture = resolveCapture(ident->name);
            if (capture != UINT32_MAX) {
                emit(OpCode::LoadLocal, capture, ident->loc);
                return;
            }
            uint32_t global = resolveGlobal(ident->name);
            if (global != UINT32_MAX) {
                emit(OpCode::LoadGlobal, global, ident->loc);
                return;
            }
            if (currentMethodHasField(ident->name)) {
                emit(OpCode::LoadLocal, 0, ident->loc);
                emit(OpCode::GetMember, addStringConstant(ident->name), ident->loc);
                return;
            }
            if (currentMethodHasInstanceMethod(ident->name)) {
                emit(OpCode::LoadLocal, 0, ident->loc);
                emit(OpCode::GetMember, addStringConstant(ident->name), ident->loc);
                return;
            }
            auto fnIt = module->functionMap.find(ident->name);
            if (fnIt != module->functionMap.end()) {
                emit(OpCode::LoadConst, addStringConstant(ident->name), ident->loc);
                return;
            }
            emit(OpCode::LoadConst, addStringConstant(ident->name), ident->loc);
            return;
        }
        case IRExprKind::Literal: {
            const auto* lit = static_cast<const IRLiteralExpr*>(expression);
            if (expression->type.kind == IRTypeKind::Bool) {
                emit(OpCode::LoadConst, addConstant(ConstantValue(lit->value == "true")), lit->loc);
            } else if (expression->type.kind == IRTypeKind::Int) {
                emit(OpCode::LoadConst, addConstant(ConstantValue(std::stoll(lit->value))), lit->loc);
            } else if (expression->type.kind == IRTypeKind::Float) {
                emit(OpCode::LoadConst, addConstant(ConstantValue(std::stod(lit->value))), lit->loc);
            } else if (expression->type.kind == IRTypeKind::Null) {
                emit(OpCode::LoadConst, addConstant(ConstantValue::null()), lit->loc);
            } else {
                emit(OpCode::LoadConst, addStringConstant(lit->value), lit->loc);
            }
            return;
        }
        case IRExprKind::Lambda: {
            const auto* lambda = static_cast<const IRLambdaExpr*>(expression);
            auto fnIt = module->functionMap.find(lambda->functionName);
            if (fnIt == module->functionMap.end()) {
                error(lambda->loc, "unknown lowered lambda function '" + lambda->functionName + "'");
                emit(OpCode::LoadConst, addConstant(ConstantValue::null()), lambda->loc);
                return;
            }
            for (const auto& captureName : lambda->captures) {
                uint32_t local = resolveLocal(captureName);
                if (local != UINT32_MAX) {
                    emit(OpCode::LoadLocal, local, lambda->loc);
                    continue;
                }
                uint32_t capture = resolveCapture(captureName);
                if (capture != UINT32_MAX) {
                    emit(OpCode::LoadLocal, capture, lambda->loc);
                    continue;
                }
                uint32_t global = resolveGlobal(captureName);
                if (global != UINT32_MAX) {
                    emit(OpCode::LoadGlobal, global, lambda->loc);
                    continue;
                }
                error(lambda->loc, "unknown lambda capture '" + captureName + "'");
                emit(OpCode::LoadConst, addConstant(ConstantValue::null()), lambda->loc);
            }
            emit(OpCode::CreateClosure, fnIt->second, lambda->loc);
            return;
        }
        case IRExprKind::Unary: {
            const auto* unary = static_cast<const IRUnaryExpr*>(expression);
            lowerExpression(unary->operand.get());
            if (unary->op == "-" ) emit(OpCode::Neg, unary->loc);
            else emit(OpCode::Not, unary->loc);
            return;
        }
        case IRExprKind::Await: {
            const auto* awaitExpr = static_cast<const IRAwaitExpr*>(expression);
            lowerExpression(awaitExpr->operand.get());
            emit(OpCode::Await, awaitExpr->loc);
            return;
        }
        case IRExprKind::TypeCheck: {
            const auto* typeCheck = static_cast<const IRTypeCheckExpr*>(expression);
            lowerExpression(typeCheck->operand.get());
            emit(OpCode::TypeCheck, addTypeConstant(typeCheck->targetType), typeCheck->loc);
            return;
        }
        case IRExprKind::SafeCast: {
            const auto* castExpr = static_cast<const IRSafeCastExpr*>(expression);
            lowerExpression(castExpr->operand.get());
            emit(OpCode::SafeCast, addTypeConstant(castExpr->targetType), castExpr->loc);
            return;
        }
        case IRExprKind::CheckedCast: {
            const auto* castExpr = static_cast<const IRCheckedCastExpr*>(expression);
            lowerExpression(castExpr->operand.get());
            emit(OpCode::CheckedCast, addTypeConstant(castExpr->targetType), castExpr->loc);
            return;
        }
        case IRExprKind::Binary: {
            const auto* binary = static_cast<const IRBinaryExpr*>(expression);
            lowerExpression(binary->left.get());
            lowerExpression(binary->right.get());
            if (binary->op == "+") emit(OpCode::Add, binary->loc);
            else if (binary->op == "-") emit(OpCode::Sub, binary->loc);
            else if (binary->op == "*") emit(OpCode::Mul, binary->loc);
            else if (binary->op == "/") emit(OpCode::Div, binary->loc);
            else if (binary->op == "%") emit(OpCode::Mod, binary->loc);
            else if (binary->op == "**") emit(OpCode::Pow, binary->loc);
            else if (isComparisonOp(binary->op)) emit(comparisonOpcode(binary->op), binary->loc);
            else if (binary->op == "and") emit(OpCode::And, binary->loc);
            else if (binary->op == "or") emit(OpCode::Or, binary->loc);
            else error(binary->loc, "unsupported binary operator in bytecode lowering: " + binary->op);
            return;
        }
        case IRExprKind::Call: {
            const auto* call = static_cast<const IRCallExpr*>(expression);
            lowerExpression(call->callee.get());
            for (const auto& arg : call->arguments) lowerExpression(arg.get());
            emit(OpCode::Call, static_cast<int64_t>(call->arguments.size()), call->loc);
            return;
        }
        case IRExprKind::Member: {
            const auto* member = static_cast<const IRMemberExpr*>(expression);
            if (member->object &&
                member->object->kind == IRExprKind::Identifier &&
                static_cast<const IRIdentifierExpr*>(member->object.get())->name == "super") {
                emit(OpCode::LoadLocal, 0, member->loc);
                emit(OpCode::GetSuperMember, addStringConstant(member->member), member->loc);
                return;
            }
            lowerExpression(member->object.get());
            emit(member->safe ? OpCode::GetMemberSafe : OpCode::GetMember, addStringConstant(member->member), member->loc);
            return;
        }
        case IRExprKind::Index: {
            const auto* index = static_cast<const IRIndexExpr*>(expression);
            lowerExpression(index->object.get());
            lowerExpression(index->index.get());
            emit(OpCode::GetIndex, index->loc);
            return;
        }
        case IRExprKind::List: {
            const auto* list = static_cast<const IRListExpr*>(expression);
            for (const auto& element : list->elements) lowerExpression(element.get());
            emit(OpCode::MakeList, static_cast<int64_t>(list->elements.size()), list->loc);
            return;
        }
        case IRExprKind::Assign: {
            const auto* assign = static_cast<const IRAssignExpr*>(expression);
            lowerExpression(assign->value.get());
            if (assign->target->kind == IRExprKind::Identifier) {
                const auto* ident = static_cast<const IRIdentifierExpr*>(assign->target.get());
                uint32_t local = resolveLocal(ident->name);
                if (local != UINT32_MAX) {
                    emit(OpCode::Dup, assign->loc);
                    emit(OpCode::StoreLocal, local, ident->loc);
                } else {
                    uint32_t capture = resolveCapture(ident->name);
                    if (capture != UINT32_MAX) {
                        emit(OpCode::Dup, assign->loc);
                        emit(OpCode::StoreLocal, capture, ident->loc);
                        return;
                    }
                    uint32_t global = resolveGlobal(ident->name);
                    if (global != UINT32_MAX) {
                        emit(OpCode::Dup, assign->loc);
                        emit(OpCode::StoreGlobal, global, ident->loc);
                    }
                    else if (currentMethodHasField(ident->name)) {
                        emit(OpCode::LoadLocal, 0, ident->loc);
                        emit(OpCode::SetMember, addStringConstant(ident->name), ident->loc);
                    } else {
                        error(ident->loc, "unknown assignment target '" + ident->name + "'");
                    }
                }
                return;
            }
            if (assign->target->kind == IRExprKind::Member) {
                const auto* member = static_cast<const IRMemberExpr*>(assign->target.get());
                if (member->object &&
                    member->object->kind == IRExprKind::Identifier &&
                    static_cast<const IRIdentifierExpr*>(member->object.get())->name == "super") {
                    emit(OpCode::LoadLocal, 0, member->loc);
                } else {
                    lowerExpression(member->object.get());
                }
                emit(OpCode::SetMember, addStringConstant(member->member), member->loc);
                return;
            }
            if (assign->target->kind == IRExprKind::Index) {
                const auto* index = static_cast<const IRIndexExpr*>(assign->target.get());
                lowerExpression(index->object.get());
                lowerExpression(index->index.get());
                emit(OpCode::SetIndex, index->loc);
                return;
            }
            error(assign->loc, "unsupported assignment target in bytecode lowering");
            return;
        }
        case IRExprKind::NullCoalesce: {
            const auto* coalesce = static_cast<const IRNullCoalesceExpr*>(expression);
            lowerExpression(coalesce->left.get());
            lowerExpression(coalesce->right.get());
            emit(OpCode::Coalesce, coalesce->loc);
            return;
        }
        case IRExprKind::Range: {
            const auto* range = static_cast<const IRRangeExpr*>(expression);
            lowerExpression(range->start.get());
            lowerExpression(range->end.get());
            emit(OpCode::MakeRange, range->inclusive ? 1 : 0, range->loc);
            return;
        }
        case IRExprKind::Construct: {
            const auto* construct = static_cast<const IRConstructExpr*>(expression);
            for (const auto& arg : construct->arguments) lowerExpression(arg.get());
            auto typeIt = module->typeMap.find(construct->typeName);
            if (typeIt == module->typeMap.end()) {
                error(construct->loc, "unknown constructed type '" + construct->typeName + "'");
                return;
            }
            const auto& typeLayout = module->types[typeIt->second];
            const int64_t operand =
                (static_cast<int64_t>(typeIt->second) << 32) |
                static_cast<int64_t>(construct->arguments.size());
            emit(typeLayout.classType ? OpCode::ConstructClass : OpCode::ConstructData, operand, construct->loc);
            return;
        }
    }
}

void BytecodeLowerer::lowerCondition(const IRExpression* expression) {
    lowerExpression(expression);
}

void BytecodeLowerer::error(const SourceLocation& loc, const std::string& message) {
    errors.push_back(locString(loc) + ": " + message);
}

}  // namespace smush
