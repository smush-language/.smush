#include "package.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "lexer.h"
#include "parser.h"
#include "stdlib_asset.h"

namespace smush {

namespace {

constexpr char kMagic[] = {'S', 'M', 'P', 'K'};
constexpr uint32_t kVersion = 1;
constexpr char kBytecodeMagic[] = {'S', 'M', 'B', 'C'};
constexpr uint32_t kBytecodeVersion = 12;

bool writeUint8(std::ofstream& out, uint8_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(out);
}

bool readUint8(std::ifstream& in, uint8_t& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(in);
}

bool writeInt64(std::ofstream& out, int64_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(out);
}

bool readInt64(std::ifstream& in, int64_t& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(in);
}

bool writeDouble(std::ofstream& out, double value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(out);
}

bool readDouble(std::ifstream& in, double& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(in);
}

bool writeUint32(std::ofstream& out, uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(out);
}

bool readUint32(std::ifstream& in, uint32_t& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(in);
}

bool writeString(std::ofstream& out, const std::string& value) {
    if (!writeUint32(out, static_cast<uint32_t>(value.size()))) {
        return false;
    }
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(out);
}

bool readString(std::ifstream& in, std::string& value) {
    uint32_t size = 0;
    if (!readUint32(in, size)) {
        return false;
    }
    value.resize(size);
    in.read(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

std::string readFile(const std::string& path, std::vector<std::string>& errors) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        errors.push_back("failed to open file: " + path);
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool readEmbeddedStdlibModule(const std::string& modulePath, std::string& source) {
    for (std::size_t i = 0; i < smush::assets::kEmbeddedStdlibModuleCount; ++i) {
        const auto& module = smush::assets::kEmbeddedStdlibModules[i];
        if (modulePath == module.moduleName) {
            source.assign(reinterpret_cast<const char*>(module.bytes), module.size);
            return true;
        }
    }
    return false;
}

bool isStdImportPath(const std::string& importPath) {
    return importPath.rfind("std::", 0) == 0;
}

bool isStdInternalPath(const std::string& importPath) {
    return importPath == "std::internal" || importPath.rfind("std::internal::", 0) == 0;
}

bool isStdModulePath(const std::string& sourcePath) {
    return isStdImportPath(sourcePath);
}

bool validateStdModuleSyntax(const std::string& modulePath,
                             const std::string& contextPath,
                             std::vector<std::string>& errors) {
    if (!isStdImportPath(modulePath)) {
        return true;
    }

    const std::string suffix = modulePath.substr(5);
    if (suffix.empty()) {
        errors.push_back(contextPath + ": invalid stdlib module name '" + modulePath + "'");
        return false;
    }

    size_t segmentStart = 0;
    while (segmentStart < suffix.size()) {
        const size_t segmentEnd = suffix.find("::", segmentStart);
        const size_t length = (segmentEnd == std::string::npos) ? suffix.size() - segmentStart
                                                                : segmentEnd - segmentStart;
        if (length == 0) {
            errors.push_back(contextPath + ": invalid stdlib module name '" + modulePath + "'");
            return false;
        }
        segmentStart = (segmentEnd == std::string::npos) ? suffix.size() : segmentEnd + 2;
    }

    return true;
}

bool validateStdImportRequest(const std::string& sourcePath,
                              const std::string& importPath,
                              std::vector<std::string>& errors) {
    if (!isStdImportPath(importPath)) {
        return true;
    }

    if (!validateStdModuleSyntax(importPath, sourcePath, errors)) {
        return false;
    }

    if (isStdInternalPath(importPath) && !isStdModulePath(sourcePath)) {
        errors.push_back(sourcePath + ": stdlib internal module imports are reserved: '" + importPath + "'");
        return false;
    }

    return true;
}

std::string stdModuleToRelativePath(const std::string& importPath) {
    std::string relative = importPath.substr(5);
    size_t pos = 0;
    while ((pos = relative.find("::", pos)) != std::string::npos) {
        relative.replace(pos, 2, "/");
        pos += 1;
    }
    relative += ".smush";
    return relative;
}

std::filesystem::path findStdlibRoot(std::filesystem::path start) {
    namespace fs = std::filesystem;

    if (fs::exists(start / "stdlib") && fs::is_directory(start / "stdlib")) {
        return start / "stdlib";
    }

    while (!start.empty()) {
        const fs::path candidate = start / "stdlib";
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            return candidate;
        }
        const fs::path parent = start.parent_path();
        if (parent == start) {
            break;
        }
        start = parent;
    }

    const fs::path cwdCandidate = fs::current_path() / "stdlib";
    if (fs::exists(cwdCandidate) && fs::is_directory(cwdCandidate)) {
        return cwdCandidate;
    }

    return {};
}

bool resolveImportBundlePath(const std::string& sourcePath,
                             const std::string& importPath,
                             std::string& resolvedPath,
                             std::vector<std::string>& errors) {
    namespace fs = std::filesystem;

    if (isStdImportPath(importPath)) {
        if (!validateStdImportRequest(sourcePath, importPath, errors)) {
            return false;
        }
        resolvedPath = importPath;
        return true;
    }

    std::error_code ec;
    const fs::path baseDir = fs::path(sourcePath).parent_path();
    const fs::path resolved = fs::weakly_canonical(baseDir / importPath, ec);
    if (ec) {
        errors.push_back(sourcePath + ": failed to resolve import '" + importPath + "'");
        return false;
    }
    resolvedPath = resolved.generic_string();
    return true;
}

bool resolveImportSource(const std::string& sourcePath,
                         const std::string& importPath,
                         std::string& bundlePath,
                         std::string& filesystemPath,
                         std::vector<std::string>& errors) {
    namespace fs = std::filesystem;

    if (isStdImportPath(importPath)) {
        if (!validateStdImportRequest(sourcePath, importPath, errors)) {
            return false;
        }
        std::string embeddedSource;
        if (!readEmbeddedStdlibModule(importPath, embeddedSource)) {
            errors.push_back(sourcePath + ": unknown stdlib module '" + importPath + "'");
            return false;
        }
        bundlePath = importPath;
        filesystemPath.clear();
        return true;
    }

    if (!resolveImportBundlePath(sourcePath, importPath, bundlePath, errors)) {
        return false;
    }
    filesystemPath = bundlePath;
    return true;
}

bool containsPathTraversal(const std::string& value) {
    return value.find("..") != std::string::npos;
}

bool writeIRType(std::ofstream& out, const IRType& type) {
    return writeUint8(out, static_cast<uint8_t>(type.kind)) &&
           writeUint8(out, type.nullable ? 1 : 0) &&
           writeString(out, type.name);
}

bool readIRType(std::ifstream& in, IRType& type) {
    uint8_t kind = 0;
    uint8_t nullable = 0;
    if (!readUint8(in, kind) || !readUint8(in, nullable) || !readString(in, type.name)) {
        return false;
    }
    type.kind = static_cast<IRTypeKind>(kind);
    type.nullable = nullable != 0;
    return true;
}

bool writeSourceLocation(std::ofstream& out, const SourceLocation& loc) {
    return writeUint32(out, loc.line) &&
           writeUint32(out, loc.column) &&
           writeString(out, loc.filename);
}

bool readSourceLocation(std::ifstream& in, SourceLocation& loc) {
    uint32_t line = 0;
    uint32_t column = 0;
    if (!readUint32(in, line) ||
        !readUint32(in, column) ||
        !readString(in, loc.filename)) {
        return false;
    }
    loc.line = static_cast<int>(line);
    loc.column = static_cast<int>(column);
    return true;
}

bool writeConstant(std::ofstream& out, const ConstantValue& constant) {
    if (!writeUint8(out, static_cast<uint8_t>(constant.kind))) {
        return false;
    }
    switch (constant.kind) {
        case ConstantKind::Null:
            return true;
        case ConstantKind::Bool:
            return writeUint8(out, std::get<bool>(constant.value) ? 1 : 0);
        case ConstantKind::Int:
            return writeInt64(out, std::get<int64_t>(constant.value));
        case ConstantKind::Float:
            return writeDouble(out, std::get<double>(constant.value));
        case ConstantKind::String:
            return writeString(out, std::get<std::string>(constant.value));
    }
    return false;
}

bool readConstant(std::ifstream& in, ConstantValue& constant) {
    uint8_t kind = 0;
    if (!readUint8(in, kind)) {
        return false;
    }
    constant.kind = static_cast<ConstantKind>(kind);
    switch (constant.kind) {
        case ConstantKind::Null:
            constant.value = std::monostate{};
            return true;
        case ConstantKind::Bool: {
            uint8_t value = 0;
            if (!readUint8(in, value)) {
                return false;
            }
            constant.value = value != 0;
            return true;
        }
        case ConstantKind::Int: {
            int64_t value = 0;
            if (!readInt64(in, value)) {
                return false;
            }
            constant.value = value;
            return true;
        }
        case ConstantKind::Float: {
            double value = 0.0;
            if (!readDouble(in, value)) {
                return false;
            }
            constant.value = value;
            return true;
        }
        case ConstantKind::String: {
            std::string value;
            if (!readString(in, value)) {
                return false;
            }
            constant.value = std::move(value);
            return true;
        }
    }
    return false;
}

}  // namespace

bool PackageFile::write(const std::string& path, const SourceBundle& bundle, std::vector<std::string>& errors) {
    const std::filesystem::path outputPath(path);
    if (outputPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        if (ec) {
            errors.push_back("failed to create package directory: " + outputPath.parent_path().generic_string());
            return false;
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        errors.push_back("failed to create package: " + path);
        return false;
    }

    out.write(kMagic, sizeof(kMagic));
    if (!writeUint32(out, kVersion) ||
        !writeString(out, bundle.entryPath) ||
        !writeUint32(out, static_cast<uint32_t>(bundle.sources.size()))) {
        errors.push_back("failed to write package header: " + path);
        return false;
    }

    for (const auto& entry : bundle.sources) {
        if (!writeString(out, entry.first) || !writeString(out, entry.second)) {
            errors.push_back("failed to write package entry: " + entry.first);
            return false;
        }
    }

    return true;
}

bool PackageFile::read(const std::string& path, SourceBundle& bundle, std::vector<std::string>& errors) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        errors.push_back("failed to open package: " + path);
        return false;
    }

    char magic[sizeof(kMagic)] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::equal(std::begin(magic), std::end(magic), std::begin(kMagic)) == false) {
        errors.push_back("invalid package magic: " + path);
        return false;
    }

    uint32_t version = 0;
    if (!readUint32(in, version) || version != kVersion) {
        errors.push_back("unsupported package version in: " + path);
        return false;
    }

    bundle = SourceBundle{};
    if (!readString(in, bundle.entryPath)) {
        errors.push_back("failed to read package entry path: " + path);
        return false;
    }
    if (containsPathTraversal(bundle.entryPath)) {
        errors.push_back("invalid package entry path");
        return false;
    }
    if (isStdImportPath(bundle.entryPath) && !validateStdModuleSyntax(bundle.entryPath, "package entry path", errors)) {
        return false;
    }

    uint32_t sourceCount = 0;
    if (!readUint32(in, sourceCount)) {
        errors.push_back("failed to read package source count: " + path);
        return false;
    }

    for (uint32_t i = 0; i < sourceCount; ++i) {
        std::string sourcePath;
        std::string sourceText;
        if (!readString(in, sourcePath) || !readString(in, sourceText)) {
            errors.push_back("failed to read package source entry");
            return false;
        }
        if (containsPathTraversal(sourcePath)) {
            errors.push_back("invalid package source path: " + sourcePath);
            return false;
        }
        if (isStdImportPath(sourcePath) && !validateStdModuleSyntax(sourcePath, "package source path", errors)) {
            return false;
        }
        bundle.sources.emplace(std::move(sourcePath), std::move(sourceText));
    }

    if (bundle.sources.find(bundle.entryPath) == bundle.sources.end()) {
        errors.push_back("package entry source not found: " + bundle.entryPath);
        return false;
    }

    return true;
}

bool BytecodePackageFile::write(const std::string& path, const BytecodeModule& module, std::vector<std::string>& errors) {
    const std::filesystem::path outputPath(path);
    if (outputPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        if (ec) {
            errors.push_back("failed to create bytecode package directory: " + outputPath.parent_path().generic_string());
            return false;
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        errors.push_back("failed to create bytecode package: " + path);
        return false;
    }

    out.write(kBytecodeMagic, sizeof(kBytecodeMagic));
    if (!writeUint32(out, kBytecodeVersion) ||
        !writeString(out, module.sourceFile) ||
        !writeUint32(out, static_cast<uint32_t>(module.entry.functionIndex)) ||
        !writeString(out, module.entry.name) ||
        !writeUint32(out, static_cast<uint32_t>(module.constants.size())) ||
        !writeUint32(out, static_cast<uint32_t>(module.typeConstants.size())) ||
        !writeUint32(out, static_cast<uint32_t>(module.globals.size())) ||
        !writeUint32(out, static_cast<uint32_t>(module.interfaces.size())) ||
        !writeUint32(out, static_cast<uint32_t>(module.types.size())) ||
        !writeUint32(out, static_cast<uint32_t>(module.functions.size()))) {
        errors.push_back("failed to write bytecode package header: " + path);
        return false;
    }

    for (const auto& constant : module.constants) {
        if (!writeConstant(out, constant)) {
            errors.push_back("failed to write bytecode constant");
            return false;
        }
    }

    for (const auto& type : module.typeConstants) {
        if (!writeIRType(out, type)) {
            errors.push_back("failed to write bytecode type constant");
            return false;
        }
    }

    for (const auto& global : module.globals) {
        if (!writeString(out, global)) {
            errors.push_back("failed to write bytecode global");
            return false;
        }
    }

    for (const auto& iface : module.interfaces) {
        if (!writeString(out, iface.name) ||
            !writeUint32(out, static_cast<uint32_t>(iface.baseInterfaces.size())) ||
            !writeUint32(out, static_cast<uint32_t>(iface.methods.size())) ||
            !writeSourceLocation(out, iface.loc)) {
            errors.push_back("failed to write bytecode interface header: " + iface.name);
            return false;
        }
        for (const auto& base : iface.baseInterfaces) {
            if (!writeString(out, base)) {
                errors.push_back("failed to write bytecode interface base: " + iface.name);
                return false;
            }
        }
        for (const auto& method : iface.methods) {
            if (!writeString(out, method.name) ||
                !writeUint32(out, static_cast<uint32_t>(method.parameterTypes.size())) ||
                !writeIRType(out, method.returnType) ||
                !writeUint8(out, method.async ? 1 : 0) ||
                !writeUint8(out, method.staticMethod ? 1 : 0) ||
                !writeUint8(out, method.abstractMethod ? 1 : 0) ||
                !writeUint8(out, static_cast<uint8_t>(method.visibility))) {
                errors.push_back("failed to write bytecode interface method: " + iface.name + "." + method.name);
                return false;
            }
            for (const auto& parameterType : method.parameterTypes) {
                if (!writeIRType(out, parameterType)) {
                    errors.push_back("failed to write bytecode interface parameter type: " + iface.name + "." + method.name);
                    return false;
                }
            }
        }
    }

    for (const auto& type : module.types) {
        if (!writeString(out, type.name) ||
            !writeString(out, type.baseType) ||
            !writeUint8(out, type.classType ? 1 : 0) ||
            !writeUint8(out, type.abstractClass ? 1 : 0) ||
            !writeUint32(out, static_cast<uint32_t>(type.fields.size())) ||
            !writeUint32(out, static_cast<uint32_t>(type.interfaces.size())) ||
            !writeUint32(out, static_cast<uint32_t>(type.methodIndices.size())) ||
            !writeUint32(out, static_cast<uint32_t>(type.staticMethodIndices.size())) ||
            !writeUint32(out, static_cast<uint32_t>(type.constructorIndices.size())) ||
            !writeSourceLocation(out, type.loc)) {
            errors.push_back("failed to write bytecode type header: " + type.name);
            return false;
        }
        for (const auto& field : type.fields) {
            if (!writeString(out, field.name) ||
                !writeIRType(out, field.type) ||
                !writeUint32(out, field.slot) ||
                !writeUint32(out, field.defaultFunctionIndex)) {
                errors.push_back("failed to write bytecode field: " + field.name);
                return false;
            }
        }
        for (const auto& iface : type.interfaces) {
            if (!writeString(out, iface)) {
                errors.push_back("failed to write bytecode interface for type: " + type.name);
                return false;
            }
        }
        for (uint32_t methodIndex : type.methodIndices) {
            if (!writeUint32(out, methodIndex)) {
                errors.push_back("failed to write bytecode method index for type: " + type.name);
                return false;
            }
        }
        for (uint32_t methodIndex : type.staticMethodIndices) {
            if (!writeUint32(out, methodIndex)) {
                errors.push_back("failed to write bytecode static method index for type: " + type.name);
                return false;
            }
        }
        for (uint32_t ctorIndex : type.constructorIndices) {
            if (!writeUint32(out, ctorIndex)) {
                errors.push_back("failed to write bytecode constructor index for type: " + type.name);
                return false;
            }
        }
    }

    for (const auto& function : module.functions) {
        if (!writeString(out, function.name) ||
            !writeString(out, function.qualifiedName) ||
            !writeIRType(out, function.returnType) ||
            !writeUint8(out, function.async ? 1 : 0) ||
            !writeUint8(out, function.method ? 1 : 0) ||
            !writeUint8(out, function.staticMethod ? 1 : 0) ||
            !writeUint8(out, function.constructor ? 1 : 0) ||
            !writeUint8(out, function.abstractMethod ? 1 : 0) ||
            !writeUint8(out, static_cast<uint8_t>(function.visibility)) ||
            !writeString(out, function.ownerType) ||
            !writeUint32(out, static_cast<uint32_t>(function.captures.size())) ||
            !writeUint32(out, static_cast<uint32_t>(function.parameters.size())) ||
            !writeUint32(out, static_cast<uint32_t>(function.locals.size())) ||
            !writeUint32(out, static_cast<uint32_t>(function.instructions.size())) ||
            !writeUint32(out, function.maxStack) ||
            !writeSourceLocation(out, function.loc)) {
            errors.push_back("failed to write bytecode function header: " + function.qualifiedName);
            return false;
        }

        for (const auto& capture : function.captures) {
            if (!writeString(out, capture.name) ||
                !writeIRType(out, capture.type) ||
                !writeUint8(out, capture.mutableBinding ? 1 : 0) ||
                !writeUint32(out, capture.slot)) {
                errors.push_back("failed to write bytecode capture in: " + function.qualifiedName);
                return false;
            }
        }

        for (const auto& parameter : function.parameters) {
            if (!writeString(out, parameter.name) ||
                !writeIRType(out, parameter.type) ||
                !writeUint8(out, parameter.mutableBinding ? 1 : 0) ||
                !writeUint32(out, parameter.slot)) {
                errors.push_back("failed to write bytecode parameter in: " + function.qualifiedName);
                return false;
            }
        }

        for (const auto& local : function.locals) {
            if (!writeString(out, local.name) ||
                !writeIRType(out, local.type) ||
                !writeUint8(out, local.mutableBinding ? 1 : 0) ||
                !writeUint32(out, local.slot)) {
                errors.push_back("failed to write bytecode local in: " + function.qualifiedName);
                return false;
            }
        }

        for (const auto& instruction : function.instructions) {
            if (!writeUint8(out, static_cast<uint8_t>(instruction.opcode)) ||
                !writeInt64(out, instruction.operand) ||
                !writeSourceLocation(out, instruction.loc)) {
                errors.push_back("failed to write bytecode instruction in: " + function.qualifiedName);
                return false;
            }
        }
    }

    return true;
}

bool BytecodePackageFile::read(const std::string& path, BytecodeModule& module, std::vector<std::string>& errors) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        errors.push_back("failed to open bytecode package: " + path);
        return false;
    }

    char magic[sizeof(kBytecodeMagic)] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::equal(std::begin(magic), std::end(magic), std::begin(kBytecodeMagic)) == false) {
        errors.push_back("invalid bytecode package magic: " + path);
        return false;
    }

    uint32_t version = 0;
    if (!readUint32(in, version) || version != kBytecodeVersion) {
        errors.push_back("unsupported bytecode package version in: " + path);
        return false;
    }

    module = BytecodeModule{};
    module.version = version;
    uint32_t entryFunctionIndex = 0;
    uint32_t constantCount = 0;
    uint32_t typeConstantCount = 0;
    uint32_t globalCount = 0;
    uint32_t interfaceCount = 0;
    uint32_t typeCount = 0;
    uint32_t functionCount = 0;
    if (!readString(in, module.sourceFile) ||
        !readUint32(in, entryFunctionIndex) ||
        !readString(in, module.entry.name) ||
        !readUint32(in, constantCount) ||
        !readUint32(in, typeConstantCount) ||
        !readUint32(in, globalCount) ||
        !readUint32(in, interfaceCount) ||
        !readUint32(in, typeCount) ||
        !readUint32(in, functionCount)) {
        errors.push_back("failed to read bytecode package header: " + path);
        return false;
    }
    module.entry.functionIndex = entryFunctionIndex;

    module.constants.resize(constantCount);
    for (uint32_t i = 0; i < constantCount; ++i) {
        if (!readConstant(in, module.constants[i])) {
            errors.push_back("failed to read bytecode constant");
            return false;
        }
    }

    module.typeConstants.resize(typeConstantCount);
    for (uint32_t i = 0; i < typeConstantCount; ++i) {
        if (!readIRType(in, module.typeConstants[i])) {
            errors.push_back("failed to read bytecode type constant");
            return false;
        }
    }

    module.globals.resize(globalCount);
    for (uint32_t i = 0; i < globalCount; ++i) {
        if (!readString(in, module.globals[i])) {
            errors.push_back("failed to read bytecode global");
            return false;
        }
    }

    module.interfaces.resize(interfaceCount);
    for (uint32_t i = 0; i < interfaceCount; ++i) {
        auto& iface = module.interfaces[i];
        uint32_t baseCount = 0;
        uint32_t methodCount = 0;
        if (!readString(in, iface.name) ||
            !readUint32(in, baseCount) ||
            !readUint32(in, methodCount) ||
            !readSourceLocation(in, iface.loc)) {
            errors.push_back("failed to read bytecode interface header");
            return false;
        }
        iface.baseInterfaces.resize(baseCount);
        iface.methods.resize(methodCount);
        for (uint32_t baseIndex = 0; baseIndex < baseCount; ++baseIndex) {
            if (!readString(in, iface.baseInterfaces[baseIndex])) {
                errors.push_back("failed to read bytecode interface base");
                return false;
            }
        }
        for (uint32_t methodIndex = 0; methodIndex < methodCount; ++methodIndex) {
            auto& method = iface.methods[methodIndex];
            uint32_t parameterCount = 0;
            uint8_t async = 0;
            uint8_t staticMethod = 0;
            uint8_t abstractMethod = 0;
            uint8_t visibility = 0;
            if (!readString(in, method.name) ||
                !readUint32(in, parameterCount) ||
                !readIRType(in, method.returnType) ||
                !readUint8(in, async) ||
                !readUint8(in, staticMethod) ||
                !readUint8(in, abstractMethod) ||
                !readUint8(in, visibility)) {
                errors.push_back("failed to read bytecode interface method");
                return false;
            }
            method.parameterTypes.resize(parameterCount);
            method.async = async != 0;
            method.staticMethod = staticMethod != 0;
            method.abstractMethod = abstractMethod != 0;
            method.visibility = static_cast<Visibility>(visibility);
            for (uint32_t paramIndex = 0; paramIndex < parameterCount; ++paramIndex) {
                if (!readIRType(in, method.parameterTypes[paramIndex])) {
                    errors.push_back("failed to read bytecode interface parameter type");
                    return false;
                }
            }
        }
        module.interfaceMap[iface.name] = i;
    }

    module.types.resize(typeCount);
    for (uint32_t i = 0; i < typeCount; ++i) {
        auto& type = module.types[i];
        uint8_t classType = 0;
        uint8_t abstractClass = 0;
        uint32_t fieldCount = 0;
        uint32_t typeInterfaceCount = 0;
        uint32_t methodCount = 0;
        uint32_t staticMethodCount = 0;
        uint32_t constructorCount = 0;
        if (!readString(in, type.name) ||
            !readString(in, type.baseType) ||
            !readUint8(in, classType) ||
            !readUint8(in, abstractClass) ||
            !readUint32(in, fieldCount) ||
            !readUint32(in, typeInterfaceCount) ||
            !readUint32(in, methodCount) ||
            !readUint32(in, staticMethodCount) ||
            !readUint32(in, constructorCount) ||
            !readSourceLocation(in, type.loc)) {
            errors.push_back("failed to read bytecode type header");
            return false;
        }
        type.classType = classType != 0;
        type.abstractClass = abstractClass != 0;
        type.fields.resize(fieldCount);
        type.interfaces.resize(typeInterfaceCount);
        type.methodIndices.resize(methodCount);
        type.staticMethodIndices.resize(staticMethodCount);
        type.constructorIndices.resize(constructorCount);
        for (uint32_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
            if (!readString(in, type.fields[fieldIndex].name) ||
                !readIRType(in, type.fields[fieldIndex].type) ||
                !readUint32(in, type.fields[fieldIndex].slot) ||
                !readUint32(in, type.fields[fieldIndex].defaultFunctionIndex)) {
                errors.push_back("failed to read bytecode field");
                return false;
            }
        }
        for (uint32_t interfaceIndex = 0; interfaceIndex < typeInterfaceCount; ++interfaceIndex) {
            if (!readString(in, type.interfaces[interfaceIndex])) {
                errors.push_back("failed to read bytecode interface");
                return false;
            }
        }
        for (uint32_t methodIndex = 0; methodIndex < methodCount; ++methodIndex) {
            if (!readUint32(in, type.methodIndices[methodIndex])) {
                errors.push_back("failed to read bytecode method index");
                return false;
            }
        }
        for (uint32_t methodIndex = 0; methodIndex < staticMethodCount; ++methodIndex) {
            if (!readUint32(in, type.staticMethodIndices[methodIndex])) {
                errors.push_back("failed to read bytecode static method index");
                return false;
            }
        }
        for (uint32_t ctorIndex = 0; ctorIndex < constructorCount; ++ctorIndex) {
            if (!readUint32(in, type.constructorIndices[ctorIndex])) {
                errors.push_back("failed to read bytecode constructor index");
                return false;
            }
        }
        module.typeMap[type.name] = i;
    }

    module.functions.resize(functionCount);
    for (uint32_t i = 0; i < functionCount; ++i) {
        auto& function = module.functions[i];
        uint8_t async = 0;
        uint8_t method = 0;
        uint8_t staticMethod = 0;
        uint8_t constructor = 0;
        uint8_t abstractMethod = 0;
        uint8_t visibility = 0;
        uint32_t captureCount = 0;
        uint32_t parameterCount = 0;
        uint32_t localCount = 0;
        uint32_t instructionCount = 0;
        if (!readString(in, function.name) ||
            !readString(in, function.qualifiedName) ||
            !readIRType(in, function.returnType) ||
            !readUint8(in, async) ||
            !readUint8(in, method) ||
            !readUint8(in, staticMethod) ||
            !readUint8(in, constructor) ||
            !readUint8(in, abstractMethod) ||
            !readUint8(in, visibility) ||
            !readString(in, function.ownerType) ||
            !readUint32(in, captureCount) ||
            !readUint32(in, parameterCount) ||
            !readUint32(in, localCount) ||
            !readUint32(in, instructionCount) ||
            !readUint32(in, function.maxStack) ||
            !readSourceLocation(in, function.loc)) {
            errors.push_back("failed to read bytecode function header");
            return false;
        }
        function.async = async != 0;
        function.method = method != 0;
        function.staticMethod = staticMethod != 0;
        function.constructor = constructor != 0;
        function.abstractMethod = abstractMethod != 0;
        function.visibility = static_cast<Visibility>(visibility);
        function.captures.resize(captureCount);
        function.parameters.resize(parameterCount);
        function.locals.resize(localCount);
        function.instructions.resize(instructionCount);

        for (uint32_t captureIndex = 0; captureIndex < captureCount; ++captureIndex) {
            auto& capture = function.captures[captureIndex];
            uint8_t mutableBinding = 0;
            if (!readString(in, capture.name) ||
                !readIRType(in, capture.type) ||
                !readUint8(in, mutableBinding) ||
                !readUint32(in, capture.slot)) {
                errors.push_back("failed to read bytecode capture");
                return false;
            }
            capture.mutableBinding = mutableBinding != 0;
        }

        for (uint32_t parameterIndex = 0; parameterIndex < parameterCount; ++parameterIndex) {
            auto& parameter = function.parameters[parameterIndex];
            uint8_t mutableBinding = 0;
            if (!readString(in, parameter.name) ||
                !readIRType(in, parameter.type) ||
                !readUint8(in, mutableBinding) ||
                !readUint32(in, parameter.slot)) {
                errors.push_back("failed to read bytecode parameter");
                return false;
            }
            parameter.mutableBinding = mutableBinding != 0;
        }

        for (uint32_t localIndex = 0; localIndex < localCount; ++localIndex) {
            auto& local = function.locals[localIndex];
            uint8_t mutableBinding = 0;
            if (!readString(in, local.name) ||
                !readIRType(in, local.type) ||
                !readUint8(in, mutableBinding) ||
                !readUint32(in, local.slot)) {
                errors.push_back("failed to read bytecode local");
                return false;
            }
            local.mutableBinding = mutableBinding != 0;
        }

        for (uint32_t instructionIndex = 0; instructionIndex < instructionCount; ++instructionIndex) {
            uint8_t opcode = 0;
            if (!readUint8(in, opcode) ||
                !readInt64(in, function.instructions[instructionIndex].operand) ||
                !readSourceLocation(in, function.instructions[instructionIndex].loc)) {
                errors.push_back("failed to read bytecode instruction");
                return false;
            }
            function.instructions[instructionIndex].opcode = static_cast<OpCode>(opcode);
        }

        module.functionMap[function.qualifiedName] = i;
        if (!function.method && function.ownerType.empty()) {
            module.functionMap[function.name] = i;
        }
    }

    if (!module.entry.name.empty() &&
        module.entry.functionIndex >= module.functions.size()) {
        errors.push_back("bytecode package entry function is out of range");
        return false;
    }

    return true;
}

bool ProjectLoader::loadBundleFromFilesystem(const std::string& entryPath,
                                             SourceBundle& bundle,
                                             std::vector<std::string>& errors) {
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(entryPath, ec);
    if (ec) {
        errors.push_back("failed to resolve entry path: " + entryPath);
        return false;
    }

    bundle = SourceBundle{};
    bundle.entryPath = canonical.generic_string();
    return loadSourceRecursive(bundle.entryPath, bundle.entryPath, bundle, errors);
}

std::unique_ptr<Program> ProjectLoader::loadProgramFromBundle(const SourceBundle& bundle,
                                                              std::vector<std::string>& errors) {
    std::unordered_map<std::string, bool> visiting;
    std::unordered_map<std::string, bool> visited;
    return loadProgramRecursive(bundle, bundle.entryPath, errors, visiting, visited);
}

bool ProjectLoader::loadSourceRecursive(const std::string& bundlePath,
                                        const std::string& filesystemPath,
                                        SourceBundle& bundle,
                                        std::vector<std::string>& errors) {
    if (bundle.sources.find(bundlePath) != bundle.sources.end()) {
        return true;
    }

    std::string source;
    if (isStdImportPath(bundlePath)) {
        if (!readEmbeddedStdlibModule(bundlePath, source)) {
            errors.push_back("unknown stdlib module '" + bundlePath + "'");
            return false;
        }
    } else {
        source = readFile(filesystemPath, errors);
        if (!errors.empty()) {
            return false;
        }
    }
    bundle.sources.emplace(bundlePath, source);

    std::vector<std::string> imports;
    if (!parseImports(bundlePath, source, imports, errors)) {
        return false;
    }

    for (const auto& importPath : imports) {
        std::string importedBundlePath;
        std::string importedFilesystemPath;
        if (!resolveImportSource(bundlePath, importPath, importedBundlePath, importedFilesystemPath, errors)) {
            return false;
        }
        if (!loadSourceRecursive(importedBundlePath, importedFilesystemPath, bundle, errors)) {
            return false;
        }
    }

    return true;
}

bool ProjectLoader::parseImports(const std::string& filename,
                                 const std::string& source,
                                 std::vector<std::string>& imports,
                                 std::vector<std::string>& errors) {
    Lexer lexer(source, filename);
    auto tokens = lexer.tokenize();
    if (!lexer.getErrors().empty()) {
        errors.insert(errors.end(), lexer.getErrors().begin(), lexer.getErrors().end());
        return false;
    }

    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::IMPORT) {
            if (tokens[i + 1].type != TokenType::STRING) {
                errors.push_back(filename + ":" + std::to_string(tokens[i].loc.line) + ":" +
                                 std::to_string(tokens[i].loc.column) + ": expected import path string");
                return false;
            }
            imports.push_back(tokens[i + 1].lexeme);
        }
    }

    return true;
}

std::unique_ptr<Program> ProjectLoader::loadProgramRecursive(const SourceBundle& bundle,
                                                             const std::string& path,
                                                             std::vector<std::string>& errors,
                                                             std::unordered_map<std::string, bool>& visiting,
                                                             std::unordered_map<std::string, bool>& visited) {
    namespace fs = std::filesystem;

    if (visited[path]) {
        return std::make_unique<Program>(SourceLocation{1, 1, path});
    }
    if (visiting[path]) {
        errors.push_back(path + ": cyclic import detected");
        return nullptr;
    }

    auto it = bundle.sources.find(path);
    if (it == bundle.sources.end()) {
        if (isStdImportPath(path)) {
            errors.push_back("stdlib module not found in bundle: " + path);
        } else {
            errors.push_back("source not found in bundle: " + path);
        }
        return nullptr;
    }

    visiting[path] = true;
    Lexer lexer(it->second, path);
    auto tokens = lexer.tokenize();
    if (!lexer.getErrors().empty()) {
        errors.insert(errors.end(), lexer.getErrors().begin(), lexer.getErrors().end());
        return nullptr;
    }

    Parser parser(std::move(tokens));
    auto program = parser.parse();
    if (!parser.getErrors().empty()) {
        errors.insert(errors.end(), parser.getErrors().begin(), parser.getErrors().end());
        return nullptr;
    }

    auto merged = std::make_unique<Program>(program->loc);
    for (auto& stmt : program->statements) {
        if (stmt->kind != StatementKind::ImportDecl) {
            merged->statements.push_back(std::move(stmt));
            continue;
        }

        const auto* importDecl = static_cast<const ImportDecl*>(stmt.get());
        std::string resolved;
        if (!resolveImportBundlePath(path, importDecl->path, resolved, errors)) {
            return nullptr;
        }
        if (isStdImportPath(resolved) && bundle.sources.find(resolved) == bundle.sources.end()) {
            errors.push_back(path + ": stdlib module not found in bundle: '" + resolved + "'");
            return nullptr;
        }
        auto imported = loadProgramRecursive(bundle, resolved, errors, visiting, visited);
        if (!imported) {
            return nullptr;
        }
        for (auto& importedStmt : imported->statements) {
            merged->statements.push_back(std::move(importedStmt));
        }
    }

    visiting[path] = false;
    visited[path] = true;
    return merged;
}

}  // namespace smush
