#include "runtime.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace smush {

namespace {

void retainObject(Object* object) {
    if (object && object->heap) {
        object->heap->retain(object);
    }
}

void releaseObject(Object* object) {
    if (object && object->heap) {
        object->heap->release(object);
    }
}

}  // namespace

Value::Value(const Value& other)
    : kind(other.kind),
      boolValue(other.boolValue),
      intValue(other.intValue),
      floatValue(other.floatValue),
      stringValue(other.stringValue),
      objectValue(other.objectValue) {
    retainObject(objectValue);
}

Value::Value(Value&& other) noexcept
    : kind(other.kind),
      boolValue(other.boolValue),
      intValue(other.intValue),
      floatValue(other.floatValue),
      stringValue(std::move(other.stringValue)),
      objectValue(other.objectValue) {
    other.kind = Kind::Null;
    other.objectValue = nullptr;
    other.boolValue = false;
    other.intValue = 0;
    other.floatValue = 0.0;
}

Value& Value::operator=(const Value& other) {
    if (this == &other) {
        return *this;
    }
    reset();
    kind = other.kind;
    boolValue = other.boolValue;
    intValue = other.intValue;
    floatValue = other.floatValue;
    stringValue = other.stringValue;
    objectValue = other.objectValue;
    retainObject(objectValue);
    return *this;
}

Value& Value::operator=(Value&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    kind = other.kind;
    boolValue = other.boolValue;
    intValue = other.intValue;
    floatValue = other.floatValue;
    stringValue = std::move(other.stringValue);
    objectValue = other.objectValue;
    other.kind = Kind::Null;
    other.objectValue = nullptr;
    other.boolValue = false;
    other.intValue = 0;
    other.floatValue = 0.0;
    return *this;
}

Value::~Value() {
    reset();
}

Value Value::null() {
    return {};
}

Value Value::boolean(bool value) {
    Value result;
    result.kind = Kind::Bool;
    result.boolValue = value;
    return result;
}

Value Value::integer(int64_t value) {
    Value result;
    result.kind = Kind::Int;
    result.intValue = value;
    return result;
}

Value Value::floating(double value) {
    Value result;
    result.kind = Kind::Float;
    result.floatValue = value;
    return result;
}

Value Value::string(std::string value) {
    Value result;
    result.kind = Kind::String;
    result.stringValue = std::move(value);
    return result;
}

Value Value::object(Object* value) {
    Value result;
    result.kind = Kind::Object;
    result.objectValue = value;
    retainObject(result.objectValue);
    return result;
}

void Value::reset() {
    releaseObject(objectValue);
    kind = Kind::Null;
    boolValue = false;
    intValue = 0;
    floatValue = 0.0;
    stringValue.clear();
    objectValue = nullptr;
}

ListObject::ListObject() : Object(ObjectKind::List) {}

ListObject::ListObject(std::vector<Value> values)
    : Object(ObjectKind::List), elements(std::move(values)) {}

std::string ListObject::describe() const {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << toString(elements[i]);
    }
    oss << "]";
    return oss.str();
}

void ListObject::trace(const std::function<void(const Value&)>& visitor) const {
    for (const auto& value : elements) {
        visitor(value);
    }
}

TupleObject::TupleObject() : Object(ObjectKind::Tuple) {}

TupleObject::TupleObject(std::vector<Value> values)
    : Object(ObjectKind::Tuple), elements(std::move(values)) {}

std::string TupleObject::describe() const {
    std::ostringstream oss;
    oss << "(";
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << toString(elements[i]);
    }
    oss << ")";
    return oss.str();
}

void TupleObject::trace(const std::function<void(const Value&)>& visitor) const {
    for (const auto& value : elements) {
        visitor(value);
    }
}

MapObject::MapObject() : Object(ObjectKind::Map) {}

std::string MapObject::describe() const {
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << toString(entries[i].first) << ": " << toString(entries[i].second);
    }
    oss << "}";
    return oss.str();
}

void MapObject::trace(const std::function<void(const Value&)>& visitor) const {
    for (const auto& entry : entries) {
        visitor(entry.first);
        visitor(entry.second);
    }
}

SetObject::SetObject() : Object(ObjectKind::Set) {}

std::string SetObject::describe() const {
    std::ostringstream oss;
    oss << "#{";
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << toString(elements[i]);
    }
    oss << "}";
    return oss.str();
}

void SetObject::trace(const std::function<void(const Value&)>& visitor) const {
    for (const auto& value : elements) {
        visitor(value);
    }
}

RangeObject::RangeObject(int64_t from, int64_t to, bool includeEnd)
    : Object(ObjectKind::Range), start(from), end(to), inclusive(includeEnd) {}

std::string RangeObject::describe() const {
    return std::to_string(start) + (inclusive ? "..." : "..") + std::to_string(end);
}

void RangeObject::trace(const std::function<void(const Value&)>&) const {}

InstanceObject::InstanceObject(std::string name)
    : Object(ObjectKind::Instance), typeName(std::move(name)) {}

std::string InstanceObject::describe() const {
    return "<" + typeName + ">";
}

void InstanceObject::trace(const std::function<void(const Value&)>& visitor) const {
    for (const auto& entry : fields) {
        visitor(entry.second);
    }
}

FutureObject::FutureObject() : Object(ObjectKind::Future) {}

FutureObject::FutureObject(Value value)
    : Object(ObjectKind::Future), state(State::Resolved), resolved(std::move(value)) {}

std::string FutureObject::describe() const {
    switch (state) {
        case State::Pending:
            return "<future pending>";
        case State::Resolved:
            return "<future resolved>";
        case State::Failed:
            return "<future failed>";
    }
    return "<future>";
}

void FutureObject::trace(const std::function<void(const Value&)>& visitor) const {
    visitor(resolved);
    visitor(failure);
}

BytesObject::BytesObject() : Object(ObjectKind::Bytes) {}

BytesObject::BytesObject(std::vector<uint8_t> bytes)
    : Object(ObjectKind::Bytes), data(std::move(bytes)) {}

std::string BytesObject::describe() const {
    return "<bytes len=" + std::to_string(data.size()) + ">";
}

void BytesObject::trace(const std::function<void(const Value&)>&) const {}

TextStreamObject::TextStreamObject(std::string streamPath, Mode streamMode)
    : Object(ObjectKind::TextStream),
      path(std::move(streamPath)),
      mode(streamMode) {}

std::string TextStreamObject::describe() const {
    const char* modeName = mode == Mode::Reader ? "reader" : (mode == Mode::Writer ? "writer" : "appender");
    return std::string("<text-stream ") + modeName + " " + path + (closed ? " closed>" : " open>");
}

void TextStreamObject::trace(const std::function<void(const Value&)>&) const {}

TcpSocketObject::TcpSocketObject(intptr_t socketHandle)
    : Object(ObjectKind::TcpSocket), handle(socketHandle) {}

std::string TcpSocketObject::describe() const {
    return closed ? "<tcp-socket closed>" : "<tcp-socket open>";
}

void TcpSocketObject::trace(const std::function<void(const Value&)>&) const {}

TcpListenerObject::TcpListenerObject(intptr_t listenerHandle, int boundPort)
    : Object(ObjectKind::TcpListener), handle(listenerHandle), port(boundPort) {}

std::string TcpListenerObject::describe() const {
    return closed ? "<tcp-listener closed>" : "<tcp-listener port=" + std::to_string(port) + ">";
}

void TcpListenerObject::trace(const std::function<void(const Value&)>&) const {}

UiAppObject::UiAppObject(std::string appName, bool native)
    : Object(ObjectKind::UiApp), name(std::move(appName)), nativeBackend(native) {}

std::string UiAppObject::describe() const {
    return "<UiApp " + name + ">";
}

void UiAppObject::trace(const std::function<void(const Value&)>& visitor) const {
    for (const auto& window : windows) {
        visitor(window);
    }
}

UiWindowObject::UiWindowObject(std::string windowTitle, int windowWidth, int windowHeight)
    : Object(ObjectKind::UiWindow),
      title(std::move(windowTitle)),
      width(windowWidth),
      height(windowHeight) {}

std::string UiWindowObject::describe() const {
    return "<UiWindow " + title + ">";
}

void UiWindowObject::trace(const std::function<void(const Value&)>& visitor) const {
    visitor(app);
    visitor(content);
    visitor(onClose);
    visitor(theme);
}

UiViewObject::UiViewObject(UiViewKind kind)
    : Object(ObjectKind::UiView), viewKind(kind) {}

std::string UiViewObject::describe() const {
    switch (viewKind) {
        case UiViewKind::Label: return "<Label>";
        case UiViewKind::Button: return "<Button>";
        case UiViewKind::TextField: return "<TextField>";
        case UiViewKind::TextArea: return "<TextArea>";
        case UiViewKind::CheckBox: return "<CheckBox>";
        case UiViewKind::Row: return "<Row>";
        case UiViewKind::Column: return "<Column>";
        case UiViewKind::Grid: return "<Grid>";
    }
    return "<UiView>";
}

void UiViewObject::trace(const std::function<void(const Value&)>& visitor) const {
    visitor(onClick);
    visitor(onChange);
    for (const auto& child : children) {
        visitor(child);
    }
}

UiThemeObject::UiThemeObject(std::string themeName)
    : Object(ObjectKind::UiTheme), name(std::move(themeName)) {}

std::string UiThemeObject::describe() const {
    return "<UiTheme " + name + ">";
}

void UiThemeObject::trace(const std::function<void(const Value&)>&) const {}

BytecodeBoundMethodObject::BytecodeBoundMethodObject(Value target, std::vector<uint32_t> indices, std::string methodName, bool staticMethod)
    : Object(ObjectKind::BytecodeBoundMethod),
      receiver(std::move(target)),
      functionIndices(std::move(indices)),
      name(std::move(methodName)),
      isStatic(staticMethod) {}

std::string BytecodeBoundMethodObject::describe() const {
    return isStatic ? "<bc-static-method " + name + ">" : "<bc-bound-method " + name + ">";
}

void BytecodeBoundMethodObject::trace(const std::function<void(const Value&)>& visitor) const {
    if (!isStatic) {
        visitor(receiver);
    }
}

BytecodeClosureObject::BytecodeClosureObject(uint32_t index, std::string functionName, std::vector<Value> capturedValues)
    : Object(ObjectKind::BytecodeClosure),
      functionIndex(index),
      name(std::move(functionName)),
      captures(std::move(capturedValues)) {}

std::string BytecodeClosureObject::describe() const {
    return "<bc-closure " + name + ">";
}

void BytecodeClosureObject::trace(const std::function<void(const Value&)>& visitor) const {
    for (const auto& capture : captures) {
        visitor(capture);
    }
}


Environment::Environment(std::shared_ptr<Environment> parentScope) : parent(std::move(parentScope)) {}

void Environment::define(const std::string& name, const Value& value, bool mutableBinding) {
    bindings[name] = Binding{value, mutableBinding};
}

bool Environment::assign(const std::string& name, const Value& value) {
    auto it = bindings.find(name);
    if (it != bindings.end()) {
        if (!it->second.mutableBinding) {
            throw std::runtime_error("cannot assign to immutable binding '" + name + "'");
        }
        it->second.value = value;
        return true;
    }
    if (parent) {
        return parent->assign(name, value);
    }
    return false;
}

Value Environment::get(const std::string& name) const {
    auto it = bindings.find(name);
    if (it != bindings.end()) {
        return it->second.value;
    }
    if (parent) {
        return parent->get(name);
    }
    throw std::runtime_error("undefined symbol '" + name + "'");
}

bool Environment::existsInCurrentScope(const std::string& name) const {
    return bindings.find(name) != bindings.end();
}

void Environment::trace(const std::function<void(const Value&)>& visitor) const {
    for (const auto& binding : bindings) {
        visitor(binding.second.value);
    }
    if (parent) {
        parent->trace(visitor);
    }
}

Heap::~Heap() {
    sweeping = true;
    std::vector<Object*> remaining(objects.begin(), objects.end());
    objects.clear();
    for (Object* object : remaining) {
        ++stats.destroyedObjects;
        delete object;
    }
    stats.currentlyTrackedObjects = 0;
}

void Heap::retain(Object* object) {
    if (object) {
        ++object->refCount;
        ++stats.retainedReferences;
    }
}

void Heap::release(Object* object) {
    if (!object || object->refCount == 0) {
        return;
    }
    --object->refCount;
    ++stats.releasedReferences;
    if (object->refCount == 0 && !sweeping) {
        destroy(object);
    }
}

void Heap::collect(const std::vector<const Environment*>& roots) {
    ++stats.collections;
    trace("collect.begin roots=" + std::to_string(roots.size()) +
          " tracked=" + std::to_string(objects.size()));
    for (Object* object : objects) {
        object->marked = false;
    }

    for (const Environment* env : roots) {
        if (env) {
            env->trace([this](const Value& value) { markValue(value); });
        }
    }

    std::vector<Object*> unreachable;
    unreachable.reserve(objects.size());
    for (Object* object : objects) {
        if (!object->marked) {
            unreachable.push_back(object);
        }
    }

    sweeping = true;
    for (Object* object : unreachable) {
        objects.erase(object);
    }
    for (Object* object : unreachable) {
        ++stats.destroyedObjects;
        delete object;
    }
    sweeping = false;
    stats.lastCollectionReclaimed = unreachable.size();
    allocatedSinceCollection = 0;
    stats.currentlyTrackedObjects = objects.size();
    trace("collect.end reclaimed=" + std::to_string(unreachable.size()) +
          " tracked=" + std::to_string(stats.currentlyTrackedObjects));
}

size_t Heap::size() const {
    return objects.size();
}

const RuntimeMetrics& Heap::metrics() const {
    return stats;
}

RuntimeMetrics& Heap::metricsMutable() {
    return stats;
}

const RuntimeOptions& Heap::options() const {
    return opts;
}

void Heap::setCollectionThreshold(size_t threshold) {
    opts.collectionThreshold = threshold == 0 ? 1 : threshold;
}

void Heap::setTraceCollections(bool enabled) {
    opts.traceCollections = enabled;
}

void Heap::setTraceAllocations(bool enabled) {
    opts.traceAllocations = enabled;
}

void Heap::setTraceExecution(bool enabled) {
    opts.traceExecution = enabled;
}

bool Heap::shouldCollect() const {
    return allocatedSinceCollection >= opts.collectionThreshold;
}

void Heap::destroy(Object* object) {
    if (!object) {
        return;
    }
    auto it = objects.find(object);
    if (it == objects.end()) {
        return;
    }
    objects.erase(it);
    ++stats.destroyedObjects;
    stats.currentlyTrackedObjects = objects.size();
    trace("destroy " + object->describe() + " tracked=" + std::to_string(stats.currentlyTrackedObjects));
    delete object;
}

void Heap::markValue(const Value& value) {
    if (value.kind == Value::Kind::Object) {
        markObject(value.objectValue);
    }
}

void Heap::markObject(Object* object) {
    if (!object || object->marked) {
        return;
    }
    object->marked = true;
    object->trace([this](const Value& value) { markValue(value); });
}

void Heap::trace(const std::string& message) const {
    if ((opts.traceCollections && message.rfind("collect", 0) == 0) ||
        (opts.traceAllocations && (message.rfind("alloc", 0) == 0 || message.rfind("destroy", 0) == 0)) ||
        (opts.traceExecution && message.rfind("exec", 0) == 0)) {
        std::cerr << "[seolc] " << message << '\n';
    }
}

bool isTruthy(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Null:
            return false;
        case Value::Kind::Bool:
            return value.boolValue;
        case Value::Kind::Int:
            return value.intValue != 0;
        case Value::Kind::Float:
            return std::fabs(value.floatValue) > 0.0;
        case Value::Kind::String:
            return !value.stringValue.empty();
        case Value::Kind::Object:
            return true;
    }
    return false;
}

bool valuesEqual(const Value& left, const Value& right) {
    if (left.kind != right.kind) {
        return false;
    }

    switch (left.kind) {
        case Value::Kind::Null:
            return true;
        case Value::Kind::Bool:
            return left.boolValue == right.boolValue;
        case Value::Kind::Int:
            return left.intValue == right.intValue;
        case Value::Kind::Float:
            return left.floatValue == right.floatValue;
        case Value::Kind::String:
            return left.stringValue == right.stringValue;
        case Value::Kind::Object:
            return left.objectValue == right.objectValue;
    }
    return false;
}

std::string toString(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Null:
            return "null";
        case Value::Kind::Bool:
            return value.boolValue ? "true" : "false";
        case Value::Kind::Int:
            return std::to_string(value.intValue);
        case Value::Kind::Float: {
            std::ostringstream oss;
            oss << value.floatValue;
            return oss.str();
        }
        case Value::Kind::String:
            return value.stringValue;
        case Value::Kind::Object:
            return value.objectValue ? value.objectValue->describe() : "null";
    }
    return "null";
}

double toNumber(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Int:
            return static_cast<double>(value.intValue);
        case Value::Kind::Float:
            return value.floatValue;
        case Value::Kind::Bool:
            return value.boolValue ? 1.0 : 0.0;
        case Value::Kind::String:
            return std::stod(value.stringValue);
        case Value::Kind::Null:
        case Value::Kind::Object:
            break;
    }
    throw std::runtime_error("value is not numeric");
}

int64_t toInt(const Value& value) {
    switch (value.kind) {
        case Value::Kind::Int:
            return value.intValue;
        case Value::Kind::Float:
            return static_cast<int64_t>(value.floatValue);
        case Value::Kind::Bool:
            return value.boolValue ? 1 : 0;
        case Value::Kind::String:
            return std::stoll(value.stringValue);
        case Value::Kind::Null:
        case Value::Kind::Object:
            break;
    }
    throw std::runtime_error("value is not an integer");
}

}  // namespace smush
