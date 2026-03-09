#include "bytecode.h"

#include <sstream>
#include <stdexcept>
#include <vector>

namespace smush {

namespace {

const std::vector<InstructionSpec> kInstructionSpecs = {
    {OpCode::Nop, "nop", OperandKind::None},
    {OpCode::LoadConst, "load_const", OperandKind::U32},
    {OpCode::LoadLocal, "load_local", OperandKind::U32},
    {OpCode::StoreLocal, "store_local", OperandKind::U32},
    {OpCode::LoadGlobal, "load_global", OperandKind::U32},
    {OpCode::StoreGlobal, "store_global", OperandKind::U32},
    {OpCode::Pop, "pop", OperandKind::None},
    {OpCode::Dup, "dup", OperandKind::None},
    {OpCode::Add, "add", OperandKind::None},
    {OpCode::Sub, "sub", OperandKind::None},
    {OpCode::Mul, "mul", OperandKind::None},
    {OpCode::Div, "div", OperandKind::None},
    {OpCode::Mod, "mod", OperandKind::None},
    {OpCode::Pow, "pow", OperandKind::None},
    {OpCode::Neg, "neg", OperandKind::None},
    {OpCode::Not, "not", OperandKind::None},
    {OpCode::And, "and", OperandKind::None},
    {OpCode::Or, "or", OperandKind::None},
    {OpCode::Eq, "eq", OperandKind::None},
    {OpCode::Ne, "ne", OperandKind::None},
    {OpCode::Lt, "lt", OperandKind::None},
    {OpCode::Le, "le", OperandKind::None},
    {OpCode::Gt, "gt", OperandKind::None},
    {OpCode::Ge, "ge", OperandKind::None},
    {OpCode::Jump, "jump", OperandKind::U32},
    {OpCode::JumpIfFalse, "jump_if_false", OperandKind::U32},
    {OpCode::JumpIfTrue, "jump_if_true", OperandKind::U32},
    {OpCode::PushHandler, "push_handler", OperandKind::I64},
    {OpCode::PopHandler, "pop_handler", OperandKind::None},
    {OpCode::Throw, "throw", OperandKind::None},
    {OpCode::MakeList, "make_list", OperandKind::U32},
    {OpCode::MakeRange, "make_range", OperandKind::U8},
    {OpCode::GetIndex, "get_index", OperandKind::None},
    {OpCode::SetIndex, "set_index", OperandKind::None},
    {OpCode::GetMember, "get_member", OperandKind::U32},
    {OpCode::GetSuperMember, "get_super_member", OperandKind::U32},
    {OpCode::GetMemberSafe, "get_member_safe", OperandKind::U32},
    {OpCode::SetMember, "set_member", OperandKind::U32},
    {OpCode::Coalesce, "coalesce", OperandKind::None},
    {OpCode::Await, "await", OperandKind::None},
    {OpCode::TypeCheck, "type_check", OperandKind::U32},
    {OpCode::SafeCast, "safe_cast", OperandKind::U32},
    {OpCode::CheckedCast, "checked_cast", OperandKind::U32},
    {OpCode::CreateClosure, "create_closure", OperandKind::U32},
    {OpCode::Call, "call", OperandKind::U32},
    {OpCode::Return, "return", OperandKind::None},
    {OpCode::ConstructData, "construct_data", OperandKind::U32},
    {OpCode::ConstructClass, "construct_class", OperandKind::U32},
    {OpCode::PrintDebugLine, "line", OperandKind::U32},
    {OpCode::Halt, "halt", OperandKind::None},
};

std::string operandToString(const Instruction& instruction, OperandKind operandKind) {
    if (operandKind == OperandKind::None) {
        return {};
    }
    return " " + std::to_string(instruction.operand);
}

}  // namespace

const InstructionSpec& BytecodeLayout::spec(OpCode opcode) {
    for (const auto& spec : kInstructionSpecs) {
        if (spec.opcode == opcode) {
            return spec;
        }
    }
    throw std::runtime_error("unknown opcode");
}

std::string BytecodeLayout::formatInstruction(const Instruction& instruction) {
    const auto& s = spec(instruction.opcode);
    return std::string(s.name) + operandToString(instruction, s.operand);
}

std::string BytecodeLayout::formatModule(const BytecodeModule& module) {
    std::ostringstream out;
    out << "bytecode-module v" << module.version << " {\n";
    out << "  source: " << module.sourceFile << "\n";
    out << "  constants: " << module.constants.size() << "\n";
    out << "  type-constants: " << module.typeConstants.size() << "\n";
    out << "  globals: " << module.globals.size() << "\n";
    out << "  interfaces: " << module.interfaces.size() << "\n";
    out << "  types: " << module.types.size() << "\n";
    out << "  functions: " << module.functions.size() << "\n";
    out << "  entry: " << module.entry.name << " #" << module.entry.functionIndex << "\n";

    for (size_t i = 0; i < module.interfaces.size(); ++i) {
        const auto& iface = module.interfaces[i];
        out << "  interface #" << i << " " << iface.name << " {\n";
        for (const auto& base : iface.baseInterfaces) {
            out << "    extends " << base << "\n";
        }
        for (const auto& method : iface.methods) {
            out << "    fn " << method.name << "(";
            for (size_t p = 0; p < method.parameterTypes.size(); ++p) {
                if (p > 0) {
                    out << ", ";
                }
                out << method.parameterTypes[p].toString();
            }
            out << ") -> " << method.returnType.toString() << "\n";
        }
        out << "  }\n";
    }

    for (size_t i = 0; i < module.types.size(); ++i) {
        const auto& type = module.types[i];
        out << "  type #" << i << " " << (type.abstractClass ? "abstract " : "") << (type.classType ? "class " : "data ") << type.name << " {\n";
        for (const auto& field : type.fields) {
            out << "    field " << field.slot << ": " << field.name << " : " << field.type.toString();
            if (field.defaultFunctionIndex != UINT32_MAX) {
                out << " default=#" << field.defaultFunctionIndex;
            }
            out << "\n";
        }
        for (const auto& iface : type.interfaces) {
            out << "    implements " << iface << "\n";
        }
        for (uint32_t methodIndex : type.methodIndices) {
            out << "    method #" << methodIndex << "\n";
        }
        out << "  }\n";
    }

    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& function = module.functions[i];
        out << "  " << (function.abstractMethod ? "abstract " : "") << (function.async ? "async fn" : "fn") << " #" << i << " " << function.qualifiedName << " -> " << function.returnType.toString()
            << " stack=" << function.maxStack << " {\n";
        for (const auto& capture : function.captures) {
            out << "    capture " << capture.slot << ": " << capture.name << " : " << capture.type.toString() << "\n";
        }
        for (const auto& parameter : function.parameters) {
            out << "    param " << parameter.slot << ": " << parameter.name << " : " << parameter.type.toString() << "\n";
        }
        for (size_t pc = 0; pc < function.instructions.size(); ++pc) {
            out << "    " << pc << ": " << formatInstruction(function.instructions[pc]) << "\n";
        }
        out << "  }\n";
    }

    out << "}\n";
    return out.str();
}

}  // namespace smush
