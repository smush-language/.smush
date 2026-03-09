#include "ast.h"

namespace smush {

std::string Type::toString() const {
    if (kind == TypeKind::Function) {
        std::string rendered = "(";
        for (size_t i = 0; i < parameterTypes.size(); ++i) {
            if (i > 0) {
                rendered += ", ";
            }
            rendered += parameterTypes[i] ? parameterTypes[i]->toString() : "Any";
        }
        rendered += ") -> ";
        rendered += returnType ? returnType->toString() : "Void";
        if (nullable) {
            rendered += "?";
        }
        return rendered;
    }
    if (kind == TypeKind::Union || kind == TypeKind::Intersection) {
        std::string rendered;
        const char* separator = kind == TypeKind::Union ? " | " : " & ";
        for (size_t i = 0; i < members.size(); ++i) {
            if (i > 0) {
                rendered += separator;
            }
            rendered += members[i] ? members[i]->toString() : "Any";
        }
        if (nullable) {
            rendered += "?";
        }
        return rendered;
    }
    std::string rendered = name;
    if (!genericArguments.empty()) {
        rendered += "<";
        for (size_t i = 0; i < genericArguments.size(); ++i) {
            if (i > 0) {
                rendered += ", ";
            }
            rendered += genericArguments[i] ? genericArguments[i]->toString() : "Any";
        }
        rendered += ">";
    }
    if (nullable) {
        rendered += "?";
    }
    return rendered;
}

}  // namespace smush
