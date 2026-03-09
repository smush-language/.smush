#include <iostream>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <cstdlib>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "bytecode_lowering.h"
#include "bytecode_vm.h"
#include "ir.h"
#include "package.h"
#include "semantic.h"
#include "splash_asset.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void printUsage();

struct ConsoleTheme {
    bool stderrColor = false;
    bool stdoutColor = false;
};

ConsoleTheme g_theme;

constexpr const char* kAccent = "\x1b[38;2;162;112;255m";
constexpr const char* kAccentSoft = "\x1b[38;2;134;116;196m";
constexpr const char* kAccentDim = "\x1b[38;2;122;110;142m";
constexpr const char* kText = "\x1b[38;2;221;218;232m";
constexpr const char* kMuted = "\x1b[38;2;128;128;128m";
constexpr const char* kWarn = "\x1b[38;2;214;137;255m";
constexpr const char* kSmoosh = "\x1b[38;2;198;150;255m";

bool envEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0';
}

#if defined(_WIN32)
bool enableVirtualTerminal(HANDLE handle) {
    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) {
        return false;
    }
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        return true;
    }
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

bool streamIsTerminal(FILE* stream) {
    return _isatty(_fileno(stream)) != 0;
}

void initializeUnicodeConsole() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#else
bool streamIsTerminal(FILE* stream) {
    return isatty(fileno(stream)) != 0;
}
#endif

void initializeConsoleTheme() {
#if defined(_WIN32)
    initializeUnicodeConsole();
#endif

    if (envEnabled("NO_COLOR")) {
        g_theme = {};
        return;
    }

    const char* colorSetting = std::getenv("SMUSHC_COLOR");
    if (colorSetting && std::string(colorSetting) == "always") {
        g_theme.stderrColor = true;
        g_theme.stdoutColor = true;
    } else {
        g_theme.stderrColor = streamIsTerminal(stderr);
        g_theme.stdoutColor = streamIsTerminal(stdout);
    }

#if defined(_WIN32)
    if (g_theme.stderrColor) {
        g_theme.stderrColor = enableVirtualTerminal(GetStdHandle(STD_ERROR_HANDLE));
    }
    if (g_theme.stdoutColor) {
        g_theme.stdoutColor = enableVirtualTerminal(GetStdHandle(STD_OUTPUT_HANDLE));
    }
#endif
}

std::string colorize(const std::string& text, const char* code, bool enabled) {
    if (!enabled) {
        return text;
    }
    return std::string(code) + text + "\x1b[0m";
}

std::string toLowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool containsAny(const std::string& value, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (value.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string ansiColorFromHex(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') {
        return {};
    }

    auto parseByte = [&](size_t offset) -> int {
        return std::stoi(hex.substr(offset, 2), nullptr, 16);
    };

    return "\x1b[38;2;" + std::to_string(parseByte(1)) + ";" +
           std::to_string(parseByte(3)) + ";" +
           std::to_string(parseByte(5)) + "m";
}

void appendVisibleText(std::string& output, const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 6, "&nbsp;") == 0) {
            output.push_back(' ');
            i += 5;
            continue;
        }

        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0) {
            output.push_back(' ');
            ++i;
            continue;
        }
        if (ch == 0xA0) {
            output.push_back(' ');
            continue;
        }

        output.push_back(text[i]);
    }
}

std::string renderBBCode(const std::string& source, bool enableColor) {
    std::string output;
    std::vector<std::string> colorStack;
    std::vector<bool> spanColorMarkers;

    auto reapplyColor = [&]() {
        if (!enableColor) {
            return;
        }
        if (colorStack.empty()) {
            output += "\x1b[0m";
        } else {
            output += ansiColorFromHex(colorStack.back());
        }
    };

    for (size_t i = 0; i < source.size();) {
        if (source[i] == '[') {
            const size_t end = source.find(']', i);
            if (end == std::string::npos) {
                appendVisibleText(output, source.substr(i));
                break;
            }

            const std::string tag = source.substr(i + 1, end - i - 1);
            const std::string lowerTag = toLowerAscii(tag);

            if (lowerTag.rfind("color=#", 0) == 0) {
                colorStack.push_back(tag.substr(6));
                reapplyColor();
            } else if (lowerTag == "/color") {
                if (!colorStack.empty()) {
                    colorStack.pop_back();
                }
                reapplyColor();
            }

            i = end + 1;
            continue;
        }

        if (source.compare(i, 5, "<span") == 0) {
            const size_t end = source.find('>', i);
            if (end == std::string::npos) {
                break;
            }

            bool pushed = false;
            const std::string lowerSpan = toLowerAscii(source.substr(i, end - i + 1));
            const size_t colorPos = lowerSpan.find("color:#");
            if (colorPos != std::string::npos && colorPos + 13 <= lowerSpan.size()) {
                colorStack.push_back(lowerSpan.substr(colorPos + 6, 7));
                pushed = true;
                reapplyColor();
            }
            spanColorMarkers.push_back(pushed);
            i = end + 1;
            continue;
        }

        if (source.compare(i, 7, "</span>") == 0) {
            if (!spanColorMarkers.empty()) {
                if (spanColorMarkers.back() && !colorStack.empty()) {
                    colorStack.pop_back();
                    reapplyColor();
                }
                spanColorMarkers.pop_back();
            }
            i += 7;
            continue;
        }

        const size_t nextTag = source.find_first_of("[<", i);
        appendVisibleText(output, source.substr(i, nextTag == std::string::npos ? std::string::npos : nextTag - i));
        if (nextTag == std::string::npos) {
            break;
        }
        i = nextTag;
    }

    if (enableColor) {
        output += "\x1b[0m";
    }
    return output;
}

void printErrorLine(const std::string& message) {
    const std::string lowerMessage = toLowerAscii(message);
    std::string smooshMessage = "Smoosh is confused, but still trying to help.";
    std::string hintMessage = "Smoosh says: run `smushc help` for command usage, then check your path, args, or code around the location above.";

    if (containsAny(lowerMessage, {
            "expected expression",
            "unexpected token",
            "parse error",
            "failed to parse",
            "invalid http",
            "missing request line",
            "missing status line"
        })) {
        smooshMessage = "Smoosh can't parse this yet.";
        hintMessage = "Smoosh says: check the syntax near the location above. A missing token, separator, or malformed expression is usually the culprit.";
    } else if (containsAny(lowerMessage, {
                   "expected '",
                   "type '",
                   "has type",
                   "expects a ",
                   "cannot assign",
                   "undefined symbol",
                   "missing_main",
                   "initializer for",
                   "return type mismatch",
                   "future<",
                   "arity",
                   "generic",
                   "interface",
                   "override",
                   "bound"
               })) {
        smooshMessage = "Smoosh is having type thoughts.";
        hintMessage = "Smoosh says: your declarations and values disagree somewhere. Compare the expected type or signature with what the code is actually producing.";
    } else if (containsAny(lowerMessage, {
                   "runtime",
                   "list index out of bounds",
                   "attempted to call a non-function value",
                   "uncaught throw",
                   "tripped",
                   "failed to open file",
                   "unknown member",
                   "unknown assignment target",
                   "cannot open",
                   "out of bounds"
               })) {
        smooshMessage = "Smoosh tripped at runtime.";
        hintMessage = "Smoosh says: the program got through checking, then something real went wrong while it was running. Inspect the data and control flow near the reported location.";
    }

    std::cerr << colorize("smoosh", kSmoosh, g_theme.stderrColor) << " "
              << colorize(smooshMessage, kAccentDim, g_theme.stderrColor) << '\n';
    std::cerr << colorize("error", kAccent, g_theme.stderrColor) << "  "
              << colorize(message, kText, g_theme.stderrColor) << '\n';
    std::cerr << colorize("hint ", kAccentDim, g_theme.stderrColor)
              << colorize(hintMessage, kMuted, g_theme.stderrColor)
              << '\n';
}

void printInfoLine(const std::string& message) {
    std::cerr << colorize("info ", kAccentSoft, g_theme.stderrColor)
              << colorize(message, kText, g_theme.stderrColor) << '\n';
}

void printBuildSuccessLine(const std::string& artifactDescription) {
    std::cerr << colorize("smoosh", kSmoosh, g_theme.stderrColor) << " "
              << colorize("Smoosh approved this build.", kAccentDim, g_theme.stderrColor) << '\n';
    std::cerr << colorize("info ", kAccentSoft, g_theme.stderrColor)
              << colorize(artifactDescription, kText, g_theme.stderrColor) << '\n';
}

void printCommandLine(const std::string& usage, const std::string& description) {
    const std::string padded = "  " + usage;
    std::cerr << colorize(padded, kText, g_theme.stderrColor);
    if (!description.empty()) {
        std::cerr << "  " << colorize(description, kAccentSoft, g_theme.stderrColor);
    }
    std::cerr << '\n';
}

void printSplash() {
    std::string splashSource(
        reinterpret_cast<const char*>(smush::assets::kSplashBBCodeBytes),
        smush::assets::kSplashBBCodeSize);
    if (splashSource.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        splashSource.erase(0, 3);
    }
    std::cerr << renderBBCode(splashSource, g_theme.stderrColor);
    std::cerr << '\n';
    std::cerr << colorize("Crush complexity.", kText, g_theme.stderrColor) << '\n';
    std::cerr << colorize("Run ", kMuted, g_theme.stderrColor)
              << colorize("smushc help", kAccentSoft, g_theme.stderrColor)
              << colorize(" to see available commands with Smoosh.", kMuted, g_theme.stderrColor) << '\n';
    std::cerr << colorize("Go to ", kMuted, g_theme.stderrColor)
              << colorize("https://smush.github.io", kWarn, g_theme.stderrColor)
              << colorize(" to take a look at the docs.", kMuted, g_theme.stderrColor) << '\n';
}


void printErrors(const std::vector<std::string>& errors) {
    for (const auto& error : errors) {
        printErrorLine(error);
    }
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool startsWithWord(const std::string& value, const std::string& word) {
    if (!startsWith(value, word)) {
        return false;
    }
    if (value.size() == word.size()) {
        return true;
    }
    return std::isspace(static_cast<unsigned char>(value[word.size()])) != 0;
}

bool containsAssignmentOperator(const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '=') {
            continue;
        }
        const char prev = i > 0 ? value[i - 1] : '\0';
        const char next = i + 1 < value.size() ? value[i + 1] : '\0';
        if (prev == '=' || prev == '!' || prev == '<' || prev == '>' || prev == ':' ||
            next == '=' || next == '>') {
            continue;
        }
        return true;
    }
    return false;
}

bool isTopLevelDeclarationLine(const std::string& line) {
    return startsWithWord(line, "fn") ||
           startsWithWord(line, "async fn") ||
           startsWithWord(line, "class") ||
           startsWithWord(line, "abstract class") ||
           startsWithWord(line, "data") ||
           startsWithWord(line, "interface") ||
           startsWithWord(line, "type") ||
           startsWithWord(line, "import");
}

bool isVariableDeclarationLine(const std::string& line) {
    return startsWithWord(line, "let") ||
           startsWithWord(line, "var") ||
           startsWithWord(line, "const");
}

struct ReplVariableDecl {
    std::string name;
    std::string declaredType;
    std::string initializer;
};

bool parseReplVariableDeclaration(const std::string& line, ReplVariableDecl& decl) {
    std::string working = trim(line);
    if (!working.empty() && working.back() == ';') {
        working.pop_back();
        working = trim(working);
    }

    size_t cursor = 0;
    while (cursor < working.size() && std::isalpha(static_cast<unsigned char>(working[cursor])) != 0) {
        ++cursor;
    }
    while (cursor < working.size() && std::isspace(static_cast<unsigned char>(working[cursor])) != 0) {
        ++cursor;
    }

    const size_t nameStart = cursor;
    if (nameStart >= working.size() ||
        !(std::isalpha(static_cast<unsigned char>(working[nameStart])) != 0 || working[nameStart] == '_')) {
        return false;
    }
    ++cursor;
    while (cursor < working.size()) {
        const unsigned char ch = static_cast<unsigned char>(working[cursor]);
        if (std::isalnum(ch) == 0 && working[cursor] != '_') {
            break;
        }
        ++cursor;
    }
    decl.name = working.substr(nameStart, cursor - nameStart);

    while (cursor < working.size() && std::isspace(static_cast<unsigned char>(working[cursor])) != 0) {
        ++cursor;
    }

    decl.declaredType.clear();
    if (cursor < working.size() && working[cursor] == ':') {
        ++cursor;
        const size_t typeStart = cursor;
        int angleDepth = 0;
        int parenDepth = 0;
        while (cursor < working.size()) {
            const char ch = working[cursor];
            if (ch == '<') {
                ++angleDepth;
            } else if (ch == '>') {
                angleDepth = std::max(0, angleDepth - 1);
            } else if (ch == '(') {
                ++parenDepth;
            } else if (ch == ')') {
                parenDepth = std::max(0, parenDepth - 1);
            } else if (ch == '=' && angleDepth == 0 && parenDepth == 0) {
                break;
            }
            ++cursor;
        }
        decl.declaredType = trim(working.substr(typeStart, cursor - typeStart));
    }

    while (cursor < working.size() && std::isspace(static_cast<unsigned char>(working[cursor])) != 0) {
        ++cursor;
    }

    decl.initializer.clear();
    if (cursor < working.size() && working[cursor] == '=') {
        ++cursor;
        decl.initializer = trim(working.substr(cursor));
    }

    return !decl.name.empty();
}

bool isLikelyStatementLine(const std::string& line) {
    return endsWith(line, ";") ||
           startsWithWord(line, "if") ||
           startsWithWord(line, "while") ||
           startsWithWord(line, "for") ||
           startsWithWord(line, "return") ||
           startsWithWord(line, "throw") ||
           startsWithWord(line, "try") ||
           startsWithWord(line, "match") ||
           startsWithWord(line, "break") ||
           startsWithWord(line, "continue") ||
           containsAssignmentOperator(line);
}

std::vector<std::string> collectArgs(char** argv, int start, int argc) {
    std::vector<std::string> args;
    for (int i = start; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

std::string replaceExtension(const std::string& path, const std::string& extension) {
    std::filesystem::path output(path);
    output.replace_extension(extension);
    return output.string();
}

bool loadBundle(const std::string& inputPath, smush::SourceBundle& bundle, std::vector<std::string>& errors) {
    if (endsWith(inputPath, ".smpk")) {
        return smush::PackageFile::read(inputPath, bundle, errors);
    }
    return smush::ProjectLoader::loadBundleFromFilesystem(inputPath, bundle, errors);
}

bool loadBytecodeModule(const std::string& inputPath,
                        std::shared_ptr<smush::BytecodeModule>& module,
                        std::vector<std::string>& errors) {
    if (!endsWith(inputPath, ".smbc")) {
        return false;
    }
    smush::BytecodeModule loaded;
    if (!smush::BytecodePackageFile::read(inputPath, loaded, errors)) {
        return false;
    }
    module = std::make_shared<smush::BytecodeModule>(std::move(loaded));
    return true;
}

bool lowerBundleToBytecode(const smush::SourceBundle& bundle,
                           std::shared_ptr<smush::BytecodeModule>& module,
                           std::vector<std::string>& errors) {
    auto program = smush::ProjectLoader::loadProgramFromBundle(bundle, errors);
    if (!errors.empty()) {
        return false;
    }

    smush::SemanticAnalyzer semantic;
    if (!semantic.analyze(program.get())) {
        errors.insert(errors.end(), semantic.getErrors().begin(), semantic.getErrors().end());
        return false;
    }

    smush::IRLowerer irLowerer;
    auto ir = irLowerer.lower(program.get());
    if (!irLowerer.getErrors().empty()) {
        errors.insert(errors.end(), irLowerer.getErrors().begin(), irLowerer.getErrors().end());
        return false;
    }

    smush::IRValidator irValidator;
    if (!irValidator.validate(ir.get())) {
        errors.insert(errors.end(), irValidator.getErrors().begin(), irValidator.getErrors().end());
        return false;
    }

    smush::IROptimizer optimizer;
    ir = optimizer.optimize(std::move(ir));

    smush::IRValidator optimizedValidator;
    if (!optimizedValidator.validate(ir.get())) {
        errors.insert(errors.end(), optimizedValidator.getErrors().begin(), optimizedValidator.getErrors().end());
        return false;
    }

    smush::BytecodeLowerer bcLowerer;
    module = bcLowerer.lower(ir.get());
    if (!bcLowerer.getErrors().empty()) {
        errors.insert(errors.end(), bcLowerer.getErrors().begin(), bcLowerer.getErrors().end());
        return false;
    }

    return true;
}

bool semanticCheckBundle(const smush::SourceBundle& bundle, std::vector<std::string>& errors) {
    auto program = smush::ProjectLoader::loadProgramFromBundle(bundle, errors);
    if (!errors.empty()) {
        return false;
    }

    smush::SemanticAnalyzer semantic;
    if (!semantic.analyze(program.get())) {
        errors.insert(errors.end(), semantic.getErrors().begin(), semantic.getErrors().end());
        return false;
    }

    return true;
}

int dumpIR(const smush::SourceBundle& bundle) {
    std::vector<std::string> errors;
    auto program = smush::ProjectLoader::loadProgramFromBundle(bundle, errors);
    if (!errors.empty()) {
        printErrors(errors);
        return 1;
    }

    smush::SemanticAnalyzer semantic;
    if (!semantic.analyze(program.get())) {
        printErrors(semantic.getErrors());
        return 1;
    }

    smush::IRLowerer lowerer;
    auto ir = lowerer.lower(program.get());
    if (!lowerer.getErrors().empty()) {
        printErrors(lowerer.getErrors());
        return 1;
    }

    smush::IRValidator validator;
    if (!validator.validate(ir.get())) {
        printErrors(validator.getErrors());
        return 1;
    }

    smush::IROptimizer optimizer;
    ir = optimizer.optimize(std::move(ir));

    smush::IRValidator optimizedValidator;
    if (!optimizedValidator.validate(ir.get())) {
        printErrors(optimizedValidator.getErrors());
        return 1;
    }

    smush::IRPrinter printer;
    std::cout << printer.print(ir.get());
    return 0;
}

int dumpBytecode(const smush::SourceBundle& bundle) {
    std::vector<std::string> errors;
    auto program = smush::ProjectLoader::loadProgramFromBundle(bundle, errors);
    if (!errors.empty()) {
        printErrors(errors);
        return 1;
    }

    smush::SemanticAnalyzer semantic;
    if (!semantic.analyze(program.get())) {
        printErrors(semantic.getErrors());
        return 1;
    }

    smush::IRLowerer irLowerer;
    auto ir = irLowerer.lower(program.get());
    if (!irLowerer.getErrors().empty()) {
        printErrors(irLowerer.getErrors());
        return 1;
    }

    smush::IRValidator irValidator;
    if (!irValidator.validate(ir.get())) {
        printErrors(irValidator.getErrors());
        return 1;
    }

    smush::IROptimizer optimizer;
    ir = optimizer.optimize(std::move(ir));

    smush::IRValidator optimizedValidator;
    if (!optimizedValidator.validate(ir.get())) {
        printErrors(optimizedValidator.getErrors());
        return 1;
    }

    smush::BytecodeLowerer bcLowerer;
    auto module = bcLowerer.lower(ir.get());
    if (!bcLowerer.getErrors().empty()) {
        printErrors(bcLowerer.getErrors());
        return 1;
    }

    std::cout << smush::BytecodeLayout::formatModule(*module);
    return 0;
}

int dumpBytecodeModule(const std::shared_ptr<smush::BytecodeModule>& module) {
    std::cout << smush::BytecodeLayout::formatModule(*module);
    return 0;
}

int runBytecode(const smush::SourceBundle& bundle, bool checkOnly) {
    std::vector<std::string> errors;
    std::shared_ptr<smush::BytecodeModule> module;
    if (!lowerBundleToBytecode(bundle, module, errors)) {
        printErrors(errors);
        return 1;
    }

    if (checkOnly) {
        return 0;
    }

    smush::BytecodeVM vm;
    vm.run(module);
    if (!vm.getErrors().empty()) {
        printErrors(vm.getErrors());
        return 1;
    }
    return 0;
}

int runBytecodeModule(const std::shared_ptr<smush::BytecodeModule>& module, bool checkOnly) {
    if (checkOnly) {
        return 0;
    }

    smush::BytecodeVM vm;
    vm.run(module);
    if (!vm.getErrors().empty()) {
        printErrors(vm.getErrors());
        return 1;
    }
    return 0;
}

int runRepl() {
    struct ReplState {
        std::vector<std::string> imports;
        std::vector<std::string> declarations;
        std::vector<std::string> globalDeclarations;
        std::unordered_set<std::string> globals;
        size_t cellCounter = 0;
    };

    auto makeBundle = [](const ReplState& state, const std::string& cellBody) {
        smush::SourceBundle bundle;
        bundle.entryPath = "<repl>";

        std::string source;
        for (const auto& importLine : state.imports) {
            source += importLine + "\n";
        }
        if (!state.imports.empty()) {
            source += "\n";
        }
        for (const auto& globalDecl : state.globalDeclarations) {
            source += globalDecl + "\n";
        }
        if (!state.globalDeclarations.empty()) {
            source += "\n";
        }
        for (const auto& declaration : state.declarations) {
            source += declaration + "\n\n";
        }

        source += "async fn __repl_cell_" + std::to_string(state.cellCounter) + "(): Void => {\n";
        if (!cellBody.empty()) {
            source += cellBody;
            if (!endsWith(cellBody, "\n")) {
                source += "\n";
            }
        }
        source += "}\n\n";
        source += "async fn main(): Void => {\n";
        source += "    await __repl_cell_" + std::to_string(state.cellCounter) + "()\n";
        source += "}\n";

        bundle.sources.emplace(bundle.entryPath, std::move(source));
        return bundle;
    };

    const bool interactive = streamIsTerminal(stdin) && streamIsTerminal(stderr);
    ReplState state;
    auto vm = std::make_unique<smush::BytecodeVM>();
    smush::BytecodeVM::setScriptArgs({});

    if (interactive) {
        std::cerr << colorize("smush repl", kAccent, g_theme.stderrColor) << "  "
                  << colorize("type .smush code, `:help` for commands.", kMuted, g_theme.stderrColor) << '\n';
    }

    std::string line;
    while (true) {
        if (interactive) {
            std::cerr << colorize("smush> ", kAccentSoft, g_theme.stderrColor);
            std::cerr.flush();
        }

        if (!std::getline(std::cin, line)) {
            if (interactive) {
                std::cerr << '\n';
            }
            break;
        }

        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }

        if (trimmed == ":quit" || trimmed == ":exit") {
            break;
        }
        if (trimmed == ":help") {
            std::cerr << colorize(":help", kAccentSoft, g_theme.stderrColor) << "  "
                      << colorize("show repl commands", kMuted, g_theme.stderrColor) << '\n'
                      << colorize(":reset", kAccentSoft, g_theme.stderrColor) << "  "
                      << colorize("clear repl state", kMuted, g_theme.stderrColor) << '\n'
                      << colorize(":quit", kAccentSoft, g_theme.stderrColor) << "  "
                      << colorize("leave the repl", kMuted, g_theme.stderrColor) << '\n';
            continue;
        }
        if (trimmed == ":reset") {
            state = ReplState{};
            vm = std::make_unique<smush::BytecodeVM>();
            smush::BytecodeVM::setScriptArgs({});
            if (interactive) {
                printInfoLine("repl state cleared");
            }
            continue;
        }

        std::string cellBody;
        bool persistChange = false;
        bool addedGlobalThisTurn = false;

        if (startsWithWord(trimmed, "import")) {
            state.imports.push_back(trimmed);
            persistChange = true;
        } else if (isTopLevelDeclarationLine(trimmed)) {
            state.declarations.push_back(trimmed);
            persistChange = true;
        } else if (isVariableDeclarationLine(trimmed)) {
            ReplVariableDecl decl;
            if (!parseReplVariableDeclaration(trimmed, decl)) {
                printErrorLine("failed to parse repl variable declaration");
                continue;
            }
            addedGlobalThisTurn = state.globals.insert(decl.name).second;
            if (addedGlobalThisTurn) {
                const std::string typeName = decl.declaredType.empty() ? "Any" : decl.declaredType;
                state.globalDeclarations.push_back("var " + decl.name + ": " + typeName + ";");
            }
            if (!decl.initializer.empty()) {
                cellBody = "    " + decl.name + " = " + decl.initializer + "\n";
            }
            persistChange = true;
        } else if (isLikelyStatementLine(trimmed)) {
            cellBody = "    " + trimmed + "\n";
        } else {
            cellBody = "    println(" + trimmed + ")\n";
        }

        ++state.cellCounter;
        smush::SourceBundle bundle = makeBundle(state, cellBody);
        std::shared_ptr<smush::BytecodeModule> module;
        std::vector<std::string> errors;
        if (!lowerBundleToBytecode(bundle, module, errors)) {
            printErrors(errors);
            if (persistChange) {
                if (startsWithWord(trimmed, "import")) {
                    state.imports.pop_back();
                } else if (isTopLevelDeclarationLine(trimmed)) {
                    state.declarations.pop_back();
                } else if (isVariableDeclarationLine(trimmed)) {
                    if (addedGlobalThisTurn && !state.globalDeclarations.empty()) {
                        ReplVariableDecl decl;
                        if (parseReplVariableDeclaration(trimmed, decl) && !decl.name.empty()) {
                            state.globals.erase(decl.name);
                            state.globalDeclarations.pop_back();
                        }
                    }
                }
                --state.cellCounter;
            }
            continue;
        }

        vm->runInteractive(module);
        if (!vm->getErrors().empty()) {
            printErrors(vm->getErrors());
            continue;
        }
    }

    return 0;
}

int buildPackage(const std::string& inputPath, const std::string& outputPath) {
    std::vector<std::string> errors;
    smush::SourceBundle bundle;
    if (!loadBundle(inputPath, bundle, errors)) {
        printErrors(errors);
        return 1;
    }

    auto program = smush::ProjectLoader::loadProgramFromBundle(bundle, errors);
    if (!program || !errors.empty()) {
        printErrors(errors);
        return 1;
    }

    smush::SemanticAnalyzer semantic;
    if (!semantic.analyze(program.get())) {
        printErrors(semantic.getErrors());
        return 1;
    }

    if (!smush::PackageFile::write(outputPath, bundle, errors)) {
        printErrors(errors);
        return 1;
    }
    printBuildSuccessLine("built source package: " + outputPath);
    return 0;
}

int buildBytecodePackage(const std::string& inputPath, const std::string& outputPath) {
    std::vector<std::string> errors;
    smush::SourceBundle bundle;
    if (!loadBundle(inputPath, bundle, errors)) {
        printErrors(errors);
        return 1;
    }

    std::shared_ptr<smush::BytecodeModule> module;
    if (!lowerBundleToBytecode(bundle, module, errors)) {
        printErrors(errors);
        return 1;
    }

    if (!smush::BytecodePackageFile::write(outputPath, *module, errors)) {
        printErrors(errors);
        return 1;
    }
    printBuildSuccessLine("built bytecode package: " + outputPath);
    return 0;
}

int runInput(const std::string& inputPath, bool checkOnly) {
    std::vector<std::string> errors;
    std::shared_ptr<smush::BytecodeModule> module;
    if (loadBytecodeModule(inputPath, module, errors)) {
        return runBytecodeModule(module, checkOnly);
    }
    if (!errors.empty()) {
        printErrors(errors);
        return 1;
    }

    smush::SourceBundle bundle;
    if (!loadBundle(inputPath, bundle, errors)) {
        printErrors(errors);
        return 1;
    }
    if (checkOnly) {
        if (!semanticCheckBundle(bundle, errors)) {
            printErrors(errors);
            return 1;
        }
        return 0;
    }
    return runBytecode(bundle, checkOnly);
}

enum class BuildMode {
    Bytecode,
    SourcePackage,
};

enum class DumpMode {
    Bytecode,
    IR,
};

int buildCommand(int argc, char** argv) {
    if (argc < 3) {
        printUsage();
        return 1;
    }

    const std::string inputPath = argv[2];
    BuildMode mode = BuildMode::Bytecode;
    bool explicitMode = false;
    std::string outputPath;

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--source") {
            mode = BuildMode::SourcePackage;
            explicitMode = true;
            continue;
        }
        if (arg == "--bytecode") {
            mode = BuildMode::Bytecode;
            explicitMode = true;
            continue;
        }
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                printErrorLine("missing output path after " + arg);
                return 1;
            }
            outputPath = argv[++i];
            continue;
        }
        if (outputPath.empty()) {
            outputPath = arg;
            continue;
        }
        printUsage();
        return 1;
    }

    if (outputPath.empty()) {
        outputPath = replaceExtension(inputPath, mode == BuildMode::Bytecode ? ".smbc" : ".smpk");
    } else if (!explicitMode) {
        if (endsWith(outputPath, ".smpk")) {
            mode = BuildMode::SourcePackage;
        } else if (endsWith(outputPath, ".smbc")) {
            mode = BuildMode::Bytecode;
        }
    }

    if (mode == BuildMode::SourcePackage) {
        if (endsWith(inputPath, ".smbc")) {
            printErrorLine("cannot build a source package from a bytecode module");
            return 1;
        }
        return buildPackage(inputPath, outputPath);
    }

    return buildBytecodePackage(inputPath, outputPath);
}

int dumpCommand(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        printUsage();
        return 1;
    }

    const std::string inputPath = argv[2];
    DumpMode mode = DumpMode::Bytecode;
    if (argc == 4) {
        const std::string option = argv[3];
        if (option == "--ir") {
            mode = DumpMode::IR;
        } else if (option == "--bytecode" || option == "--bc") {
            mode = DumpMode::Bytecode;
        } else {
            printUsage();
            return 1;
        }
    }

    if (mode == DumpMode::IR) {
        if (endsWith(inputPath, ".smbc")) {
            printErrorLine("cannot dump IR from a serialized bytecode module");
            return 1;
        }
        smush::SourceBundle bundle;
        std::vector<std::string> errors;
        if (!loadBundle(inputPath, bundle, errors)) {
            printErrors(errors);
            return 1;
        }
        return dumpIR(bundle);
    }

    std::shared_ptr<smush::BytecodeModule> module;
    smush::SourceBundle bundle;
    std::vector<std::string> errors;
    if (loadBytecodeModule(inputPath, module, errors)) {
        return dumpBytecodeModule(module);
    }
    if (!errors.empty()) {
        printErrors(errors);
        return 1;
    }
    if (!loadBundle(inputPath, bundle, errors)) {
        printErrors(errors);
        return 1;
    }
    return dumpBytecode(bundle);
}

void printUsage() {
    std::cerr
        << colorize("smushc", kAccent, g_theme.stderrColor) << "  "
        << colorize("Crush complexity.", kText, g_theme.stderrColor) << "\n\n"
        << colorize("Usage", kAccentSoft, g_theme.stderrColor) << '\n'
        ;
    printCommandLine("smushc <input.smush|input.smpk|input.smbc>", "run a script, source package, or bytecode package");
    printCommandLine("smushc check <input.smush|input.smpk|input.smbc>", "validate a program without executing it");
    printCommandLine("smushc repl", "start an interactive .smush session");
    printCommandLine("smushc build <input.smush|input.smpk> [-o output.smbc]", "build a bytecode package");
    printCommandLine("smushc build <input.smush|input.smpk> --source [-o output.smpk]", "build a source package");
    printCommandLine("smushc dump <input.smush|input.smpk|input.smbc> [--ir|--bytecode]", "inspect lowered IR or bytecode");
    printCommandLine("smushc help", "show command help");
    std::cerr << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    initializeConsoleTheme();

    std::vector<std::string> processArgs;
    processArgs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        processArgs.emplace_back(argv[i]);
    }
    smush::BytecodeVM::setProcessArgs(std::move(processArgs));

    if (argc < 2) {
        printSplash();
        return 0;
    }

    try {
        const std::string command = argv[1];

        if (command == "--help" || command == "-h" || command == "help") {
            printUsage();
            return 0;
        }

        if (command == "run") {
            if (argc < 3) {
                printUsage();
                return 1;
            }
            smush::BytecodeVM::setScriptArgs(collectArgs(argv, 3, argc));
            return runInput(argv[2], false);
        }

        if (command == "check") {
            if (argc != 3) {
                printUsage();
                return 1;
            }
            smush::BytecodeVM::setScriptArgs({});
            return runInput(argv[2], true);
        }

        if (command == "repl") {
            if (argc != 2) {
                printUsage();
                return 1;
            }
            return runRepl();
        }

        if (command == "build") {
            return buildCommand(argc, argv);
        }

        if (command == "build-bc") {
            if (argc < 3 || argc > 4) {
                printUsage();
                return 1;
            }
            const std::string output = argc == 4 ? argv[3] : replaceExtension(argv[2], ".smbc");
            return buildBytecodePackage(argv[2], output);
        }

        if (command == "run-bc" || command == "check-bc") {
            if (argc < 3 || (command == "check-bc" && argc != 3)) {
                printUsage();
                return 1;
            }
            smush::BytecodeVM::setScriptArgs(command == "run-bc" ? collectArgs(argv, 3, argc) : std::vector<std::string>{});
            std::shared_ptr<smush::BytecodeModule> module;
            smush::SourceBundle bundle;
            std::vector<std::string> errors;
            if (loadBytecodeModule(argv[2], module, errors)) {
                return runBytecodeModule(module, command == "check-bc");
            }
            if (!errors.empty()) {
                printErrors(errors);
                return 1;
            }
            if (!loadBundle(argv[2], bundle, errors)) {
                printErrors(errors);
                return 1;
            }
            return runBytecode(bundle, command == "check-bc");
        }

        if (command == "dump") {
            return dumpCommand(argc, argv);
        }

        if (command == "dump-ir") {
            if (argc != 3) {
                printUsage();
                return 1;
            }
            smush::SourceBundle bundle;
            std::vector<std::string> errors;
            if (!loadBundle(argv[2], bundle, errors)) {
                printErrors(errors);
                return 1;
            }
            return dumpIR(bundle);
        }

        if (command == "dump-bc") {
            if (argc != 3) {
                printUsage();
                return 1;
            }
            return dumpCommand(argc, argv);
        }

        const std::string input = argv[1];
        const bool checkOnly = argc >= 3 && std::string(argv[2]) == "--check";
        if (checkOnly && argc != 3) {
            printUsage();
            return 1;
        }
        smush::BytecodeVM::setScriptArgs(checkOnly ? std::vector<std::string>{} : collectArgs(argv, 2, argc));
        return runInput(input, checkOnly);
    } catch (const std::exception& ex) {
        printErrorLine(ex.what());
        return 1;
    }
}
