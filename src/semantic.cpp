#include "semantic.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_set>

namespace smush {

namespace {

constexpr size_t kVariadicArity = std::numeric_limits<size_t>::max();

bool fieldIsOptional(const FieldDecl& field) {
    const bool nullableType = field.declaredType && (field.declaredType->nullable || field.declaredType->name == "Any" || field.declaredType->name == "Null");
    return field.defaultValue != nullptr || nullableType;
}

std::vector<std::string> genericParamNames(const std::vector<GenericParamDecl>& parameters) {
    std::vector<std::string> names;
    names.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        names.push_back(parameter.name);
    }
    return names;
}

TypePtr cloneType(const TypePtr& type) {
    if (!type) {
        return nullptr;
    }
    if (type->kind == TypeKind::Function) {
        std::vector<TypePtr> parameters;
        for (const auto& param : type->parameterTypes) {
            parameters.push_back(cloneType(param));
        }
        auto result = std::make_shared<Type>(std::move(parameters), cloneType(type->returnType), type->nullable);
        return result;
    }
    if (type->kind == TypeKind::Union || type->kind == TypeKind::Intersection) {
        std::vector<TypePtr> members;
        for (const auto& member : type->members) {
            members.push_back(cloneType(member));
        }
        return std::make_shared<Type>(type->kind, std::move(members), type->nullable);
    }
    std::vector<TypePtr> arguments;
    for (const auto& arg : type->genericArguments) {
        arguments.push_back(cloneType(arg));
    }
    return std::make_shared<Type>(type->name, std::move(arguments), type->nullable);
}

SemanticAnalyzer::CallableInfo builtin(
    std::string name,
    size_t arity,
    std::vector<TypePtr> parameterTypes,
    TypePtr returnType) {
    SemanticAnalyzer::CallableInfo info;
    info.name = std::move(name);
    info.arity = arity;
    info.parameterTypes = std::move(parameterTypes);
    info.returnType = std::move(returnType);
    return info;
}

const std::unordered_map<std::string, SemanticAnalyzer::CallableInfo>& builtins() {
    static const std::unordered_map<std::string, SemanticAnalyzer::CallableInfo> kBuiltins = {
        {"print", builtin("print", kVariadicArity, {}, std::make_shared<Type>("Void"))},
        {"println", builtin("println", kVariadicArity, {}, std::make_shared<Type>("Void"))},
        {"len", builtin("len", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Int"))},
        {"args", builtin("args", 0, {}, std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("String")}))},
        {"input", builtin("input", kVariadicArity, {}, std::make_shared<Type>("String"))},
        {"tuple", builtin("tuple", kVariadicArity, {}, std::make_shared<Type>("Tuple"))},
        {"map", builtin("map", 0, {}, std::make_shared<Type>("Map"))},
        {"set", builtin("set", 0, {}, std::make_shared<Type>("Set"))},
        {"map_get", builtin("map_get", 2, {std::make_shared<Type>("Map"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Any"))},
        {"map_set", builtin("map_set", 3, {std::make_shared<Type>("Map"), std::make_shared<Type>("Any"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Map"))},
        {"map_has", builtin("map_has", 2, {std::make_shared<Type>("Map"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Bool"))},
        {"map_remove", builtin("map_remove", 2, {std::make_shared<Type>("Map"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Bool"))},
        {"map_keys", builtin("map_keys", 1, {std::make_shared<Type>("Map")}, std::make_shared<Type>("List"))},
        {"map_values", builtin("map_values", 1, {std::make_shared<Type>("Map")}, std::make_shared<Type>("List"))},
        {"list_push", builtin("list_push", 2, {std::make_shared<Type>("List"), std::make_shared<Type>("Any")}, std::make_shared<Type>("List"))},
        {"list_pop", builtin("list_pop", 1, {std::make_shared<Type>("List")}, std::make_shared<Type>("Any"))},
        {"list_contains", builtin("list_contains", 2, {std::make_shared<Type>("List"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Bool"))},
        {"list_first", builtin("list_first", 1, {std::make_shared<Type>("List")}, std::make_shared<Type>("Any"))},
        {"list_last", builtin("list_last", 1, {std::make_shared<Type>("List")}, std::make_shared<Type>("Any"))},
        {"set_add", builtin("set_add", 2, {std::make_shared<Type>("Set"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Set"))},
        {"set_has", builtin("set_has", 2, {std::make_shared<Type>("Set"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Bool"))},
        {"set_remove", builtin("set_remove", 2, {std::make_shared<Type>("Set"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Bool"))},
        {"set_values", builtin("set_values", 1, {std::make_shared<Type>("Set")}, std::make_shared<Type>("List"))},
        {"iter_range", builtin("iter_range", 2, {std::make_shared<Type>("Int"), std::make_shared<Type>("Int")}, std::make_shared<Type>("Range"))},
        {"iter_collect", builtin("iter_collect", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("List"))},
        {"iter_take", builtin("iter_take", 2, {std::make_shared<Type>("Any"), std::make_shared<Type>("Int")}, std::make_shared<Type>("List"))},
        {"iter_skip", builtin("iter_skip", 2, {std::make_shared<Type>("Any"), std::make_shared<Type>("Int")}, std::make_shared<Type>("List"))},
        {"iter_enumerate", builtin("iter_enumerate", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("List"))},
        {"iter_first", builtin("iter_first", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Any"))},
        {"iter_last", builtin("iter_last", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Any"))},
        {"iter_map", builtin("iter_map", 2, {std::make_shared<Type>("Any"), std::make_shared<Type>("Function")}, std::make_shared<Type>("List"))},
        {"iter_filter", builtin("iter_filter", 2, {std::make_shared<Type>("Any"), std::make_shared<Type>("Function")}, std::make_shared<Type>("List"))},
        {"iter_fold", builtin("iter_fold", 3, {std::make_shared<Type>("Any"), std::make_shared<Type>("Any"), std::make_shared<Type>("Function")}, std::make_shared<Type>("Any"))},
        {"math_abs", builtin("math_abs", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Float"))},
        {"math_min", builtin("math_min", 2, {std::make_shared<Type>("Any"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Float"))},
        {"math_max", builtin("math_max", 2, {std::make_shared<Type>("Any"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Float"))},
        {"math_clamp", builtin("math_clamp", 3, {std::make_shared<Type>("Any"), std::make_shared<Type>("Any"), std::make_shared<Type>("Any")}, std::make_shared<Type>("Float"))},
        {"math_floor", builtin("math_floor", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Int"))},
        {"math_ceil", builtin("math_ceil", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Int"))},
        {"math_round", builtin("math_round", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Int"))},
        {"math_sqrt", builtin("math_sqrt", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Float"))},
        {"math_sin", builtin("math_sin", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Float"))},
        {"math_cos", builtin("math_cos", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Float"))},
        {"math_tan", builtin("math_tan", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Float"))},
        {"fs_read_text", builtin("fs_read_text", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"fs_write_text", builtin("fs_write_text", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"fs_append_text", builtin("fs_append_text", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"fs_exists", builtin("fs_exists", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"fs_create_dir", builtin("fs_create_dir", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"fs_remove_file", builtin("fs_remove_file", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"fs_remove_dir", builtin("fs_remove_dir", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"fs_list_dir", builtin("fs_list_dir", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("String")}))},
        {"path_join", builtin("path_join", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"path_normalize", builtin("path_normalize", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"path_basename", builtin("path_basename", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"path_dirname", builtin("path_dirname", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"path_extension", builtin("path_extension", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"path_stem", builtin("path_stem", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"time_now_ms", builtin("time_now_ms", 0, {}, std::make_shared<Type>("Int"))},
        {"time_now_seconds", builtin("time_now_seconds", 0, {}, std::make_shared<Type>("Int"))},
        {"time_sleep_ms", builtin("time_sleep_ms", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Void"))},
        {"duration_millis", builtin("duration_millis", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Int"))},
        {"duration_seconds", builtin("duration_seconds", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Int"))},
        {"duration_minutes", builtin("duration_minutes", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Int"))},
        {"duration_hours", builtin("duration_hours", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Int"))},
        {"duration_days", builtin("duration_days", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Int"))},
        {"duration_in_millis", builtin("duration_in_millis", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Int"))},
        {"duration_in_seconds", builtin("duration_in_seconds", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Int"))},
        {"date_from_parts", builtin("date_from_parts", 3, {std::make_shared<Type>("Int"), std::make_shared<Type>("Int"), std::make_shared<Type>("Int")}, std::make_shared<Type>("String"))},
        {"date_year", builtin("date_year", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Int"))},
        {"date_month", builtin("date_month", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Int"))},
        {"date_day", builtin("date_day", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Int"))},
        {"date_today_utc", builtin("date_today_utc", 0, {}, std::make_shared<Type>("String"))},
        {"env_get", builtin("env_get", 1, {std::make_shared<Type>("String")}, [] { auto type = std::make_shared<Type>("String"); type->nullable = true; return type; }())},
        {"env_set", builtin("env_set", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"env_has", builtin("env_has", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"env_args", builtin("env_args", 0, {}, std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("String")}))},
        {"env_cwd", builtin("env_cwd", 0, {}, std::make_shared<Type>("String"))},
        {"process_pid", builtin("process_pid", 0, {}, std::make_shared<Type>("Int"))},
        {"process_run", builtin("process_run", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Int"))},
        {"process_exit", builtin("process_exit", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Void"))},
        {"io_open_reader", builtin("io_open_reader", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("TextStream"))},
        {"io_open_writer", builtin("io_open_writer", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("TextStream"))},
        {"io_open_appender", builtin("io_open_appender", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("TextStream"))},
        {"io_read_line", builtin("io_read_line", 1, {std::make_shared<Type>("TextStream")}, [] { auto type = std::make_shared<Type>("String"); type->nullable = true; return type; }())},
        {"io_read_all", builtin("io_read_all", 1, {std::make_shared<Type>("TextStream")}, std::make_shared<Type>("String"))},
        {"io_write_text", builtin("io_write_text", 2, {std::make_shared<Type>("TextStream"), std::make_shared<Type>("String")}, std::make_shared<Type>("Void"))},
        {"io_write_line", builtin("io_write_line", 2, {std::make_shared<Type>("TextStream"), std::make_shared<Type>("String")}, std::make_shared<Type>("Void"))},
        {"io_flush_stream", builtin("io_flush_stream", 1, {std::make_shared<Type>("TextStream")}, std::make_shared<Type>("Void"))},
        {"io_close_stream", builtin("io_close_stream", 1, {std::make_shared<Type>("TextStream")}, std::make_shared<Type>("Void"))},
        {"json_stringify", builtin("json_stringify", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("String"))},
        {"json_parse", builtin("json_parse", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Any"))},
        {"bytes_from_string", builtin("bytes_from_string", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Bytes"))},
        {"bytes_to_string", builtin("bytes_to_string", 1, {std::make_shared<Type>("Bytes")}, std::make_shared<Type>("String"))},
        {"bytes_from_int_list", builtin("bytes_from_int_list", 1, {std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("Int")})}, std::make_shared<Type>("Bytes"))},
        {"bytes_to_int_list", builtin("bytes_to_int_list", 1, {std::make_shared<Type>("Bytes")}, std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("Int")}))},
        {"bytes_length", builtin("bytes_length", 1, {std::make_shared<Type>("Bytes")}, std::make_shared<Type>("Int"))},
        {"random_int", builtin("random_int", 2, {std::make_shared<Type>("Int"), std::make_shared<Type>("Int")}, std::make_shared<Type>("Int"))},
        {"random_float", builtin("random_float", 0, {}, std::make_shared<Type>("Float"))},
        {"random_choice", builtin("random_choice", 1, {std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("Any")})}, std::make_shared<Type>("Any"))},
        {"random_shuffle", builtin("random_shuffle", 1, {std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("Any")})}, std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("Any")}))},
        {"random_bytes", builtin("random_bytes", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Bytes"))},
        {"hash_fnv1a64_text", builtin("hash_fnv1a64_text", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"hash_fnv1a64_bytes", builtin("hash_fnv1a64_bytes", 1, {std::make_shared<Type>("Bytes")}, std::make_shared<Type>("String"))},
        {"hash_sha256_text", builtin("hash_sha256_text", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"hash_sha256_bytes", builtin("hash_sha256_bytes", 1, {std::make_shared<Type>("Bytes")}, std::make_shared<Type>("String"))},
        {"crypto_random_bytes", builtin("crypto_random_bytes", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Bytes"))},
        {"crypto_random_hex", builtin("crypto_random_hex", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("String"))},
        {"net_listen_tcp", builtin("net_listen_tcp", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("Int")}, std::make_shared<Type>("Listener"))},
        {"net_listener_port", builtin("net_listener_port", 1, {std::make_shared<Type>("Listener")}, std::make_shared<Type>("Int"))},
        {"net_accept", builtin("net_accept", 1, {std::make_shared<Type>("Listener")}, std::make_shared<Type>("Socket"))},
        {"net_connect_tcp", builtin("net_connect_tcp", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("Int")}, std::make_shared<Type>("Socket"))},
        {"net_send_text", builtin("net_send_text", 2, {std::make_shared<Type>("Socket"), std::make_shared<Type>("String")}, std::make_shared<Type>("Int"))},
        {"net_receive_text", builtin("net_receive_text", 1, {std::make_shared<Type>("Socket")}, std::make_shared<Type>("String"))},
        {"net_close_socket", builtin("net_close_socket", 1, {std::make_shared<Type>("Socket")}, std::make_shared<Type>("Void"))},
        {"net_close_listener", builtin("net_close_listener", 1, {std::make_shared<Type>("Listener")}, std::make_shared<Type>("Void"))},
        {"net_url_encode_component", builtin("net_url_encode_component", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"net_url_decode_component", builtin("net_url_decode_component", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"net_url_build", builtin("net_url_build", 5, {std::make_shared<Type>("String"), std::make_shared<Type>("String"), std::make_shared<Type>("Int"), std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"net_url_parse", builtin("net_url_parse", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("Map", std::vector<TypePtr>{std::make_shared<Type>("String"), std::make_shared<Type>("Any")}))},
        {"ui_backend_name", builtin("ui_backend_name", 0, {}, std::make_shared<Type>("String"))},
        {"ui_backend_available", builtin("ui_backend_available", 0, {}, std::make_shared<Type>("Bool"))},
        {"ui_app_new", builtin("ui_app_new", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("UiApp"))},
        {"ui_app_run", builtin("ui_app_run", 1, {std::make_shared<Type>("UiApp")}, std::make_shared<Type>("Int"))},
        {"ui_app_name", builtin("ui_app_name", 1, {std::make_shared<Type>("UiApp")}, std::make_shared<Type>("String"))},
        {"ui_window_new", builtin("ui_window_new", 5, {std::make_shared<Type>("UiApp"), std::make_shared<Type>("String"), std::make_shared<Type>("Int"), std::make_shared<Type>("Int"), std::make_shared<Type>("UiView")}, std::make_shared<Type>("UiWindow"))},
        {"ui_window_show", builtin("ui_window_show", 1, {std::make_shared<Type>("UiWindow")}, std::make_shared<Type>("Void"))},
        {"ui_window_close", builtin("ui_window_close", 1, {std::make_shared<Type>("UiWindow")}, std::make_shared<Type>("Void"))},
        {"ui_window_is_visible", builtin("ui_window_is_visible", 1, {std::make_shared<Type>("UiWindow")}, std::make_shared<Type>("Bool"))},
        {"ui_window_get_title", builtin("ui_window_get_title", 1, {std::make_shared<Type>("UiWindow")}, std::make_shared<Type>("String"))},
        {"ui_window_set_title", builtin("ui_window_set_title", 2, {std::make_shared<Type>("UiWindow"), std::make_shared<Type>("String")}, std::make_shared<Type>("Void"))},
        {"ui_window_get_width", builtin("ui_window_get_width", 1, {std::make_shared<Type>("UiWindow")}, std::make_shared<Type>("Int"))},
        {"ui_window_get_height", builtin("ui_window_get_height", 1, {std::make_shared<Type>("UiWindow")}, std::make_shared<Type>("Int"))},
        {"ui_window_set_size", builtin("ui_window_set_size", 3, {std::make_shared<Type>("UiWindow"), std::make_shared<Type>("Int"), std::make_shared<Type>("Int")}, std::make_shared<Type>("Void"))},
        {"ui_window_set_content", builtin("ui_window_set_content", 2, {std::make_shared<Type>("UiWindow"), std::make_shared<Type>("UiView")}, std::make_shared<Type>("Void"))},
        {"ui_window_on_close", builtin("ui_window_on_close", 2, {std::make_shared<Type>("UiWindow"), std::make_shared<Type>("Function")}, std::make_shared<Type>("Void"))},
        {"ui_window_apply_theme", builtin("ui_window_apply_theme", 2, {std::make_shared<Type>("UiWindow"), std::make_shared<Type>("UiTheme")}, std::make_shared<Type>("Void"))},
        {"ui_label_new", builtin("ui_label_new", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("UiView"))},
        {"ui_button_new", builtin("ui_button_new", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("UiView"))},
        {"ui_text_field_new", builtin("ui_text_field_new", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("UiView"))},
        {"ui_text_area_new", builtin("ui_text_area_new", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("UiView"))},
        {"ui_check_box_new", builtin("ui_check_box_new", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("Bool")}, std::make_shared<Type>("UiView"))},
        {"ui_row_new", builtin("ui_row_new", 1, {std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("UiView")})}, std::make_shared<Type>("UiView"))},
        {"ui_column_new", builtin("ui_column_new", 1, {std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("UiView")})}, std::make_shared<Type>("UiView"))},
        {"ui_grid_new", builtin("ui_grid_new", 2, {std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("UiView")}), std::make_shared<Type>("Int")}, std::make_shared<Type>("UiView"))},
        {"ui_view_get_text", builtin("ui_view_get_text", 1, {std::make_shared<Type>("UiView")}, std::make_shared<Type>("String"))},
        {"ui_view_set_text", builtin("ui_view_set_text", 2, {std::make_shared<Type>("UiView"), std::make_shared<Type>("String")}, std::make_shared<Type>("Void"))},
        {"ui_view_get_placeholder", builtin("ui_view_get_placeholder", 1, {std::make_shared<Type>("UiView")}, std::make_shared<Type>("String"))},
        {"ui_view_set_placeholder", builtin("ui_view_set_placeholder", 2, {std::make_shared<Type>("UiView"), std::make_shared<Type>("String")}, std::make_shared<Type>("Void"))},
        {"ui_view_get_checked", builtin("ui_view_get_checked", 1, {std::make_shared<Type>("UiView")}, std::make_shared<Type>("Bool"))},
        {"ui_view_set_checked", builtin("ui_view_set_checked", 2, {std::make_shared<Type>("UiView"), std::make_shared<Type>("Bool")}, std::make_shared<Type>("Void"))},
        {"ui_view_on_click", builtin("ui_view_on_click", 2, {std::make_shared<Type>("UiView"), std::make_shared<Type>("Function")}, std::make_shared<Type>("Void"))},
        {"ui_view_on_change", builtin("ui_view_on_change", 2, {std::make_shared<Type>("UiView"), std::make_shared<Type>("Function")}, std::make_shared<Type>("Void"))},
        {"ui_view_click", builtin("ui_view_click", 1, {std::make_shared<Type>("UiView")}, std::make_shared<Type>("Void"))},
        {"ui_dialog_info", builtin("ui_dialog_info", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("Void"))},
        {"ui_dialog_confirm", builtin("ui_dialog_confirm", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"ui_theme_new", builtin("ui_theme_new", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("UiTheme"))},
        {"ui_theme_set", builtin("ui_theme_set", 3, {std::make_shared<Type>("UiTheme"), std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("UiTheme"))},
        {"async_ready", builtin("async_ready", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Future", std::vector<TypePtr>{std::make_shared<Type>("Any")}))},
        {"async_yield", builtin("async_yield", 0, {}, std::make_shared<Type>("Future", std::vector<TypePtr>{std::make_shared<Type>("Void")}))},
        {"str", builtin("str", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("String"))},
        {"int", builtin("int", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Int"))},
        {"string_trim", builtin("string_trim", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"string_lower", builtin("string_lower", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"string_upper", builtin("string_upper", 1, {std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"string_contains", builtin("string_contains", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"string_starts_with", builtin("string_starts_with", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"string_ends_with", builtin("string_ends_with", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"string_replace", builtin("string_replace", 3, {std::make_shared<Type>("String"), std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"string_split", builtin("string_split", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("String")}, std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("String")}))},
        {"string_join", builtin("string_join", 2, {std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("Any")}), std::make_shared<Type>("String")}, std::make_shared<Type>("String"))},
        {"string_format", builtin("string_format", 2, {std::make_shared<Type>("String"), std::make_shared<Type>("List", std::vector<TypePtr>{std::make_shared<Type>("Any")})}, std::make_shared<Type>("String"))},
        {"type_name", builtin("type_name", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("String"))},
        {"implements", builtin("implements", 2, {std::make_shared<Type>("Any"), std::make_shared<Type>("String")}, std::make_shared<Type>("Bool"))},
        {"panic", builtin("panic", 1, {std::make_shared<Type>("Any")}, std::make_shared<Type>("Void"))},
        {"gc_heap_size", builtin("gc_heap_size", 0, {}, std::make_shared<Type>("Int"))},
        {"gc_collections", builtin("gc_collections", 0, {}, std::make_shared<Type>("Int"))},
        {"gc_allocated_objects", builtin("gc_allocated_objects", 0, {}, std::make_shared<Type>("Int"))},
        {"gc_last_reclaimed", builtin("gc_last_reclaimed", 0, {}, std::make_shared<Type>("Int"))},
        {"gc_peak_heap_size", builtin("gc_peak_heap_size", 0, {}, std::make_shared<Type>("Int"))},
        {"gc_collect", builtin("gc_collect", 0, {}, std::make_shared<Type>("Int"))},
        {"gc_set_threshold", builtin("gc_set_threshold", 1, {std::make_shared<Type>("Int")}, std::make_shared<Type>("Int"))},
        {"gc_trace", builtin("gc_trace", 1, {std::make_shared<Type>("Bool")}, std::make_shared<Type>("Bool"))},
        {"trace_execution", builtin("trace_execution", 1, {std::make_shared<Type>("Bool")}, std::make_shared<Type>("Bool"))},
        {"profile_statement_count", builtin("profile_statement_count", 0, {}, std::make_shared<Type>("Int"))},
        {"profile_expression_count", builtin("profile_expression_count", 0, {}, std::make_shared<Type>("Int"))},
        {"profile_instruction_count", builtin("profile_instruction_count", 0, {}, std::make_shared<Type>("Int"))},
        {"profile_call_count", builtin("profile_call_count", 0, {}, std::make_shared<Type>("Int"))},
        {"profile_native_call_count", builtin("profile_native_call_count", 0, {}, std::make_shared<Type>("Int"))},
        {"profile_max_call_depth", builtin("profile_max_call_depth", 0, {}, std::make_shared<Type>("Int"))},
    };
    return kBuiltins;
}

TypePtr substituteType(const TypePtr& type, const std::unordered_map<std::string, TypePtr>& mapping) {
    if (!type) {
        return nullptr;
    }
    if (type->kind == TypeKind::Function) {
        std::vector<TypePtr> parameters;
        for (const auto& param : type->parameterTypes) {
            parameters.push_back(substituteType(param, mapping));
        }
        return std::make_shared<Type>(std::move(parameters), substituteType(type->returnType, mapping), type->nullable);
    }
    if (type->kind == TypeKind::Union || type->kind == TypeKind::Intersection) {
        std::vector<TypePtr> members;
        for (const auto& member : type->members) {
            members.push_back(substituteType(member, mapping));
        }
        return std::make_shared<Type>(type->kind, std::move(members), type->nullable);
    }
    auto mapped = mapping.find(type->name);
    if (mapped != mapping.end() && type->genericArguments.empty()) {
        auto result = cloneType(mapped->second);
        if (result) {
            result->nullable = result->nullable || type->nullable;
        }
        return result;
    }
    std::vector<TypePtr> args;
    for (const auto& arg : type->genericArguments) {
        args.push_back(substituteType(arg, mapping));
    }
    return std::make_shared<Type>(type->name, std::move(args), type->nullable);
}

void inferGenericMapping(
    const TypePtr& parameterType,
    const TypePtr& argumentType,
    const std::unordered_set<std::string>& genericParameters,
    std::unordered_map<std::string, TypePtr>& mapping) {
    if (!parameterType || !argumentType) {
        return;
    }
    if (parameterType->kind == TypeKind::Function || argumentType->kind == TypeKind::Function) {
        if (parameterType->kind != TypeKind::Function || argumentType->kind != TypeKind::Function ||
            parameterType->parameterTypes.size() != argumentType->parameterTypes.size()) {
            return;
        }
        for (size_t i = 0; i < parameterType->parameterTypes.size(); ++i) {
            inferGenericMapping(parameterType->parameterTypes[i], argumentType->parameterTypes[i], genericParameters, mapping);
        }
        inferGenericMapping(parameterType->returnType, argumentType->returnType, genericParameters, mapping);
        return;
    }
    if (parameterType->kind == TypeKind::Union || parameterType->kind == TypeKind::Intersection ||
        argumentType->kind == TypeKind::Union || argumentType->kind == TypeKind::Intersection) {
        return;
    }
    if (genericParameters.find(parameterType->name) != genericParameters.end() &&
        parameterType->genericArguments.empty()) {
        mapping.emplace(parameterType->name, cloneType(argumentType));
        return;
    }
    if (parameterType->name != argumentType->name ||
        parameterType->genericArguments.size() != argumentType->genericArguments.size()) {
        return;
    }
    for (size_t i = 0; i < parameterType->genericArguments.size(); ++i) {
        inferGenericMapping(parameterType->genericArguments[i], argumentType->genericArguments[i], genericParameters, mapping);
    }
}

TypePtr mergeInferredReturnTypes(const TypePtr& current, const TypePtr& next) {
    if (!current) {
        return cloneType(next);
    }
    if (!next) {
        return cloneType(current);
    }
    if (current->toString() == next->toString()) {
        return cloneType(current);
    }
    if ((current->name == "Null" && current->kind == TypeKind::Named) ||
        (next->name == "Null" && next->kind == TypeKind::Named)) {
        TypePtr base = current->name == "Null" ? cloneType(next) : cloneType(current);
        if (base) {
            base->nullable = true;
        }
        return base ? base : std::make_shared<Type>("Any");
    }
    return std::make_shared<Type>("Any");
}

}  // namespace

bool SemanticAnalyzer::analyze(Program* program) {
    errors.clear();
    scopes.clear();
    functions.clear();
    constructors.clear();
    interfaces.clear();
    interfaceDecls.clear();
    classes.clear();
    knownTypes.clear();
    typeGenericParameters.clear();
    typeAliases.clear();
    genericScopes.clear();
    currentClass = nullptr;
    currentReturnType.reset();
    inFunction = false;
    inAsyncFunction = false;
    loopDepth = 0;

    knownTypes["Any"] = 0;
    knownTypes["Void"] = 0;
    knownTypes["Null"] = 0;
    knownTypes["Bool"] = 0;
    knownTypes["Int"] = 0;
    knownTypes["Float"] = 0;
    knownTypes["Float64"] = 0;
    knownTypes["String"] = 0;
    knownTypes["Function"] = 0;
    knownTypes["Object"] = 0;
    knownTypes["List"] = 1;
    knownTypes["Tuple"] = 0;
    knownTypes["Map"] = 2;
    knownTypes["Set"] = 1;
    knownTypes["Range"] = 0;
    knownTypes["Future"] = 1;
    knownTypes["Bytes"] = 0;
    knownTypes["TextStream"] = 0;
    knownTypes["Socket"] = 0;
    knownTypes["Listener"] = 0;
    knownTypes["UiApp"] = 0;
    knownTypes["UiWindow"] = 0;
    knownTypes["UiView"] = 0;
    knownTypes["UiTheme"] = 0;

    pushScope();
    for (const auto& [name, info] : builtins()) {
        define(name, false, SourceLocation{}, std::make_shared<Type>(info.parameterTypes, cloneType(info.returnType ? info.returnType : makeType("Any"))));
    }
    collectDeclarations(program);
    for (const auto& stmt : program->statements) {
        analyzeStatement(stmt.get());
    }

    auto mainIt = functions.find("main");
    if (mainIt == functions.end()) {
        error(program->loc, "program entry point 'main' is required");
    } else if (mainIt->second.arity != 0) {
        error(program->loc, "'main' must not take parameters");
    }

    popScope();
    return errors.empty();
}

const std::vector<std::string>& SemanticAnalyzer::getErrors() const {
    return errors;
}

void SemanticAnalyzer::collectDeclarations(Program* program) {
    for (const auto& stmt : program->statements) {
        switch (stmt->kind) {
            case StatementKind::Function: {
                auto* fn = static_cast<const FunctionDecl*>(stmt.get());
                CallableInfo info;
                info.name = fn->name;
                info.arity = fn->parameters.size();
                info.returnType = cloneType(fn->returnType ? fn->returnType : makeType("Any"));
                info.genericParameters = fn->genericParameters;
                info.isAsync = fn->isAsync;
                for (const auto& param : fn->parameters) {
                    info.parameterTypes.push_back(cloneType(param.second ? param.second : makeType("Any")));
                }
                functions.emplace(fn->name, std::move(info));
                define(fn->name, false, fn->loc, std::make_shared<Type>(functions[fn->name].parameterTypes, cloneType(functions[fn->name].returnType ? functions[fn->name].returnType : makeType("Any"))));
                break;
            }
            case StatementKind::ClassDecl: {
                auto* klass = static_cast<const ClassDecl*>(stmt.get());
                classes[klass->name] = klass;
                knownTypes[klass->name] = klass->genericParameters.size();
                typeGenericParameters[klass->name] = klass->genericParameters;
                std::vector<CallableInfo> ctorGroup;
                bool hasExplicitConstructor = false;
                for (const auto& method : klass->methods) {
                    if (!method.isConstructor) {
                        continue;
                    }
                    hasExplicitConstructor = true;
                    CallableInfo ctor;
                    ctor.name = klass->name;
                    ctor.arity = method.parameters.size();
                    ctor.returnType = std::make_shared<Type>(klass->name);
                    ctor.genericParameters = method.genericParameters;
                    ctor.isStatic = false;
                    ctor.isConstructor = true;
                    ctor.visibility = method.visibility;
                    ctor.ownerType = klass->name;
                    for (const auto& param : method.parameters) {
                        ctor.parameterTypes.push_back(cloneType(param.second ? param.second : makeType("Any")));
                    }
                    ctorGroup.push_back(std::move(ctor));
                }
                if (!hasExplicitConstructor) {
                    const std::vector<const FieldDecl*> allFields = collectClassFields(klass);
                    size_t requiredCount = allFields.size();
                    for (size_t i = 0; i < allFields.size(); ++i) {
                        if (allFields[i] && fieldIsOptional(*allFields[i])) {
                            requiredCount = i;
                            break;
                        }
                    }
                    for (size_t arity = requiredCount; arity <= allFields.size(); ++arity) {
                        CallableInfo ctor;
                        ctor.name = klass->name;
                        ctor.arity = arity;
                        ctor.returnType = std::make_shared<Type>(klass->name);
                        ctor.genericParameters = klass->genericParameters;
                        ctor.isConstructor = true;
                        ctor.visibility = Visibility::Public;
                        ctor.ownerType = klass->name;
                        for (size_t i = 0; i < arity; ++i) {
                            const auto* field = allFields[i];
                            ctor.parameterTypes.push_back(cloneType(field && field->declaredType ? field->declaredType : makeType("Any")));
                        }
                        ctorGroup.push_back(std::move(ctor));
                    }
                }
                constructors.emplace(klass->name, std::move(ctorGroup));
                define(klass->name, false, klass->loc, std::make_shared<Type>(klass->name));
                break;
            }
            case StatementKind::DataDecl: {
                auto* data = static_cast<const DataDecl*>(stmt.get());
                knownTypes[data->name] = data->genericParameters.size();
                typeGenericParameters[data->name] = data->genericParameters;
                size_t requiredCount = data->fields.size();
                for (size_t i = 0; i < data->fields.size(); ++i) {
                    if (fieldIsOptional(data->fields[i])) {
                        requiredCount = i;
                        break;
                    }
                }
                for (size_t arity = requiredCount; arity <= data->fields.size(); ++arity) {
                    CallableInfo ctor;
                    ctor.name = data->name;
                    ctor.arity = arity;
                    ctor.returnType = std::make_shared<Type>(data->name);
                    ctor.genericParameters = data->genericParameters;
                    ctor.isConstructor = true;
                    ctor.ownerType = data->name;
                    for (size_t i = 0; i < arity; ++i) {
                        ctor.parameterTypes.push_back(cloneType(data->fields[i].declaredType ? data->fields[i].declaredType : makeType("Any")));
                    }
                    constructors[data->name].push_back(std::move(ctor));
                }
                define(data->name, false, data->loc, std::make_shared<Type>(data->name));
                break;
            }
            case StatementKind::InterfaceDecl: {
                auto* iface = static_cast<const InterfaceDecl*>(stmt.get());
                knownTypes[iface->name] = iface->genericParameters.size();
                typeGenericParameters[iface->name] = iface->genericParameters;
                InterfaceInfo info;
                info.baseInterfaces = iface->baseInterfaces;
                for (const auto& method : iface->methods) {
                    CallableInfo callable;
                    callable.name = method.name;
                    callable.arity = method.parameters.size();
                    callable.returnType = cloneType(method.returnType);
                    callable.genericParameters = method.genericParameters;
                    callable.isAsync = method.isAsync;
                    callable.isStatic = method.isStatic;
                    callable.isAbstract = true;
                    callable.visibility = method.visibility;
                    callable.ownerType = iface->name;
                    for (const auto& param : method.parameters) {
                        callable.parameterTypes.push_back(cloneType(param.second ? param.second : makeType("Any")));
                    }
                    info.methods[method.name].push_back(std::move(callable));
                }
                interfaces.emplace(iface->name, std::move(info));
                interfaceDecls[iface->name] = iface;
                define(iface->name, false, iface->loc, std::make_shared<Type>(iface->name));
                break;
            }
            case StatementKind::TypeAliasDecl: {
                auto* alias = static_cast<const TypeAliasDecl*>(stmt.get());
                knownTypes[alias->name] = alias->genericParameters.size();
                typeGenericParameters[alias->name] = alias->genericParameters;
                typeAliases[alias->name] = TypeAliasInfo{alias->genericParameters, cloneType(alias->aliasedType), alias->loc};
                break;
            }
            default:
                break;
        }
    }
}

void SemanticAnalyzer::analyzeStatement(const Statement* statement) {
    switch (statement->kind) {
        case StatementKind::Program: {
            const auto* program = static_cast<const Program*>(statement);
            for (const auto& stmt : program->statements) {
                analyzeStatement(stmt.get());
            }
            return;
        }
        case StatementKind::InterfaceDecl: {
            auto* iface = static_cast<const InterfaceDecl*>(statement);
            pushGenericScope(iface->genericParameters);
            for (const auto& generic : iface->genericParameters) {
                for (const auto& bound : generic.bounds) {
                    validateType(bound, generic.loc);
                }
            }
            for (const auto& base : iface->baseInterfaces) {
                if (interfaces.find(base) == interfaces.end()) {
                    error(statement->loc, "unknown interface '" + base + "'");
                } else if (base == iface->name) {
                    error(statement->loc, "interface '" + iface->name + "' cannot extend itself");
                }
            }
            for (const auto& method : iface->methods) {
                pushGenericScope(method.genericParameters);
                for (const auto& generic : method.genericParameters) {
                    for (const auto& bound : generic.bounds) {
                        validateType(bound, generic.loc);
                    }
                }
                validateType(method.returnType ? method.returnType : makeType("Void"), statement->loc);
                for (const auto& param : method.parameters) {
                    validateType(param.second ? param.second : makeType("Any"), statement->loc);
                }
                popGenericScope();
            }
            popGenericScope();
            return;
        }
        case StatementKind::TypeAliasDecl: {
            auto* alias = static_cast<const TypeAliasDecl*>(statement);
            pushGenericScope(alias->genericParameters);
            for (const auto& generic : alias->genericParameters) {
                for (const auto& bound : generic.bounds) {
                    validateType(bound, generic.loc);
                }
            }
            validateType(alias->aliasedType, alias->loc);
            popGenericScope();
            return;
        }
        case StatementKind::ImportDecl:
            return;
        case StatementKind::Block:
            analyzeBlock(static_cast<const BlockStmt*>(statement));
            return;
        case StatementKind::Function:
            analyzeFunction(static_cast<const FunctionDecl*>(statement));
            return;
        case StatementKind::ClassDecl:
            analyzeClass(static_cast<const ClassDecl*>(statement));
            return;
        case StatementKind::DataDecl:
            analyzeData(static_cast<const DataDecl*>(statement));
            return;
        case StatementKind::TryStmt: {
            auto* tryStmt = static_cast<const TryStmt*>(statement);
            analyzeStatement(tryStmt->tryBranch.get());
            pushScope();
            define(tryStmt->catchName, true, tryStmt->loc, makeType("Any"));
            analyzeStatement(tryStmt->catchBranch.get());
            popScope();
            return;
        }
        case StatementKind::ThrowStmt:
            analyzeExpression(static_cast<const ThrowStmt*>(statement)->value.get());
            return;
        case StatementKind::MatchStmt: {
            auto* matchStmt = static_cast<const MatchStmt*>(statement);
            TypePtr matchedType = analyzeExpression(matchStmt->value.get());
            for (const auto& matchCase : matchStmt->cases) {
                if (matchCase.pattern) {
                    analyzeExpression(matchCase.pattern.get());
                }
                if (!matchCase.typeName.empty()) {
                    validateType(std::make_shared<Type>(matchCase.typeName), matchCase.loc);
                }
                analyzeStatement(matchCase.body.get());
            }
            if (!isExhaustiveMatch(matchedType, matchStmt)) {
                error(matchStmt->loc, "non-exhaustive match for type '" + typeName(matchedType) + "'");
            }
            return;
        }
        case StatementKind::IfStmt: {
            auto* ifStmt = static_cast<const IfStmt*>(statement);
            TypePtr conditionType = analyzeExpression(ifStmt->condition.get());
            if (conditionType && conditionType->name != "Bool" && conditionType->name != "Any") {
                error(ifStmt->condition->loc, "if-condition must be Bool, got '" + typeName(conditionType) + "'");
            }
            FlowNarrowing thenNarrowing;
            FlowNarrowing elseNarrowing;
            collectFlowNarrowing(ifStmt->condition.get(), true, thenNarrowing);
            collectFlowNarrowing(ifStmt->condition.get(), false, elseNarrowing);
            pushFlowScope(thenNarrowing);
            analyzeStatement(ifStmt->thenBranch.get());
            popScope();
            if (ifStmt->elseBranch) {
                pushFlowScope(elseNarrowing);
                analyzeStatement(ifStmt->elseBranch.get());
                popScope();
            }
            return;
        }
        case StatementKind::WhileStmt: {
            auto* whileStmt = static_cast<const WhileStmt*>(statement);
            TypePtr conditionType = analyzeExpression(whileStmt->condition.get());
            if (conditionType && conditionType->name != "Bool" && conditionType->name != "Any") {
                error(whileStmt->condition->loc, "while-condition must be Bool, got '" + typeName(conditionType) + "'");
            }
            ++loopDepth;
            FlowNarrowing bodyNarrowing;
            collectFlowNarrowing(whileStmt->condition.get(), true, bodyNarrowing);
            pushFlowScope(bodyNarrowing);
            analyzeStatement(whileStmt->body.get());
            popScope();
            --loopDepth;
            return;
        }
        case StatementKind::ForStmt: {
            auto* forStmt = static_cast<const ForStmt*>(statement);
            TypePtr iterableType = analyzeExpression(forStmt->iterable.get());
            if (iterableType && iterableType->name != "List" &&
                iterableType->name != "Range" &&
                iterableType->name != "String" &&
                iterableType->name != "Any") {
                error(forStmt->iterable->loc, "for-loop expects List, Range, or String, got '" + typeName(iterableType) + "'");
            }
            pushScope();
            TypePtr elementType = makeType("Any");
            if (iterableType && iterableType->name == "List" && !iterableType->genericArguments.empty()) {
                elementType = cloneType(iterableType->genericArguments.front());
            } else if (iterableType && iterableType->name == "String") {
                elementType = makeType("String");
            } else if (iterableType && iterableType->name == "Range") {
                elementType = makeType("Int");
            }
            define(forStmt->name, true, forStmt->loc, elementType);
            ++loopDepth;
            analyzeStatement(forStmt->body.get());
            --loopDepth;
            popScope();
            return;
        }
        case StatementKind::ReturnStmt: {
            auto* ret = static_cast<const ReturnStmt*>(statement);
            if (!inFunction) {
                error(ret->loc, "'return' is only valid inside a function or method");
                return;
            }
            TypePtr returnedType = ret->value ? analyzeExpression(ret->value.get(), currentReturnType) : makeType("Void");
            if (!currentReturnType) {
                currentReturnType = mergeInferredReturnTypes(currentReturnType, returnedType);
                return;
            }
            if (!isAssignable(currentReturnType, returnedType)) {
                error(ret->loc, "return type mismatch: expected '" + typeName(currentReturnType) +
                                    "' but got '" + typeName(returnedType) + "'");
            }
            return;
        }
        case StatementKind::BreakStmt:
            if (loopDepth == 0) {
                error(statement->loc, "'break' is only valid inside a loop");
            }
            return;
        case StatementKind::ContinueStmt:
            if (loopDepth == 0) {
                error(statement->loc, "'continue' is only valid inside a loop");
            }
            return;
        case StatementKind::VarDecl: {
            auto* var = static_cast<const VarDeclStmt*>(statement);
            TypePtr initializerType = var->initializer ? analyzeExpression(var->initializer.get(), var->declaredType) : makeType("Null");
            if (var->declaredType) {
                validateType(var->declaredType, var->loc);
                if (var->initializer && !isAssignable(var->declaredType, initializerType)) {
                    error(var->loc, "initializer for '" + var->name + "' has type '" + typeName(initializerType) +
                                        "', expected '" + typeName(var->declaredType) + "'");
                }
            }
            define(var->name, var->mutableBinding, var->loc, var->declaredType ? cloneType(var->declaredType) : initializerType);
            return;
        }
        case StatementKind::ExpressionStmt:
            analyzeExpression(static_cast<const ExpressionStmt*>(statement)->expression.get());
            return;
    }
}

TypePtr SemanticAnalyzer::analyzeExpression(const Expression* expression, const TypePtr& expectedType) {
    if (!expression) {
        return makeType("Void");
    }

    switch (expression->kind) {
        case ExpressionKind::Identifier: {
            auto* ident = static_cast<const IdentifierExpr*>(expression);
            if (const Symbol* symbol = resolve(ident->name)) {
                return symbol->type ? cloneType(symbol->type) : makeType("Any");
            }
            if (currentClass) {
                for (const auto& method : collectClassMethods(currentClass, ident->name, inStaticMethod)) {
                    std::vector<TypePtr> params;
                    for (const auto& param : method.parameterTypes) {
                        params.push_back(cloneType(param));
                    }
                    return std::make_shared<Type>(
                        std::move(params),
                        cloneType(method.returnType ? method.returnType : makeType("Any")));
                }
            }
            if (auto builtinIt = builtins().find(ident->name); builtinIt != builtins().end()) {
                return std::make_shared<Type>(builtinIt->second.parameterTypes, cloneType(builtinIt->second.returnType));
            }
            error(expression->loc, "undefined symbol '" + ident->name + "'");
            return makeType("Any");
        }
        case ExpressionKind::Literal: {
            auto* literal = static_cast<const LiteralExpr*>(expression);
            switch (literal->tokenType) {
                case TokenType::TRUE:
                case TokenType::FALSE:
                    return makeType("Bool");
                case TokenType::INTEGER:
                    return makeType("Int");
                case TokenType::FLOAT:
                    return makeType("Float");
                case TokenType::STRING:
                case TokenType::CHAR:
                    return makeType("String");
                case TokenType::NULL_KW:
                    return makeType("Null");
                default:
                    return makeType("Any");
            }
        }
        case ExpressionKind::Lambda: {
            auto* lambda = static_cast<const LambdaExpr*>(expression);
            pushScope();
            const bool previousInFunction = inFunction;
            const bool previousAsync = inAsyncFunction;
            TypePtr previousReturn = currentReturnType;
            inFunction = true;
            inAsyncFunction = false;
            currentReturnType = cloneType(lambda->returnType);
            std::vector<TypePtr> parameterTypes;
            parameterTypes.reserve(lambda->parameters.size());
            for (const auto& param : lambda->parameters) {
                TypePtr paramType = param.second ? cloneType(param.second) : makeType("Any");
                validateType(paramType, lambda->loc);
                parameterTypes.push_back(cloneType(paramType));
                define(param.first, true, lambda->loc, std::move(paramType));
            }
            if (currentReturnType) {
                validateType(currentReturnType, lambda->loc);
            }
            analyzeStatement(lambda->body.get());
            TypePtr inferredLambdaReturn = currentReturnType ? cloneType(currentReturnType) : makeType("Void");
            if (!lambda->returnType) {
                const_cast<LambdaExpr*>(lambda)->returnType = cloneType(inferredLambdaReturn);
            }
            currentReturnType = previousReturn;
            inAsyncFunction = previousAsync;
            inFunction = previousInFunction;
            popScope();
            return std::make_shared<Type>(std::move(parameterTypes), cloneType(inferredLambdaReturn));
        }
        case ExpressionKind::Unary: {
            auto* unary = static_cast<const UnaryExpr*>(expression);
            TypePtr operandType = analyzeExpression(unary->operand.get());
            if ((unary->op.type == TokenType::BANG || unary->op.type == TokenType::NOT) &&
                operandType->name != "Bool" && operandType->name != "Any") {
                error(unary->loc, "logical negation expects Bool, got '" + typeName(operandType) + "'");
            }
            if (unary->op.type == TokenType::MINUS &&
                operandType->name != "Int" &&
                operandType->name != "Float" &&
                operandType->name != "Any") {
                error(unary->loc, "numeric negation expects Int or Float, got '" + typeName(operandType) + "'");
            }
            return unary->op.type == TokenType::MINUS ? operandType : makeType("Bool");
        }
        case ExpressionKind::Await: {
            auto* awaitExpr = static_cast<const AwaitExpr*>(expression);
            TypePtr operandType = analyzeExpression(awaitExpr->operand.get());
            TypePtr inner = unwrapFutureType(operandType);
            if (!inner) {
                error(awaitExpr->loc, "'await' expects Future<T>, got '" + typeName(operandType) + "'");
                return makeType("Any");
            }
            return inner;
        }
        case ExpressionKind::TypeCheck: {
            auto* typeCheck = static_cast<const TypeCheckExpr*>(expression);
            TypePtr operandType = analyzeExpression(typeCheck->operand.get());
            validateType(typeCheck->targetType, typeCheck->loc);
            if (!supportsRuntimeTypeCheck(typeCheck->targetType)) {
                error(typeCheck->loc,
                      "'is' only supports runtime-testable types, got '" + typeName(typeCheck->targetType) + "'");
            }
            if (operandType && operandType->name == "Void") {
                error(typeCheck->loc, "cannot apply 'is' to a Void expression");
            }
            return makeType("Bool");
        }
        case ExpressionKind::SafeCast: {
            auto* castExpr = static_cast<const SafeCastExpr*>(expression);
            TypePtr operandType = analyzeExpression(castExpr->operand.get());
            validateType(castExpr->targetType, castExpr->loc);
            if (!supportsRuntimeTypeCheck(castExpr->targetType)) {
                error(castExpr->loc,
                      "'as' only supports runtime-testable types, got '" + typeName(castExpr->targetType) + "'");
            }
            if (operandType && operandType->name == "Void") {
                error(castExpr->loc, "cannot cast a Void expression");
            }
            return makeSafeCastResult(castExpr->targetType);
        }
        case ExpressionKind::CheckedCast: {
            auto* castExpr = static_cast<const CheckedCastExpr*>(expression);
            TypePtr operandType = analyzeExpression(castExpr->operand.get());
            validateType(castExpr->targetType, castExpr->loc);
            if (!supportsRuntimeTypeCheck(castExpr->targetType)) {
                error(castExpr->loc,
                      "'cast' only supports runtime-testable types, got '" + typeName(castExpr->targetType) + "'");
            }
            if (operandType && operandType->name == "Void") {
                error(castExpr->loc, "cannot cast a Void expression");
            }
            return cloneType(castExpr->targetType ? castExpr->targetType : makeType("Any"));
        }
        case ExpressionKind::Binary: {
            auto* binary = static_cast<const BinaryExpr*>(expression);
            TypePtr left = analyzeExpression(binary->left.get());
            TypePtr right = analyzeExpression(binary->right.get());
            return inferBinaryType(binary, left, right);
        }
        case ExpressionKind::Call: {
            auto* call = static_cast<const CallExpr*>(expression);
            std::vector<TypePtr> argumentTypes;
            for (const auto& arg : call->arguments) {
                argumentTypes.push_back(analyzeExpression(arg.get()));
            }

            if (call->callee->kind == ExpressionKind::Member) {
                const auto* member = static_cast<const MemberExpr*>(call->callee.get());
                const bool classAccess = member->object->kind == ExpressionKind::Identifier &&
                    classes.find(static_cast<const IdentifierExpr*>(member->object.get())->name) != classes.end();
                TypePtr objectType = analyzeExpression(member->object.get());
                if (!classAccess && !member->safe && objectType && isNullableType(objectType) && objectType->name != "Any") {
                    error(member->loc, "cannot call member '" + member->member + "' on nullable value of type '" +
                                           typeName(objectType) + "' without '?.'");
                }
                std::string ownerTypeName = classAccess
                    ? static_cast<const IdentifierExpr*>(member->object.get())->name
                    : (objectType ? objectType->name : "");
                if (!ownerTypeName.empty() && ownerTypeName != "Any") {
                    auto classIt = classes.find(ownerTypeName);
                    if (classIt != classes.end()) {
                        std::vector<CallableInfo> candidates = collectClassMethods(classIt->second, member->member, classAccess);
                        if (!candidates.empty()) {
                            const CallableInfo* resolved = resolveBestOverload(
                                candidates,
                                argumentTypes,
                                call->loc,
                                ownerTypeName + "." + member->member,
                                true);
                            if (resolved) {
                                TypePtr returnType = cloneType(resolved->returnType ? resolved->returnType : makeType("Any"));
                                return resolved->isAsync ? makeFutureType(returnType) : returnType;
                            }
                            return makeType("Any");
                        }
                        error(call->loc,
                              std::string(classAccess ? "no accessible static overload for '" : "no accessible overload for '") +
                                  ownerTypeName + "." + member->member + "'");
                        return makeType("Any");
                    }
                    auto ifaceIt = interfaces.find(ownerTypeName);
                    if (ifaceIt != interfaces.end() && !classAccess) {
                        std::vector<CallableInfo> candidates = collectInterfaceMethods(ownerTypeName, member->member);
                        if (!candidates.empty()) {
                            const CallableInfo* resolved = resolveBestOverload(
                                candidates,
                                argumentTypes,
                                call->loc,
                                ownerTypeName + "." + member->member,
                                true);
                            if (resolved) {
                                TypePtr returnType = cloneType(resolved->returnType ? resolved->returnType : makeType("Any"));
                                return resolved->isAsync ? makeFutureType(returnType) : returnType;
                            }
                        }
                    }
                }
            }

            TypePtr calleeType = analyzeExpression(call->callee.get());

            if (call->callee->kind == ExpressionKind::Identifier) {
                const auto* ident = static_cast<const IdentifierExpr*>(call->callee.get());
                if (ident->name == "tuple") {
                    auto tupleType = std::make_shared<Type>("Tuple");
                    for (const auto& argType : argumentTypes) {
                        tupleType->genericArguments.push_back(cloneType(argType ? argType : makeType("Any")));
                    }
                    if (tupleType->genericArguments.empty()) {
                        tupleType->genericArguments.push_back(makeType("Any"));
                    }
                    return tupleType;
                }
                if (ident->name == "iter_range") {
                    if (argumentTypes.size() != 2) {
                        error(call->loc, "iter_range expects 2 arguments");
                    }
                    return makeType("Range");
                }
                if (ident->name == "iter_collect" || ident->name == "iter_take" || ident->name == "iter_skip") {
                    const size_t expectedArity = ident->name == "iter_collect" ? 1u : 2u;
                    if (argumentTypes.size() != expectedArity) {
                        error(call->loc, ident->name + " expects " + std::to_string(expectedArity) + " arguments");
                    }
                    auto listType = std::make_shared<Type>("List");
                    listType->genericArguments.push_back(iterableElementType(argumentTypes.empty() ? makeType("Any") : argumentTypes[0]));
                    return listType;
                }
                if (ident->name == "iter_enumerate") {
                    if (argumentTypes.size() != 1) {
                        error(call->loc, "iter_enumerate expects 1 argument");
                    }
                    auto tupleType = std::make_shared<Type>("Tuple");
                    tupleType->genericArguments.push_back(makeType("Int"));
                    tupleType->genericArguments.push_back(iterableElementType(argumentTypes.empty() ? makeType("Any") : argumentTypes[0]));
                    auto listType = std::make_shared<Type>("List");
                    listType->genericArguments.push_back(tupleType);
                    return listType;
                }
                if (ident->name == "iter_first" || ident->name == "iter_last") {
                    if (argumentTypes.size() != 1) {
                        error(call->loc, ident->name + " expects 1 argument");
                    }
                    return iterableElementType(argumentTypes.empty() ? makeType("Any") : argumentTypes[0]);
                }
                if (ident->name == "iter_map") {
                    if (argumentTypes.size() != 2) {
                        error(call->loc, "iter_map expects 2 arguments");
                        auto listType = std::make_shared<Type>("List");
                        listType->genericArguments.push_back(makeType("Any"));
                        return listType;
                    }
                    TypePtr elementType = iterableElementType(argumentTypes[0]);
                    TypePtr returnType = makeType("Any");
                    auto fnType = argumentTypes[1];
                    if (!fnType || fnType->kind != TypeKind::Function || fnType->parameterTypes.size() != 1) {
                        error(call->arguments[1]->loc, "iter_map expects a function of type (" + typeName(elementType) + ") -> T");
                    } else {
                        if (!isAssignable(fnType->parameterTypes[0], elementType)) {
                            error(call->arguments[1]->loc,
                                  "iter_map callback expects '" + typeName(fnType->parameterTypes[0]) +
                                      "' but iterable yields '" + typeName(elementType) + "'");
                        }
                        returnType = cloneType(fnType->returnType ? fnType->returnType : makeType("Any"));
                    }
                    auto listType = std::make_shared<Type>("List");
                    listType->genericArguments.push_back(returnType);
                    return listType;
                }
                if (ident->name == "iter_filter") {
                    if (argumentTypes.size() != 2) {
                        error(call->loc, "iter_filter expects 2 arguments");
                        auto listType = std::make_shared<Type>("List");
                        listType->genericArguments.push_back(makeType("Any"));
                        return listType;
                    }
                    TypePtr elementType = iterableElementType(argumentTypes[0]);
                    auto fnType = argumentTypes[1];
                    if (!fnType || fnType->kind != TypeKind::Function || fnType->parameterTypes.size() != 1) {
                        error(call->arguments[1]->loc, "iter_filter expects a function of type (" + typeName(elementType) + ") -> Bool");
                    } else {
                        if (!isAssignable(fnType->parameterTypes[0], elementType)) {
                            error(call->arguments[1]->loc,
                                  "iter_filter callback expects '" + typeName(fnType->parameterTypes[0]) +
                                      "' but iterable yields '" + typeName(elementType) + "'");
                        }
                        if (!isAssignable(makeType("Bool"), fnType->returnType ? fnType->returnType : makeType("Any"))) {
                            error(call->arguments[1]->loc, "iter_filter callback must return Bool");
                        }
                    }
                    auto listType = std::make_shared<Type>("List");
                    listType->genericArguments.push_back(elementType);
                    return listType;
                }
                if (ident->name == "iter_fold") {
                    if (argumentTypes.size() != 3) {
                        error(call->loc, "iter_fold expects 3 arguments");
                        return makeType("Any");
                    }
                    TypePtr elementType = iterableElementType(argumentTypes[0]);
                    TypePtr accumulatorType = cloneType(argumentTypes[1] ? argumentTypes[1] : makeType("Any"));
                    auto fnType = argumentTypes[2];
                    if (!fnType || fnType->kind != TypeKind::Function || fnType->parameterTypes.size() != 2) {
                        error(call->arguments[2]->loc,
                              "iter_fold expects a function of type (" + typeName(accumulatorType) +
                                  ", " + typeName(elementType) + ") -> " + typeName(accumulatorType));
                    } else {
                        if (!isAssignable(fnType->parameterTypes[0], accumulatorType)) {
                            error(call->arguments[2]->loc, "iter_fold callback accumulator parameter is incompatible");
                        }
                        if (!isAssignable(fnType->parameterTypes[1], elementType)) {
                            error(call->arguments[2]->loc, "iter_fold callback element parameter is incompatible");
                        }
                        if (!isAssignable(accumulatorType, fnType->returnType ? fnType->returnType : makeType("Any"))) {
                            error(call->arguments[2]->loc, "iter_fold callback return type is incompatible with the accumulator");
                        }
                    }
                    return accumulatorType;
                }
                if (ident->name == "map") {
                    if (!argumentTypes.empty()) {
                        error(call->loc, "map expects 0 arguments");
                    }
                    auto mapType = std::make_shared<Type>("Map");
                    mapType->genericArguments.push_back(makeType("Any"));
                    mapType->genericArguments.push_back(makeType("Any"));
                    return mapType;
                }
                if (ident->name == "set") {
                    if (!argumentTypes.empty()) {
                        error(call->loc, "set expects 0 arguments");
                    }
                    auto setType = std::make_shared<Type>("Set");
                    setType->genericArguments.push_back(makeType("Any"));
                    return setType;
                }
                if (ident->name == "map_get" || ident->name == "map_set" || ident->name == "map_has" ||
                    ident->name == "map_remove" || ident->name == "map_keys" || ident->name == "map_values") {
                    const size_t expectedArity =
                        ident->name == "map_set" ? 3u :
                        (ident->name == "map_keys" || ident->name == "map_values" ? 1u : 2u);
                    if (argumentTypes.size() != expectedArity) {
                        error(call->loc,
                              ident->name + " expects " + std::to_string(expectedArity) + " arguments");
                        if (ident->name == "map_has" || ident->name == "map_remove") {
                            return makeType("Bool");
                        }
                        return makeType("Any");
                    }
                    auto mapType = argumentTypes[0];
                    if (!mapType || mapType->name != "Map" || mapType->genericArguments.size() != 2) {
                        error(call->arguments[0]->loc, ident->name + " expects a Map<K, V> as its first argument");
                        if (ident->name == "map_has" || ident->name == "map_remove") {
                            return makeType("Bool");
                        }
                        return makeType("Any");
                    }
                    if ((ident->name == "map_get" || ident->name == "map_set" || ident->name == "map_has" || ident->name == "map_remove") &&
                        !isAssignable(mapType->genericArguments[0], argumentTypes[1])) {
                        error(call->arguments[1]->loc,
                              "map key has type '" + typeName(argumentTypes[1]) + "', expected '" +
                                  typeName(mapType->genericArguments[0]) + "'");
                    }
                    if (ident->name == "map_set") {
                        if (!isAssignable(mapType->genericArguments[1], argumentTypes[2])) {
                            error(call->arguments[2]->loc,
                                  "map value has type '" + typeName(argumentTypes[2]) + "', expected '" +
                                      typeName(mapType->genericArguments[1]) + "'");
                        }
                        return cloneType(mapType);
                    }
                    if (ident->name == "map_has") {
                        return makeType("Bool");
                    }
                    if (ident->name == "map_remove") {
                        return makeType("Bool");
                    }
                    if (ident->name == "map_keys") {
                        auto listType = std::make_shared<Type>("List");
                        listType->genericArguments.push_back(cloneType(mapType->genericArguments[0]));
                        return listType;
                    }
                    if (ident->name == "map_values") {
                        auto listType = std::make_shared<Type>("List");
                        listType->genericArguments.push_back(cloneType(mapType->genericArguments[1]));
                        return listType;
                    }
                    return cloneType(mapType->genericArguments[1]);
                }
                if (ident->name == "list_push" || ident->name == "list_pop" || ident->name == "list_contains" ||
                    ident->name == "list_first" || ident->name == "list_last") {
                    const size_t expectedArity =
                        ident->name == "list_push" || ident->name == "list_contains" ? 2u : 1u;
                    if (argumentTypes.size() != expectedArity) {
                        error(call->loc, ident->name + " expects " + std::to_string(expectedArity) + " arguments");
                        return ident->name == "list_push" ? makeType("Any") :
                               ident->name == "list_contains" ? makeType("Bool") :
                               makeType("Any");
                    }
                    auto listType = argumentTypes[0];
                    if (!listType || listType->name != "List" || listType->genericArguments.size() != 1) {
                        error(call->arguments[0]->loc, ident->name + " expects a List<T> as its first argument");
                        return ident->name == "list_contains" ? makeType("Bool") : makeType("Any");
                    }
                    if ((ident->name == "list_push" || ident->name == "list_contains") &&
                        !isAssignable(listType->genericArguments[0], argumentTypes[1])) {
                        error(call->arguments[1]->loc,
                              "list value has type '" + typeName(argumentTypes[1]) + "', expected '" +
                                  typeName(listType->genericArguments[0]) + "'");
                    }
                    if (ident->name == "list_contains") {
                        return makeType("Bool");
                    }
                    if (ident->name == "list_push") {
                        return cloneType(listType);
                    }
                    return cloneType(listType->genericArguments[0]);
                }
                if (ident->name == "set_add" || ident->name == "set_has" || ident->name == "set_remove" || ident->name == "set_values") {
                    const size_t expectedArity = ident->name == "set_values" ? 1u : 2u;
                    if (argumentTypes.size() != expectedArity) {
                        error(call->loc, ident->name + " expects " + std::to_string(expectedArity) + " arguments");
                        return ident->name == "set_has" || ident->name == "set_remove" ? makeType("Bool") : makeType("Any");
                    }
                    auto setType = argumentTypes[0];
                    if (!setType || setType->name != "Set" || setType->genericArguments.size() != 1) {
                        error(call->arguments[0]->loc, ident->name + " expects a Set<T> as its first argument");
                        return ident->name == "set_has" || ident->name == "set_remove" ? makeType("Bool") : makeType("Any");
                    }
                    if ((ident->name == "set_add" || ident->name == "set_has" || ident->name == "set_remove") &&
                        !isAssignable(setType->genericArguments[0], argumentTypes[1])) {
                        error(call->arguments[1]->loc,
                              "set value has type '" + typeName(argumentTypes[1]) + "', expected '" +
                                  typeName(setType->genericArguments[0]) + "'");
                    }
                    if (ident->name == "set_has" || ident->name == "set_remove") {
                        return makeType("Bool");
                    }
                    if (ident->name == "set_values") {
                        auto listType = std::make_shared<Type>("List");
                        listType->genericArguments.push_back(cloneType(setType->genericArguments[0]));
                        return listType;
                    }
                    return cloneType(setType);
                }
                const CallableInfo* callable = nullptr;
                if (auto builtinIt = builtins().find(ident->name); builtinIt != builtins().end()) {
                    callable = &builtinIt->second;
                } else if (auto fnIt = functions.find(ident->name); fnIt != functions.end()) {
                    callable = &fnIt->second;
                } else if (auto ctorIt = constructors.find(ident->name); ctorIt != constructors.end()) {
                    auto classIt = classes.find(ident->name);
                    if (classIt != classes.end() && classIt->second->isAbstract) {
                        error(call->loc, "cannot instantiate abstract class '" + ident->name + "'");
                        return makeType(ident->name);
                    }
                    callable = resolveBestOverload(
                        ctorIt->second,
                        argumentTypes,
                        call->loc,
                        ident->name,
                        true);
                }

                if (callable) {
                    std::unordered_set<std::string> genericParameters;
                    for (const auto& generic : callable->genericParameters) {
                        genericParameters.insert(generic.name);
                    }
                    std::unordered_map<std::string, TypePtr> genericMapping;
                    if (expectedType) {
                        TypePtr callableReturn = callable->returnType ? callable->returnType : makeType("Any");
                        inferGenericMapping(callableReturn,
                                            expectedType,
                                            genericParameters,
                                            genericMapping);
                        inferGenericMapping(makeNonNullable(callableReturn),
                                            makeNonNullable(expectedType),
                                            genericParameters,
                                            genericMapping);
                    }
                    const size_t checked = std::min(callable->parameterTypes.size(), argumentTypes.size());
                    for (size_t i = 0; i < checked; ++i) {
                        inferGenericMapping(callable->parameterTypes[i], argumentTypes[i], genericParameters, genericMapping);
                        if (!isAssignable(substituteType(callable->parameterTypes[i], genericMapping), argumentTypes[i])) {
                            error(call->arguments[i]->loc,
                                  "argument " + std::to_string(i + 1) + " to '" + ident->name + "' has type '" +
                                      typeName(argumentTypes[i]) + "', expected '" +
                                      typeName(substituteType(callable->parameterTypes[i], genericMapping)) + "'");
                        }
                    }

                    for (const auto& generic : callable->genericParameters) {
                        auto mappingIt = genericMapping.find(generic.name);
                        if (mappingIt != genericMapping.end() && !satisfiesGenericBounds(mappingIt->second, generic.bounds)) {
                            error(call->loc,
                                  "type argument '" + typeName(mappingIt->second) + "' does not satisfy bounds for '" +
                                      generic.name + "'");
                        }
                    }

                    if (constructors.find(ident->name) != constructors.end()) {
                        if (!canAccessVisibility(callable->visibility, ident->name)) {
                            error(call->loc, "constructor for '" + ident->name + "' is not visible here");
                        }
                        return makeType(ident->name);
                    }

                    TypePtr returnType = substituteType(callable->returnType ? callable->returnType : makeType("Any"), genericMapping);
                    return callable->isAsync ? makeFutureType(returnType) : returnType;
                }
            }

            if (calleeType && calleeType->kind == TypeKind::Function) {
                if (calleeType->parameterTypes.size() != call->arguments.size()) {
                    error(call->loc, "function value expects " + std::to_string(calleeType->parameterTypes.size()) +
                                         " arguments but got " + std::to_string(call->arguments.size()));
                } else {
                    for (size_t i = 0; i < call->arguments.size(); ++i) {
                        if (!isAssignable(calleeType->parameterTypes[i], argumentTypes[i])) {
                            error(call->arguments[i]->loc,
                                  "argument " + std::to_string(i + 1) + " has type '" +
                                      typeName(argumentTypes[i]) + "', expected '" +
                                      typeName(calleeType->parameterTypes[i]) + "'");
                        }
                    }
                }
                return cloneType(calleeType->returnType ? calleeType->returnType : makeType("Any"));
            }

            return makeType("Any");
        }
        case ExpressionKind::Member: {
            auto* member = static_cast<const MemberExpr*>(expression);
            const bool classAccess = member->object->kind == ExpressionKind::Identifier &&
                classes.find(static_cast<const IdentifierExpr*>(member->object.get())->name) != classes.end();
            TypePtr objectType = analyzeExpression(member->object.get());
            if (member->safe && objectType && !objectType->nullable && objectType->name != "Any") {
                error(member->loc, "safe member access is only useful on nullable values");
            }
            if (!classAccess && !member->safe && objectType && isNullableType(objectType) && objectType->name != "Any") {
                error(member->loc, "cannot access member '" + member->member + "' on nullable value of type '" +
                                       typeName(objectType) + "' without '?.'");
            }
            std::string ownerTypeName = classAccess
                ? static_cast<const IdentifierExpr*>(member->object.get())->name
                : (objectType ? objectType->name : "");
            if (!ownerTypeName.empty() && ownerTypeName != "Any") {
                auto classIt = classes.find(ownerTypeName);
                if (classIt != classes.end()) {
                    if (!classAccess) {
                        if (const FieldDecl* field = findClassField(classIt->second, member->member)) {
                            TypePtr result = cloneType(field->declaredType ? field->declaredType : makeType("Any"));
                            if (result) {
                                result->nullable = result->nullable || member->safe;
                            }
                            return result ? result : makeType("Any", member->safe);
                        }
                    }
                    std::vector<CallableInfo> methods = collectClassMethods(classIt->second, member->member, classAccess);
                    if (!methods.empty()) {
                        const auto& method = methods.front();
                        return std::make_shared<Type>(
                            [&]() {
                                std::vector<TypePtr> params;
                                for (const auto& param : method.parameterTypes) {
                                    params.push_back(cloneType(param));
                                }
                                return params;
                            }(),
                            cloneType(method.returnType ? method.returnType : makeType("Any")),
                            member->safe);
                    }
                }
                auto ifaceIt = interfaces.find(ownerTypeName);
                if (ifaceIt != interfaces.end() && !classAccess) {
                    std::vector<CallableInfo> methods = collectInterfaceMethods(ownerTypeName, member->member);
                    if (!methods.empty()) {
                        const auto& method = methods.front();
                        return std::make_shared<Type>(
                            [&]() {
                                std::vector<TypePtr> params;
                                for (const auto& param : method.parameterTypes) {
                                    params.push_back(cloneType(param));
                                }
                                return params;
                            }(),
                            cloneType(method.returnType ? method.returnType : makeType("Any")),
                            member->safe);
                    }
                }
            }
            return makeType("Any", member->safe);
        }
        case ExpressionKind::Index: {
            auto* index = static_cast<const IndexExpr*>(expression);
            TypePtr objectType = analyzeExpression(index->object.get());
            TypePtr indexType = analyzeExpression(index->index.get());
            if (objectType && isNullableType(objectType) && objectType->name != "Any") {
                error(index->loc, "cannot index nullable value of type '" + typeName(objectType) + "'");
            }
            if (objectType->name == "Map" && objectType->genericArguments.size() == 2) {
                if (!isAssignable(objectType->genericArguments[0], indexType)) {
                    error(index->index->loc,
                          "map index has type '" + typeName(indexType) + "', expected '" +
                              typeName(objectType->genericArguments[0]) + "'");
                }
                return cloneType(objectType->genericArguments[1]);
            }
            if (indexType->name != "Int" && indexType->name != "Any") {
                error(index->index->loc, "index expression expects Int, got '" + typeName(indexType) + "'");
            }
            if (objectType->name == "String") {
                return makeType("String");
            }
            if (objectType->name == "List" && !objectType->genericArguments.empty()) {
                return cloneType(objectType->genericArguments.front());
            }
            if (objectType->name == "Tuple" && !objectType->genericArguments.empty()) {
                TypePtr elementType = cloneType(objectType->genericArguments.front());
                for (size_t i = 1; i < objectType->genericArguments.size(); ++i) {
                    if (!isAssignable(elementType, objectType->genericArguments[i]) ||
                        !isAssignable(objectType->genericArguments[i], elementType)) {
                        return makeType("Any");
                    }
                }
                return elementType;
            }
            return makeType("Any");
        }
        case ExpressionKind::ListLiteral: {
            auto* list = static_cast<const ListExpr*>(expression);
            TypePtr elementType;
            for (const auto& element : list->elements) {
                TypePtr current = analyzeExpression(element.get());
                if (!elementType) {
                    elementType = current;
                } else if (!isAssignable(elementType, current)) {
                    elementType = makeType("Any");
                }
            }
            auto listType = std::make_shared<Type>("List");
            listType->genericArguments.push_back(elementType ? elementType : makeType("Any"));
            return listType;
        }
        case ExpressionKind::Assign: {
            auto* assign = static_cast<const AssignExpr*>(expression);
            TypePtr valueType = analyzeExpression(assign->value.get());
            if (assign->target->kind == ExpressionKind::Identifier) {
                auto* ident = static_cast<const IdentifierExpr*>(assign->target.get());
                const Symbol* symbol = resolve(ident->name);
                if (!symbol) {
                    error(assign->loc, "assignment to undefined symbol '" + ident->name + "'");
                    return valueType;
                }
                if (!symbol->mutableBinding) {
                    error(assign->loc, "cannot assign to immutable binding '" + ident->name + "'");
                }
                if (!isAssignable(symbol->type, valueType)) {
                    error(assign->loc, "cannot assign '" + typeName(valueType) + "' to '" + ident->name +
                                           "' of type '" + typeName(symbol->type) + "'");
                }
                return symbol->type ? cloneType(symbol->type) : valueType;
            }
            if (assign->target->kind == ExpressionKind::Member) {
                auto* member = static_cast<const MemberExpr*>(assign->target.get());
                const bool classAccess = member->object->kind == ExpressionKind::Identifier &&
                    classes.find(static_cast<const IdentifierExpr*>(member->object.get())->name) != classes.end();
                TypePtr objectType = analyzeExpression(member->object.get());
                if (classAccess) {
                    error(assign->loc, "cannot assign to static member '" + member->member + "'");
                    return valueType;
                }
                if (objectType && isNullableType(objectType) && objectType->name != "Any") {
                    error(assign->loc, "cannot assign through nullable value of type '" + typeName(objectType) + "'");
                }
                if (objectType && objectType->name != "Any") {
                    auto classIt = classes.find(objectType->name);
                    if (classIt != classes.end()) {
                        if (const FieldDecl* field = findClassField(classIt->second, member->member)) {
                            if (!isAssignable(field->declaredType, valueType)) {
                                error(assign->loc,
                                      "cannot assign '" + typeName(valueType) + "' to field '" + member->member +
                                          "' of type '" + typeName(field->declaredType) + "'");
                            }
                            return cloneType(field->declaredType ? field->declaredType : valueType);
                        }
                    }
                }
                analyzeExpression(assign->target.get());
                return valueType;
            }
            analyzeExpression(assign->target.get());
            return valueType;
        }
        case ExpressionKind::NullCoalesce: {
            auto* coalesce = static_cast<const NullCoalesceExpr*>(expression);
            TypePtr left = analyzeExpression(coalesce->left.get());
            TypePtr right = analyzeExpression(coalesce->right.get());
            if (left->name == "Null") {
                return right;
            }
            auto result = cloneType(left);
            if (result) {
                result->nullable = false;
            }
            return result ? result : right;
        }
        case ExpressionKind::Range: {
            auto* range = static_cast<const RangeExpr*>(expression);
            TypePtr start = analyzeExpression(range->start.get());
            TypePtr end = analyzeExpression(range->end.get());
            if ((start->name != "Int" && start->name != "Any") ||
                (end->name != "Int" && end->name != "Any")) {
                error(range->loc, "range bounds must be Int");
            }
            return makeType("Range");
        }
        case ExpressionKind::NewObject: {
            auto* object = static_cast<const NewExpr*>(expression);
            std::vector<TypePtr> argumentTypes;
            for (const auto& arg : object->arguments) {
                argumentTypes.push_back(analyzeExpression(arg.get()));
            }
            auto it = constructors.find(object->typeName);
            if (it == constructors.end()) {
                error(object->loc, "unknown type '" + object->typeName + "'");
            } else {
                auto classIt = classes.find(object->typeName);
                if (classIt != classes.end() && classIt->second->isAbstract) {
                    error(object->loc, "cannot instantiate abstract class '" + object->typeName + "'");
                    return makeType(object->typeName);
                }
                const CallableInfo* ctor = resolveBestOverload(
                    it->second,
                    argumentTypes,
                    object->loc,
                    object->typeName,
                    true);
                if (ctor && !canAccessVisibility(ctor->visibility, object->typeName)) {
                    error(object->loc, "constructor for '" + object->typeName + "' is not visible here");
                }
            }
            return makeType(object->typeName);
        }
    }

    return makeType("Any");
}

void SemanticAnalyzer::analyzeBlock(const BlockStmt* block) {
    pushScope();
    for (const auto& stmt : block->statements) {
        analyzeStatement(stmt.get());
    }
    popScope();
}

void SemanticAnalyzer::analyzeFunction(const FunctionDecl* function) {
    pushGenericScope(function->genericParameters);
    for (const auto& generic : function->genericParameters) {
        for (const auto& bound : generic.bounds) {
            validateType(bound, generic.loc);
        }
    }
    if (function->returnType) {
        validateType(function->returnType, function->loc);
    }
    for (const auto& param : function->parameters) {
        validateType(param.second ? param.second : makeType("Any"), function->loc);
    }

    const bool previousInFunction = inFunction;
    const bool previousAsync = inAsyncFunction;
    TypePtr previousReturn = currentReturnType;
    inFunction = true;
    inAsyncFunction = function->isAsync;
    currentReturnType = cloneType(function->returnType);

    pushScope();
    for (const auto& param : function->parameters) {
        define(param.first, true, function->loc, param.second ? cloneType(param.second) : makeType("Any"));
    }
    analyzeStatement(function->body.get());
    popScope();
    popGenericScope();

    TypePtr inferredReturn = currentReturnType ? cloneType(currentReturnType) : makeType("Void");
    if (!function->returnType) {
        const_cast<FunctionDecl*>(function)->returnType = cloneType(inferredReturn);
    }
    auto fnIt = functions.find(function->name);
    if (fnIt != functions.end()) {
        fnIt->second.returnType = cloneType(inferredReturn);
    }
    if (!scopes.empty()) {
        auto globalIt = scopes.front().find(function->name);
        if (globalIt != scopes.front().end()) {
            std::vector<TypePtr> parameterTypes;
            for (const auto& param : function->parameters) {
                parameterTypes.push_back(cloneType(param.second ? param.second : makeType("Any")));
            }
            globalIt->second.type = std::make_shared<Type>(std::move(parameterTypes), cloneType(inferredReturn));
        }
    }

    currentReturnType = previousReturn;
    inAsyncFunction = previousAsync;
    inFunction = previousInFunction;
}

void SemanticAnalyzer::analyzeClass(const ClassDecl* klass) {
    pushGenericScope(klass->genericParameters);
    for (const auto& generic : klass->genericParameters) {
        for (const auto& bound : generic.bounds) {
            validateType(bound, generic.loc);
        }
    }
    if (klass->isAbstract && constructors.find(klass->name) == constructors.end()) {
        error(klass->loc, "internal error: missing constructor group for abstract class '" + klass->name + "'");
    }
    std::unordered_set<std::string> fieldNames;
    if (!klass->baseClass.empty()) {
        auto baseIt = classes.find(klass->baseClass);
        if (baseIt == classes.end()) {
            error(klass->loc, "unknown base class '" + klass->baseClass + "'");
        } else if (isSubclassOf(klass->baseClass, klass->name)) {
            error(klass->loc, "inheritance cycle involving '" + klass->name + "'");
        }
        for (const auto* inherited : collectClassFields(baseIt != classes.end() ? baseIt->second : nullptr)) {
            if (inherited) {
                fieldNames.insert(inherited->name);
            }
        }
    }
    for (const auto& field : klass->fields) {
        validateType(field.declaredType ? field.declaredType : makeType("Any"), field.loc);
        if (!fieldNames.insert(field.name).second) {
            error(field.loc, "duplicate field '" + field.name + "' in class '" + klass->name + "'");
        }
    }
    bool seenOptionalField = false;
    for (const auto* field : collectClassFields(klass)) {
        if (!field) {
            continue;
        }
        const bool optional = fieldIsOptional(*field);
        if (optional) {
            seenOptionalField = true;
        } else if (seenOptionalField) {
            error(field->loc, "required field '" + field->name + "' cannot follow an optional field in class '" + klass->name + "'");
        }
    }

    const ClassDecl* previousClass = currentClass;
    currentClass = klass;

    pushScope();
    define("self", false, klass->loc, makeType(klass->name));
    for (const auto* field : collectClassFields(klass)) {
        if (!field) {
            continue;
        }
        define(field->name, true, field->loc, cloneType(field->declaredType ? field->declaredType : makeType("Any")));
    }
    for (const auto* field : collectClassFields(klass)) {
        if (!field || !field->defaultValue) {
            continue;
        }
        TypePtr defaultType = analyzeExpression(field->defaultValue.get());
        if (!isAssignable(field->declaredType, defaultType)) {
            error(field->defaultValue->loc,
                  "default value for field '" + field->name + "' has type '" + typeName(defaultType) +
                      "', expected '" + typeName(field->declaredType) + "'");
        }
    }
    popScope();

    std::unordered_map<std::string, std::vector<CallableInfo>> methodGroups;
    std::vector<CallableInfo> constructorGroups;
    bool hasAbstractMethod = false;
    for (const auto& method : klass->methods) {
        const TypePtr initialMethodReturn = method.isConstructor ? makeType(klass->name) : cloneType(method.returnType ? method.returnType : makeType("Any"));
        CallableInfo currentSignature;
        currentSignature.name = method.isConstructor ? klass->name : method.name;
        currentSignature.arity = method.parameters.size();
        currentSignature.returnType = cloneType(initialMethodReturn);
        currentSignature.genericParameters = method.genericParameters;
        currentSignature.isAsync = method.isAsync;
        currentSignature.isStatic = method.isStatic;
        currentSignature.isConstructor = method.isConstructor;
        currentSignature.isAbstract = method.isAbstract;
        currentSignature.visibility = method.visibility;
        currentSignature.ownerType = klass->name;
        for (const auto& param : method.parameters) {
            currentSignature.parameterTypes.push_back(cloneType(param.second ? param.second : makeType("Any")));
        }
        auto& overloadSet = method.isConstructor ? constructorGroups : methodGroups[method.name];
        for (const auto& existing : overloadSet) {
            if (sameCallableShape(existing, currentSignature)) {
                error(klass->loc,
                      method.isConstructor
                          ? "duplicate constructor overload in class '" + klass->name + "'"
                          : "duplicate method overload '" + method.name + "' in class '" + klass->name + "'");
                break;
            }
        }
        overloadSet.push_back(currentSignature);

        if (method.isConstructor && method.isStatic) {
            error(method.body ? method.body->loc : klass->loc, "constructors cannot be static");
        }
        if (method.isConstructor && method.isAbstract) {
            error(klass->loc, "constructors cannot be abstract");
        }
        if (method.isConstructor && method.isAsync) {
            error(method.body ? method.body->loc : klass->loc, "constructors cannot be async");
        }
        if (method.isConstructor && method.returnType && method.returnType->name != "Void") {
            error(klass->loc, "constructors cannot declare an explicit return type");
        }
        if (method.isAbstract && method.isStatic) {
            error(klass->loc, "static methods cannot be abstract");
        }
        if (method.isAbstract && method.body) {
            error(klass->loc, "abstract method '" + method.name + "' must not have a body");
        }
        if (!method.isAbstract && !method.body) {
            error(klass->loc, "method '" + method.name + "' is missing a body");
        }
        if (method.isAbstract && !klass->isAbstract) {
            error(klass->loc, "class '" + klass->name + "' must be abstract to declare abstract methods");
        }
        hasAbstractMethod = hasAbstractMethod || method.isAbstract;

        pushGenericScope(method.genericParameters);
        for (const auto& generic : method.genericParameters) {
            for (const auto& bound : generic.bounds) {
                validateType(bound, generic.loc);
            }
        }
        if (method.isConstructor) {
            validateType(makeType("Void"), klass->loc);
        } else if (method.returnType) {
            validateType(method.returnType, klass->loc);
        }

        const bool previousInFunction = inFunction;
        const bool previousAsync = inAsyncFunction;
        const bool previousStatic = inStaticMethod;
        TypePtr previousReturn = currentReturnType;
        inFunction = true;
        inAsyncFunction = method.isAsync;
        inStaticMethod = method.isStatic;
        currentReturnType = method.isConstructor
            ? makeType("Void")
            : cloneType(method.returnType);

        pushScope();
        if (!method.isStatic) {
            define("self", false, klass->loc, makeType(klass->name));
            if (!klass->baseClass.empty()) {
                define("super", false, klass->loc, makeType(klass->baseClass));
            }
            for (const auto* field : collectClassFields(klass)) {
                if (!field) {
                    continue;
                }
                define(field->name, true, field->loc, cloneType(field->declaredType ? field->declaredType : makeType("Any")));
            }
        }
        for (const auto& param : method.parameters) {
            validateType(param.second ? param.second : makeType("Any"), klass->loc);
            define(param.first, true, klass->loc, param.second ? cloneType(param.second) : makeType("Any"));
        }
        if (method.body) {
            analyzeStatement(method.body.get());
        }
        popScope();

        if (!method.isConstructor) {
            TypePtr inferredMethodReturn = currentReturnType ? cloneType(currentReturnType) : makeType("Void");
            if (!method.returnType) {
                const_cast<MethodDecl&>(method).returnType = cloneType(inferredMethodReturn);
            }
            overloadSet.back().returnType = cloneType(inferredMethodReturn);
        }

        currentReturnType = previousReturn;
        inAsyncFunction = previousAsync;
        inStaticMethod = previousStatic;
        inFunction = previousInFunction;
        popGenericScope();
    }

    if (hasAbstractMethod && !klass->isAbstract) {
        error(klass->loc, "class '" + klass->name + "' must be declared abstract");
    }

        for (const auto& ifaceName : klass->implementedInterfaces) {
        auto ifaceIt = interfaces.find(ifaceName);
        if (ifaceIt == interfaces.end()) {
            error(klass->loc, "unknown interface '" + ifaceName + "'");
            continue;
        }
        for (const auto& requirement : ifaceIt->second.methods) {
            for (const auto& contract : collectInterfaceMethods(ifaceName, requirement.first)) {
                std::vector<CallableInfo> availableMethods = collectClassMethods(klass, requirement.first, contract.isStatic);
                if (availableMethods.empty()) {
                    if (!klass->isAbstract) {
                        error(klass->loc, "class '" + klass->name + "' does not implement method '" +
                                              requirement.first + "' required by interface '" + ifaceName + "'");
                    }
                    continue;
                }
                const CallableInfo* matched = resolveBestOverload(
                    availableMethods,
                    contract.parameterTypes,
                    klass->loc,
                    klass->name + "." + requirement.first,
                    false);
                if (!matched || matched->isAbstract) {
                    if (!klass->isAbstract) {
                        error(klass->loc, "class '" + klass->name + "' does not provide a matching overload for method '" +
                                              requirement.first + "' required by interface '" + ifaceName + "'");
                    }
                    continue;
                }
                if (!signatureCompatible(*matched, contract, true)) {
                    error(klass->loc, "class '" + klass->name + "' method '" + requirement.first +
                                          "' is not signature-compatible with interface '" + ifaceName + "'");
                }
            }
        }
    }

    std::unordered_map<std::string, std::vector<CallableInfo>> requiredAbstractMethods;
    if (!klass->baseClass.empty()) {
        auto baseIt = classes.find(klass->baseClass);
        if (baseIt != classes.end()) {
            for (const auto& method : klass->methods) {
                if (method.isConstructor) {
                    continue;
                }
                CallableInfo implementation;
                implementation.name = method.name;
                implementation.arity = method.parameters.size();
                implementation.returnType = cloneType(method.returnType ? method.returnType : makeType("Any"));
                implementation.genericParameters = method.genericParameters;
                implementation.isAsync = method.isAsync;
                implementation.isStatic = method.isStatic;
                implementation.isConstructor = false;
                implementation.isAbstract = method.isAbstract;
                implementation.visibility = method.visibility;
                implementation.ownerType = klass->name;
                for (const auto& param : method.parameters) {
                    implementation.parameterTypes.push_back(cloneType(param.second ? param.second : makeType("Any")));
                }

                auto inherited = collectClassMethods(baseIt->second, method.name, method.isStatic);
                for (const auto& candidate : inherited) {
                    if (!sameCallableShape(candidate, implementation)) {
                        continue;
                    }
                    if (!signatureCompatible(implementation, candidate, true)) {
                        error(klass->loc,
                              "method '" + method.name + "' in class '" + klass->name +
                                  "' is not signature-compatible with inherited method from '" +
                                  candidate.ownerType + "'");
                    }
                    break;
                }
            }
            for (const auto& method : baseIt->second->methods) {
                if (method.isAbstract) {
                    CallableInfo info;
                    info.name = method.name;
                    info.arity = method.parameters.size();
                    info.returnType = cloneType(method.returnType ? method.returnType : makeType("Any"));
                    info.genericParameters = method.genericParameters;
                    info.isAsync = method.isAsync;
                    info.isStatic = method.isStatic;
                    info.isConstructor = method.isConstructor;
                    info.isAbstract = true;
                    info.visibility = method.visibility;
                    info.ownerType = baseIt->second->name;
                    for (const auto& param : method.parameters) {
                        info.parameterTypes.push_back(cloneType(param.second ? param.second : makeType("Any")));
                    }
                    requiredAbstractMethods[method.name].push_back(std::move(info));
                }
            }
        }
    }

    for (const auto& requirementGroup : requiredAbstractMethods) {
        for (const auto& requirement : requirementGroup.second) {
            auto availableMethods = collectClassMethods(klass, requirement.name, requirement.isStatic);
            const CallableInfo* matched = resolveBestOverload(
                availableMethods,
                requirement.parameterTypes,
                klass->loc,
                klass->name + "." + requirement.name,
                false);
            if (!matched || matched->isAbstract) {
                if (!klass->isAbstract) {
                    error(klass->loc,
                          "class '" + klass->name + "' does not implement abstract method '" + requirement.name + "'");
                }
            } else if (!signatureCompatible(*matched, requirement, true)) {
                error(klass->loc,
                      "class '" + klass->name + "' method '" + requirement.name +
                          "' is not signature-compatible with inherited abstract method");
            }
        }
    }

    currentClass = previousClass;
    popGenericScope();
}

void SemanticAnalyzer::analyzeData(const DataDecl* data) {
    pushGenericScope(data->genericParameters);
    for (const auto& generic : data->genericParameters) {
        for (const auto& bound : generic.bounds) {
            validateType(bound, generic.loc);
        }
    }
    std::unordered_set<std::string> fieldNames;
    bool seenOptionalField = false;
    for (const auto& field : data->fields) {
        validateType(field.declaredType ? field.declaredType : makeType("Any"), field.loc);
        if (!fieldNames.insert(field.name).second) {
            error(field.loc, "duplicate field '" + field.name + "' in data type '" + data->name + "'");
        }
        const bool optional = fieldIsOptional(field);
        if (optional) {
            seenOptionalField = true;
        } else if (seenOptionalField) {
            error(field.loc, "required field '" + field.name + "' cannot follow an optional field in data type '" + data->name + "'");
        }
    }

    pushScope();
    define("self", false, data->loc, makeType(data->name));
    for (const auto& field : data->fields) {
        define(field.name, true, field.loc, cloneType(field.declaredType ? field.declaredType : makeType("Any")));
        if (!field.defaultValue) {
            continue;
        }
        TypePtr defaultType = analyzeExpression(field.defaultValue.get());
        if (!isAssignable(field.declaredType, defaultType)) {
            error(field.defaultValue->loc,
                  "default value for field '" + field.name + "' has type '" + typeName(defaultType) +
                      "', expected '" + typeName(field.declaredType) + "'");
        }
    }
    popScope();
    popGenericScope();
}

void SemanticAnalyzer::pushScope() {
    scopes.emplace_back();
}

void SemanticAnalyzer::popScope() {
    scopes.pop_back();
}

void SemanticAnalyzer::pushFlowScope(const FlowNarrowing& narrowing) {
    pushScope();
    for (const auto& entry : narrowing) {
        if (const Symbol* symbol = resolve(entry.first)) {
            scopes.back()[entry.first] = Symbol{symbol->mutableBinding, cloneType(entry.second)};
        }
    }
}

void SemanticAnalyzer::pushGenericScope(const std::vector<GenericParamDecl>& genericParameters) {
    genericScopes.emplace_back();
    for (const auto& parameter : genericParameters) {
        genericScopes.back()[parameter.name] = parameter.bounds;
    }
}

void SemanticAnalyzer::popGenericScope() {
    if (!genericScopes.empty()) {
        genericScopes.pop_back();
    }
}

bool SemanticAnalyzer::define(const std::string& name, bool mutableBinding, const SourceLocation& loc, TypePtr type) {
    if (scopes.empty()) {
        pushScope();
    }
    auto& scope = scopes.back();
    if (scope.find(name) != scope.end()) {
        error(loc, "duplicate symbol '" + name + "'");
        return false;
    }
    scope.emplace(name, Symbol{mutableBinding, std::move(type)});
    return true;
}

const SemanticAnalyzer::Symbol* SemanticAnalyzer::resolve(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto symbol = it->find(name);
        if (symbol != it->end()) {
            return &symbol->second;
        }
    }
    return nullptr;
}

bool SemanticAnalyzer::isSubclassOf(const std::string& derived, const std::string& base) const {
    if (derived.empty() || base.empty()) {
        return false;
    }
    std::string current = derived;
    std::unordered_set<std::string> seen;
    while (!current.empty() && seen.insert(current).second) {
        if (current == base) {
            return true;
        }
        auto it = classes.find(current);
        if (it == classes.end()) {
            break;
        }
        current = it->second->baseClass;
    }
    return false;
}

std::vector<const FieldDecl*> SemanticAnalyzer::collectClassFields(const ClassDecl* klass) const {
    if (!klass) {
        return {};
    }
    std::vector<const FieldDecl*> fields;
    if (!klass->baseClass.empty()) {
        auto baseIt = classes.find(klass->baseClass);
        if (baseIt != classes.end()) {
            fields = collectClassFields(baseIt->second);
        }
    }
    for (const auto& field : klass->fields) {
        fields.push_back(&field);
    }
    return fields;
}

const FieldDecl* SemanticAnalyzer::findClassField(const ClassDecl* klass, const std::string& fieldName) const {
    for (const auto* field : collectClassFields(klass)) {
        if (field && field->name == fieldName) {
            return field;
        }
    }
    return nullptr;
}

std::vector<SemanticAnalyzer::CallableInfo> SemanticAnalyzer::collectClassMethods(
    const ClassDecl* klass,
    const std::string& methodName,
    bool staticOnly) const {
    std::vector<CallableInfo> methods;
    if (!klass) {
        return methods;
    }
    if (!klass->baseClass.empty()) {
        auto baseIt = classes.find(klass->baseClass);
        if (baseIt != classes.end()) {
            methods = collectClassMethods(baseIt->second, methodName, staticOnly);
            methods.erase(
                std::remove_if(
                    methods.begin(),
                    methods.end(),
                    [&](const CallableInfo& info) { return info.visibility == Visibility::Private; }),
                methods.end());
        }
    }
    for (const auto& method : klass->methods) {
        if (method.name != methodName || method.isConstructor || method.isStatic != staticOnly) {
            continue;
        }
        if (!canAccessVisibility(method.visibility, klass->name)) {
            continue;
        }
        CallableInfo info;
        info.name = method.name;
        info.arity = method.parameters.size();
        info.returnType = cloneType(method.returnType ? method.returnType : makeType("Any"));
        info.genericParameters = method.genericParameters;
        info.isAsync = method.isAsync;
        info.isStatic = method.isStatic;
        info.isConstructor = method.isConstructor;
        info.isAbstract = method.isAbstract;
        info.visibility = method.visibility;
        info.ownerType = klass->name;
        for (const auto& param : method.parameters) {
            info.parameterTypes.push_back(cloneType(param.second ? param.second : makeType("Any")));
        }
        methods.erase(
            std::remove_if(
                methods.begin(),
                methods.end(),
                [&](const CallableInfo& existing) { return sameCallableShape(existing, info); }),
            methods.end());
        methods.push_back(std::move(info));
    }
    return methods;
}

std::vector<SemanticAnalyzer::CallableInfo> SemanticAnalyzer::collectInterfaceMethods(
    const std::string& interfaceName,
    const std::string& methodName) const {
    std::vector<CallableInfo> methods;
    auto ifaceIt = interfaces.find(interfaceName);
    if (ifaceIt == interfaces.end()) {
        return methods;
    }
    for (const auto& base : ifaceIt->second.baseInterfaces) {
        auto inherited = collectInterfaceMethods(base, methodName);
        methods.insert(methods.end(), inherited.begin(), inherited.end());
    }
    auto methodIt = ifaceIt->second.methods.find(methodName);
    if (methodIt != ifaceIt->second.methods.end()) {
        for (const auto& method : methodIt->second) {
            methods.erase(
                std::remove_if(
                    methods.begin(),
                    methods.end(),
                    [&](const CallableInfo& existing) { return sameCallableShape(existing, method); }),
                methods.end());
            methods.push_back(method);
        }
    }
    return methods;
}

const SemanticAnalyzer::CallableInfo* SemanticAnalyzer::resolveBestOverload(
    const std::vector<CallableInfo>& candidates,
    const std::vector<TypePtr>& argumentTypes,
    const SourceLocation& loc,
    const std::string& displayName,
    bool reportErrors) {
    const CallableInfo* best = nullptr;
    int bestScore = -1;
    bool ambiguous = false;

    for (const auto& candidate : candidates) {
        if (candidate.arity != kVariadicArity && candidate.arity != argumentTypes.size()) {
            continue;
        }
        std::unordered_set<std::string> candidateGenerics;
        for (const auto& generic : candidate.genericParameters) {
            candidateGenerics.insert(generic.name);
        }
        std::function<bool(const TypePtr&, const TypePtr&)> parameterMatches =
            [&](const TypePtr& parameterType, const TypePtr& argumentType) -> bool {
                if (!parameterType || !argumentType) {
                    return true;
                }
                if (parameterType->kind == TypeKind::Function || argumentType->kind == TypeKind::Function) {
                    if (parameterType->kind != TypeKind::Function || argumentType->kind != TypeKind::Function ||
                        parameterType->parameterTypes.size() != argumentType->parameterTypes.size()) {
                        return false;
                    }
                    for (size_t i = 0; i < parameterType->parameterTypes.size(); ++i) {
                        if (!parameterMatches(parameterType->parameterTypes[i], argumentType->parameterTypes[i])) {
                            return false;
                        }
                    }
                    return parameterMatches(parameterType->returnType, argumentType->returnType);
                }
                if (parameterType->kind == TypeKind::Union || parameterType->kind == TypeKind::Intersection ||
                    argumentType->kind == TypeKind::Union || argumentType->kind == TypeKind::Intersection) {
                    return isAssignable(parameterType, argumentType);
                }
                if (candidateGenerics.find(parameterType->name) != candidateGenerics.end() &&
                    parameterType->genericArguments.empty()) {
                    return true;
                }
                if (parameterType->name == argumentType->name &&
                    parameterType->genericArguments.size() == argumentType->genericArguments.size()) {
                    for (size_t i = 0; i < parameterType->genericArguments.size(); ++i) {
                        if (!parameterMatches(parameterType->genericArguments[i], argumentType->genericArguments[i])) {
                            return false;
                        }
                    }
                    return true;
                }
                return isAssignable(parameterType, argumentType);
            };
        int score = 0;
        bool matches = true;
        for (size_t i = 0; i < std::min(candidate.parameterTypes.size(), argumentTypes.size()); ++i) {
            if (!parameterMatches(candidate.parameterTypes[i], argumentTypes[i])) {
                matches = false;
                break;
            }
            const bool exactForward = isAssignable(candidate.parameterTypes[i], argumentTypes[i]);
            const bool exactReverse = isAssignable(argumentTypes[i], candidate.parameterTypes[i]);
            score += (exactForward && exactReverse) ? 2 : 1;
        }
        if (!matches) {
            continue;
        }
        if (score > bestScore) {
            best = &candidate;
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore) {
            ambiguous = true;
        }
    }

    if (ambiguous) {
        if (reportErrors) {
            error(loc, "ambiguous overload for '" + displayName + "'");
        }
        return nullptr;
    }
    if (!best && reportErrors) {
        error(loc, "no matching overload for '" + displayName + "'");
    }
    return best;
}

bool SemanticAnalyzer::sameSignature(const CallableInfo& left, const CallableInfo& right) const {
    if (!sameCallableShape(left, right)) {
        return false;
    }
    return typeEquivalent(left.returnType, right.returnType);
}

bool SemanticAnalyzer::sameCallableShape(const CallableInfo& left, const CallableInfo& right) const {
    if (left.name != right.name ||
        left.arity != right.arity ||
        left.isAsync != right.isAsync ||
        left.isStatic != right.isStatic ||
        left.isConstructor != right.isConstructor ||
        left.genericParameters.size() != right.genericParameters.size() ||
        left.parameterTypes.size() != right.parameterTypes.size()) {
        return false;
    }
    for (size_t i = 0; i < left.parameterTypes.size(); ++i) {
        if (!typeEquivalent(left.parameterTypes[i], right.parameterTypes[i])) {
            return false;
        }
    }
    return true;
}

bool SemanticAnalyzer::typeEquivalent(const TypePtr& left, const TypePtr& right) const {
    return isAssignable(left, right) && isAssignable(right, left);
}

bool SemanticAnalyzer::signatureCompatible(
    const CallableInfo& implementation,
    const CallableInfo& contract,
    bool allowCovariantReturn) const {
    if (implementation.name != contract.name ||
        implementation.arity != contract.arity ||
        implementation.isAsync != contract.isAsync ||
        implementation.isStatic != contract.isStatic ||
        implementation.isConstructor != contract.isConstructor ||
        implementation.genericParameters.size() != contract.genericParameters.size() ||
        implementation.parameterTypes.size() != contract.parameterTypes.size()) {
        return false;
    }
    for (size_t i = 0; i < implementation.parameterTypes.size(); ++i) {
        if (!typeEquivalent(implementation.parameterTypes[i], contract.parameterTypes[i])) {
            return false;
        }
    }
    if (allowCovariantReturn) {
        return isAssignable(contract.returnType, implementation.returnType);
    }
    return typeEquivalent(implementation.returnType, contract.returnType);
}

bool SemanticAnalyzer::canAccessVisibility(Visibility visibility, const std::string& ownerType) const {
    if (visibility == Visibility::Public) {
        return true;
    }
    return currentClass && currentClass->name == ownerType;
}

void SemanticAnalyzer::error(const SourceLocation& loc, const std::string& message) {
    errors.push_back(loc.filename + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column) + ": " + message);
}

bool SemanticAnalyzer::isKnownCallable(const std::string& name, size_t* arity) const {
    if (auto it = functions.find(name); it != functions.end()) {
        if (arity) {
            *arity = it->second.arity;
        }
        return true;
    }
    if (auto it = constructors.find(name); it != constructors.end()) {
        if (arity) {
            *arity = it->second.empty() ? 0 : it->second.front().arity;
        }
        return true;
    }
    if (auto it = builtins().find(name); it != builtins().end()) {
        if (arity) {
            *arity = it->second.arity;
        }
        return true;
    }
    return false;
}

bool SemanticAnalyzer::isGenericParameter(const std::string& name) const {
    for (auto it = genericScopes.rbegin(); it != genericScopes.rend(); ++it) {
        if (it->find(name) != it->end()) {
            return true;
        }
    }
    return false;
}

const std::vector<TypePtr>* SemanticAnalyzer::genericBounds(const std::string& name) const {
    for (auto it = genericScopes.rbegin(); it != genericScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

TypePtr SemanticAnalyzer::expandAliases(const TypePtr& type) const {
    std::vector<std::string> activeAliases;
    return expandAliases(type, activeAliases);
}

TypePtr SemanticAnalyzer::expandAliases(const TypePtr& type, std::vector<std::string>& activeAliases) const {
    if (!type) {
        return nullptr;
    }
    if (type->kind == TypeKind::Function) {
        std::vector<TypePtr> parameters;
        for (const auto& parameter : type->parameterTypes) {
            auto expandedParameter = expandAliases(parameter, activeAliases);
            if (!expandedParameter) {
                return nullptr;
            }
            parameters.push_back(expandedParameter);
        }
        auto expandedReturn = expandAliases(type->returnType, activeAliases);
        if (!expandedReturn) {
            return nullptr;
        }
        auto expanded = std::make_shared<Type>(std::move(parameters), expandedReturn, type->nullable);
        return expanded;
    }
    if (type->kind == TypeKind::Union || type->kind == TypeKind::Intersection) {
        std::vector<TypePtr> members;
        for (const auto& member : type->members) {
            auto expandedMember = expandAliases(member, activeAliases);
            if (!expandedMember) {
                return nullptr;
            }
            members.push_back(expandedMember);
        }
        return std::make_shared<Type>(type->kind, std::move(members), type->nullable);
    }
    if (isGenericParameter(type->name)) {
        return cloneType(type);
    }

    auto aliasIt = typeAliases.find(type->name);
    if (aliasIt == typeAliases.end()) {
        std::vector<TypePtr> arguments;
        for (const auto& argument : type->genericArguments) {
            arguments.push_back(expandAliases(argument, activeAliases));
        }
        return std::make_shared<Type>(type->name, std::move(arguments), type->nullable);
    }

    if (std::find(activeAliases.begin(), activeAliases.end(), type->name) != activeAliases.end()) {
        return nullptr;
    }

    std::unordered_map<std::string, TypePtr> mapping;
    for (size_t i = 0; i < aliasIt->second.genericParameters.size() && i < type->genericArguments.size(); ++i) {
        mapping.emplace(aliasIt->second.genericParameters[i].name, cloneType(type->genericArguments[i]));
    }

    activeAliases.push_back(type->name);
    auto substituted = substituteType(aliasIt->second.aliasedType, mapping);
    auto expanded = expandAliases(substituted, activeAliases);
    activeAliases.pop_back();
    if (!expanded) {
        return nullptr;
    }
    expanded->nullable = expanded->nullable || type->nullable;
    return expanded;
}

TypePtr SemanticAnalyzer::makeNonNullable(const TypePtr& type) const {
    TypePtr result = expandAliases(type);
    if (!result) {
        result = cloneType(type);
    }
    if (!result) {
        return nullptr;
    }
    result->nullable = false;
    if (result->name == "Null" && result->kind == TypeKind::Named) {
        return makeType("Any");
    }
    if (result->kind == TypeKind::Union) {
        std::vector<TypePtr> members;
        for (const auto& member : result->members) {
            if (member && member->kind == TypeKind::Named && member->name == "Null") {
                continue;
            }
            auto narrowed = makeNonNullable(member);
            if (narrowed) {
                members.push_back(narrowed);
            }
        }
        if (members.empty()) {
            return makeType("Any");
        }
        if (members.size() == 1) {
            return members.front();
        }
        return std::make_shared<Type>(TypeKind::Union, std::move(members), false);
    }
    return result;
}

bool SemanticAnalyzer::isNullableType(const TypePtr& type) const {
    TypePtr expanded = expandAliases(type);
    if (!expanded) {
        expanded = cloneType(type);
    }
    if (!expanded) {
        return false;
    }
    if (expanded->nullable || expanded->name == "Any" || expanded->name == "Null") {
        return true;
    }
    if (expanded->kind == TypeKind::Union) {
        for (const auto& member : expanded->members) {
            if (member && member->kind == TypeKind::Named && member->name == "Null") {
                return true;
            }
            if (member && member->nullable) {
                return true;
            }
        }
    }
    return false;
}

void SemanticAnalyzer::collectFlowNarrowing(const Expression* expression, bool truthy, FlowNarrowing& narrowing) const {
    if (!expression) {
        return;
    }
    switch (expression->kind) {
        case ExpressionKind::Identifier: {
            if (!truthy) {
                return;
            }
            const auto* ident = static_cast<const IdentifierExpr*>(expression);
            if (const Symbol* symbol = resolve(ident->name)) {
                if (isNullableType(symbol->type)) {
                    narrowing[ident->name] = makeNonNullable(symbol->type);
                }
            }
            return;
        }
        case ExpressionKind::Unary: {
            const auto* unary = static_cast<const UnaryExpr*>(expression);
            if (unary->op.type == TokenType::BANG || unary->op.type == TokenType::NOT) {
                collectFlowNarrowing(unary->operand.get(), !truthy, narrowing);
            }
            return;
        }
        case ExpressionKind::TypeCheck: {
            if (!truthy) {
                return;
            }
            const auto* typeCheck = static_cast<const TypeCheckExpr*>(expression);
            if (typeCheck->operand->kind != ExpressionKind::Identifier) {
                return;
            }
            const auto* ident = static_cast<const IdentifierExpr*>(typeCheck->operand.get());
            if (!supportsRuntimeTypeCheck(typeCheck->targetType)) {
                return;
            }
            narrowing[ident->name] = cloneType(typeCheck->targetType ? typeCheck->targetType : makeType("Any"));
            return;
        }
        case ExpressionKind::Binary: {
            const auto* binary = static_cast<const BinaryExpr*>(expression);
            if (binary->op.type == TokenType::AND) {
                if (truthy) {
                    collectFlowNarrowing(binary->left.get(), true, narrowing);
                    collectFlowNarrowing(binary->right.get(), true, narrowing);
                }
                return;
            }
            if (binary->op.type == TokenType::OR) {
                if (!truthy) {
                    collectFlowNarrowing(binary->left.get(), false, narrowing);
                    collectFlowNarrowing(binary->right.get(), false, narrowing);
                }
                return;
            }
            if (binary->op.type != TokenType::EQ_EQ && binary->op.type != TokenType::BANG_EQ) {
                return;
            }
            const bool equalComparison = binary->op.type == TokenType::EQ_EQ;
            const Expression* identifierExpr = nullptr;
            const Expression* nullExpr = nullptr;
            if (binary->left->kind == ExpressionKind::Identifier && binary->right->kind == ExpressionKind::Literal) {
                const auto* literal = static_cast<const LiteralExpr*>(binary->right.get());
                if (literal->tokenType == TokenType::NULL_KW) {
                    identifierExpr = binary->left.get();
                    nullExpr = binary->right.get();
                }
            } else if (binary->right->kind == ExpressionKind::Identifier && binary->left->kind == ExpressionKind::Literal) {
                const auto* literal = static_cast<const LiteralExpr*>(binary->left.get());
                if (literal->tokenType == TokenType::NULL_KW) {
                    identifierExpr = binary->right.get();
                    nullExpr = binary->left.get();
                }
            }
            if (identifierExpr && nullExpr) {
                const bool narrowsNonNull = truthy ? !equalComparison : equalComparison;
                if (narrowsNonNull) {
                    const auto* ident = static_cast<const IdentifierExpr*>(identifierExpr);
                    if (const Symbol* symbol = resolve(ident->name)) {
                        if (isNullableType(symbol->type)) {
                            narrowing[ident->name] = makeNonNullable(symbol->type);
                        }
                    }
                }
            }
            return;
        }
        default:
            return;
    }
}

SemanticAnalyzer::FlowNarrowing SemanticAnalyzer::mergeFlowNarrowings(
    const FlowNarrowing& left,
    const FlowNarrowing& right) const {
    FlowNarrowing merged;
    for (const auto& entry : left) {
        auto it = right.find(entry.first);
        if (it != right.end() && typeName(entry.second) == typeName(it->second)) {
            merged[entry.first] = cloneType(entry.second);
        }
    }
    return merged;
}

bool SemanticAnalyzer::validateType(const TypePtr& type, const SourceLocation& loc) {
    if (!type) {
        return true;
    }
    if (type->kind == TypeKind::Function) {
        for (const auto& param : type->parameterTypes) {
            validateType(param, loc);
        }
        validateType(type->returnType ? type->returnType : makeType("Void"), loc);
        return true;
    }
    if (type->kind == TypeKind::Union || type->kind == TypeKind::Intersection) {
        bool valid = true;
        if (type->members.size() < 2) {
            error(loc, "composite types require at least two members");
            valid = false;
        }
        for (const auto& member : type->members) {
            valid = validateType(member, loc) && valid;
        }
        return valid;
    }
    if (!isGenericParameter(type->name)) {
        auto known = knownTypes.find(type->name);
        if (known == knownTypes.end()) {
            error(loc, "unknown type '" + type->name + "'");
            return false;
        }
        if (type->name == "Tuple") {
            if (type->genericArguments.empty()) {
                error(loc, "type 'Tuple' expects at least one generic argument");
                return false;
            }
        } else if (known->second != type->genericArguments.size()) {
            error(loc, "type '" + type->name + "' expects " + std::to_string(known->second) +
                           " generic arguments but got " + std::to_string(type->genericArguments.size()));
            return false;
        }
        for (const auto& argument : type->genericArguments) {
            validateType(argument, loc);
        }
        auto genericInfo = typeGenericParameters.find(type->name);
        if (genericInfo != typeGenericParameters.end()) {
            for (size_t i = 0; i < type->genericArguments.size() && i < genericInfo->second.size(); ++i) {
                if (!satisfiesGenericBounds(type->genericArguments[i], genericInfo->second[i].bounds)) {
                    error(loc,
                          "type argument '" + typeName(type->genericArguments[i]) + "' does not satisfy bounds for '" +
                              genericInfo->second[i].name + "'");
                    return false;
                }
            }
        }
        if (typeAliases.find(type->name) != typeAliases.end()) {
            std::vector<std::string> activeAliases{type->name};
            std::unordered_map<std::string, TypePtr> mapping;
            const auto& aliasInfo = typeAliases.at(type->name);
            for (size_t i = 0; i < aliasInfo.genericParameters.size() && i < type->genericArguments.size(); ++i) {
                mapping.emplace(aliasInfo.genericParameters[i].name, cloneType(type->genericArguments[i]));
            }
            auto expanded = expandAliases(substituteType(aliasInfo.aliasedType, mapping), activeAliases);
            if (!expanded) {
                error(loc, "cyclic type alias '" + type->name + "'");
                return false;
            }
            return validateType(expanded, loc);
        }
    }
    return true;
}

bool SemanticAnalyzer::satisfiesGenericBounds(const TypePtr& type, const std::vector<TypePtr>& bounds) const {
    for (const auto& bound : bounds) {
        if (!isAssignable(bound, type)) {
            return false;
        }
    }
    return true;
}

bool SemanticAnalyzer::supportsRuntimeTypeCheck(const TypePtr& type) const {
    TypePtr expanded = expandAliases(type);
    if (!expanded) {
        return false;
    }
    if (expanded->kind == TypeKind::Union || expanded->kind == TypeKind::Intersection) {
        return false;
    }
    if (expanded->kind == TypeKind::Function) {
        return true;
    }
    if (expanded->name == "Void") {
        return false;
    }
    return true;
}

TypePtr SemanticAnalyzer::makeSafeCastResult(const TypePtr& target) const {
    TypePtr result = cloneType(target ? target : makeType("Any"));
    if (!result) {
        return makeType("Any");
    }
    if (result->name != "Null" && result->name != "Any") {
        result->nullable = true;
    }
    return result;
}

bool SemanticAnalyzer::doesCaseCoverType(const MatchCase& matchCase, const TypePtr& candidateType) const {
    if (matchCase.kind == MatchPatternKind::Wildcard) {
        return true;
    }
    if (!candidateType) {
        return false;
    }
    if (matchCase.kind == MatchPatternKind::Type) {
        TypePtr patternType = std::make_shared<Type>(matchCase.typeName);
        return isAssignable(patternType, candidateType);
    }
    if (!matchCase.pattern) {
        return false;
    }
    if (candidateType->name == "Bool" &&
        matchCase.pattern->kind == ExpressionKind::Literal &&
        static_cast<const LiteralExpr*>(matchCase.pattern.get())->tokenType == TokenType::TRUE) {
        return true;
    }
    if (candidateType->name == "Null" &&
        matchCase.pattern->kind == ExpressionKind::Literal &&
        static_cast<const LiteralExpr*>(matchCase.pattern.get())->tokenType == TokenType::NULL_KW) {
        return true;
    }
    return false;
}

bool SemanticAnalyzer::isExhaustiveMatch(const TypePtr& matchedType, const MatchStmt* matchStmt) const {
    if (!matchStmt || !matchedType) {
        return true;
    }
    for (const auto& matchCase : matchStmt->cases) {
        if (matchCase.kind == MatchPatternKind::Wildcard) {
            return true;
        }
    }

    TypePtr expanded = expandAliases(matchedType);
    if (!expanded) {
        expanded = matchedType;
    }
    if (!expanded || expanded->name == "Any") {
        return true;
    }

    if (expanded->kind == TypeKind::Union) {
        for (const auto& member : expanded->members) {
            bool covered = false;
            for (const auto& matchCase : matchStmt->cases) {
                if (doesCaseCoverType(matchCase, member)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                return false;
            }
        }
        return true;
    }

    if (expanded->nullable && expanded->name != "Null" && expanded->name != "Any") {
        auto nonNullType = cloneType(expanded);
        nonNullType->nullable = false;
        bool coversValue = false;
        bool coversNull = false;
        for (const auto& matchCase : matchStmt->cases) {
            coversValue = coversValue || doesCaseCoverType(matchCase, nonNullType);
            coversNull = coversNull || doesCaseCoverType(matchCase, makeType("Null"));
        }
        return coversValue && coversNull;
    }

    if (expanded->kind == TypeKind::Intersection) {
        for (const auto& matchCase : matchStmt->cases) {
            if (doesCaseCoverType(matchCase, expanded)) {
                return true;
            }
        }
        return false;
    }

    if (expanded->name == "Bool") {
        bool hasTrue = false;
        bool hasFalse = false;
        for (const auto& matchCase : matchStmt->cases) {
            if (matchCase.kind == MatchPatternKind::Type && matchCase.typeName == "Bool") {
                return true;
            }
            if (matchCase.kind != MatchPatternKind::Literal || !matchCase.pattern) {
                continue;
            }
            if (matchCase.pattern->kind != ExpressionKind::Literal) {
                continue;
            }
            const auto* literal = static_cast<const LiteralExpr*>(matchCase.pattern.get());
            if (literal->tokenType == TokenType::TRUE) {
                hasTrue = true;
            } else if (literal->tokenType == TokenType::FALSE) {
                hasFalse = true;
            }
        }
        return hasTrue && hasFalse;
    }

    for (const auto& matchCase : matchStmt->cases) {
        if (doesCaseCoverType(matchCase, expanded)) {
            return true;
        }
    }
    return false;
}

bool SemanticAnalyzer::isAssignable(const TypePtr& target, const TypePtr& source) const {
    TypePtr targetType = expandAliases(target);
    TypePtr sourceType = expandAliases(source);
    auto expandedTarget = targetType;
    auto expandedSource = sourceType;
    if (!expandedTarget || !expandedSource) {
        return false;
    }
    targetType = expandedTarget;
    sourceType = expandedSource;

    if (!targetType || targetType->name == "Any") {
        return true;
    }
    if (!sourceType || sourceType->name == "Any") {
        return true;
    }
    if (sourceType->kind == TypeKind::Union) {
        for (const auto& member : sourceType->members) {
            if (!isAssignable(targetType, member)) {
                return false;
            }
        }
        return true;
    }
    if (targetType->kind == TypeKind::Union) {
        for (const auto& member : targetType->members) {
            if (isAssignable(member, sourceType)) {
                return true;
            }
        }
        return false;
    }
    if (targetType->kind == TypeKind::Intersection) {
        for (const auto& member : targetType->members) {
            if (!isAssignable(member, sourceType)) {
                return false;
            }
        }
        return true;
    }
    if (sourceType->kind == TypeKind::Intersection) {
        for (const auto& member : sourceType->members) {
            if (isAssignable(targetType, member)) {
                return true;
            }
        }
        return false;
    }
    if (targetType->kind == TypeKind::Function || sourceType->kind == TypeKind::Function) {
        if (targetType->kind != TypeKind::Function || sourceType->kind != TypeKind::Function) {
            return false;
        }
        if (targetType->parameterTypes.size() != sourceType->parameterTypes.size()) {
            return false;
        }
        for (size_t i = 0; i < targetType->parameterTypes.size(); ++i) {
            if (!isAssignable(sourceType->parameterTypes[i], targetType->parameterTypes[i])) {
                return false;
            }
        }
        return isAssignable(targetType->returnType, sourceType->returnType) &&
               (targetType->nullable || !sourceType->nullable);
    }
    if (isGenericParameter(targetType->name)) {
        const auto* bounds = genericBounds(targetType->name);
        return !bounds || satisfiesGenericBounds(sourceType, *bounds);
    }
    if (targetType->name == "Float" && sourceType->name == "Int") {
        return true;
    }
    if (sourceType->name == "Null") {
        return targetType->nullable || targetType->name == "Null" || targetType->name == "Any";
    }
    if (interfaces.find(targetType->name) != interfaces.end()) {
        return typeSatisfiesInterface(sourceType, targetType->name) && (targetType->nullable || !sourceType->nullable);
    }
    if (targetType->name != sourceType->name && isSubclassOf(sourceType->name, targetType->name)) {
        return targetType->nullable || !sourceType->nullable;
    }
    if (targetType->name != sourceType->name) {
        return false;
    }
    if (targetType->genericArguments.size() != sourceType->genericArguments.size()) {
        return false;
    }
    for (size_t i = 0; i < targetType->genericArguments.size(); ++i) {
        const bool covariantContainer = targetType->name == "Future";
        const bool invariantContainer = targetType->name == "List";
        if (targetType->name == "Tuple") {
            if (targetType->genericArguments.size() != sourceType->genericArguments.size()) {
                return false;
            }
            if (!isAssignable(targetType->genericArguments[i], sourceType->genericArguments[i]) ||
                !isAssignable(sourceType->genericArguments[i], targetType->genericArguments[i])) {
                return false;
            }
            continue;
        }
        if (covariantContainer) {
            if (!isAssignable(targetType->genericArguments[i], sourceType->genericArguments[i])) {
                return false;
            }
            continue;
        }
        if (invariantContainer || targetType->name == "Map" || targetType->name == "Set") {
            if (!isAssignable(targetType->genericArguments[i], sourceType->genericArguments[i]) ||
                !isAssignable(sourceType->genericArguments[i], targetType->genericArguments[i])) {
                return false;
            }
            continue;
        }
        if (!isAssignable(targetType->genericArguments[i], sourceType->genericArguments[i]) ||
            !isAssignable(sourceType->genericArguments[i], targetType->genericArguments[i])) {
            return false;
        }
    }
    return targetType->nullable || !sourceType->nullable || targetType->name == "Null";
}

bool SemanticAnalyzer::typeSatisfiesInterface(const TypePtr& source, const std::string& interfaceName) const {
    TypePtr sourceType = expandAliases(source);
    if (!sourceType) {
        return false;
    }
    if (sourceType->kind == TypeKind::Union) {
        for (const auto& member : sourceType->members) {
            if (!typeSatisfiesInterface(member, interfaceName)) {
                return false;
            }
        }
        return true;
    }
    if (sourceType->kind == TypeKind::Intersection) {
        for (const auto& member : sourceType->members) {
            if (typeSatisfiesInterface(member, interfaceName)) {
                return true;
            }
        }
        return false;
    }
    if (sourceType->name == "Any") {
        return true;
    }
    if (sourceType->name == interfaceName) {
        return true;
    }
    auto ifaceIt = interfaces.find(interfaceName);
    if (ifaceIt == interfaces.end()) {
        return false;
    }
    if (auto sourceIfaceIt = interfaces.find(sourceType->name); sourceIfaceIt != interfaces.end()) {
        for (const auto& base : sourceIfaceIt->second.baseInterfaces) {
            if (base == interfaceName || typeSatisfiesInterface(std::make_shared<Type>(base), interfaceName)) {
                return true;
            }
        }
        for (const auto& requirementGroup : ifaceIt->second.methods) {
            for (const auto& requirement : collectInterfaceMethods(interfaceName, requirementGroup.first)) {
                auto available = collectInterfaceMethods(sourceType->name, requirement.name);
                const CallableInfo* matched = nullptr;
                for (const auto& candidate : available) {
                    if (signatureCompatible(candidate, requirement, true)) {
                        matched = &candidate;
                        break;
                    }
                }
                if (!matched ||
                    matched->isStatic != requirement.isStatic ||
                    matched->isAsync != requirement.isAsync ||
                    !signatureCompatible(*matched, requirement, true)) {
                    return false;
                }
            }
        }
        return true;
    }

    auto classIt = classes.find(sourceType->name);
    if (classIt == classes.end()) {
        return false;
    }
    for (const auto& requirementGroup : ifaceIt->second.methods) {
        for (const auto& requirement : collectInterfaceMethods(interfaceName, requirementGroup.first)) {
            auto available = collectClassMethods(classIt->second, requirement.name, requirement.isStatic);
            const CallableInfo* matched = nullptr;
            for (const auto& candidate : available) {
                if (signatureCompatible(candidate, requirement, true)) {
                    matched = &candidate;
                    break;
                }
            }
            if (!matched ||
                matched->isAbstract ||
                matched->isStatic != requirement.isStatic ||
                matched->isAsync != requirement.isAsync ||
                !signatureCompatible(*matched, requirement, true)) {
                return false;
            }
        }
    }
    return true;
}

TypePtr SemanticAnalyzer::inferBinaryType(const BinaryExpr* binary, const TypePtr& left, const TypePtr& right) {
    switch (binary->op.type) {
        case TokenType::AND:
        case TokenType::OR:
            if ((left->name != "Bool" && left->name != "Any") ||
                (right->name != "Bool" && right->name != "Any")) {
                error(binary->loc, "logical operators require Bool operands");
            }
            return makeType("Bool");
        case TokenType::EQ_EQ:
        case TokenType::BANG_EQ:
            return makeType("Bool");
        case TokenType::LT:
        case TokenType::LTE:
        case TokenType::GT:
        case TokenType::GTE:
            if (((left->name != "Int" && left->name != "Float" && left->name != "Any")) ||
                ((right->name != "Int" && right->name != "Float" && right->name != "Any"))) {
                error(binary->loc, "comparison operators require numeric operands");
            }
            return makeType("Bool");
        case TokenType::PLUS:
            if (left->name == "String" || right->name == "String") {
                return makeType("String");
            }
            [[fallthrough]];
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::STAR_STAR:
            if (((left->name != "Int" && left->name != "Float" && left->name != "Any")) ||
                ((right->name != "Int" && right->name != "Float" && right->name != "Any"))) {
                error(binary->loc, "arithmetic operators require numeric operands");
            }
            if (left->name == "Float" || right->name == "Float" ||
                binary->op.type == TokenType::SLASH ||
                binary->op.type == TokenType::STAR_STAR) {
                return makeType("Float");
            }
            return makeType("Int");
        case TokenType::PERCENT:
            if ((left->name != "Int" && left->name != "Any") || (right->name != "Int" && right->name != "Any")) {
                error(binary->loc, "modulo requires Int operands");
            }
            return makeType("Int");
        default:
            return makeType("Any");
    }
}

TypePtr SemanticAnalyzer::makeType(const std::string& name, bool nullable) const {
    return std::make_shared<Type>(name, nullable);
}

TypePtr SemanticAnalyzer::iterableElementType(const TypePtr& iterableType) const {
    if (!iterableType) {
        return makeType("Any");
    }

    TypePtr expanded = expandAliases(iterableType);
    if (!expanded) {
        expanded = iterableType;
    }
    if (!expanded) {
        return makeType("Any");
    }
    if (expanded->name == "List" && !expanded->genericArguments.empty()) {
        return cloneType(expanded->genericArguments[0]);
    }
    if (expanded->name == "Set" && !expanded->genericArguments.empty()) {
        return cloneType(expanded->genericArguments[0]);
    }
    if (expanded->name == "Range") {
        return makeType("Int");
    }
    if (expanded->name == "Tuple" && !expanded->genericArguments.empty()) {
        TypePtr first = cloneType(expanded->genericArguments[0]);
        for (size_t i = 1; i < expanded->genericArguments.size(); ++i) {
            if (!typeEquivalent(first, expanded->genericArguments[i])) {
                return makeType("Any");
            }
        }
        return first;
    }
    return makeType("Any");
}

TypePtr SemanticAnalyzer::makeFutureType(const TypePtr& inner) const {
    std::vector<TypePtr> arguments;
    arguments.push_back(cloneType(inner ? inner : makeType("Any")));
    return std::make_shared<Type>("Future", std::move(arguments), false);
}

TypePtr SemanticAnalyzer::unwrapFutureType(const TypePtr& type) const {
    if (!type || type->name != "Future" || type->genericArguments.size() != 1) {
        return nullptr;
    }
    return cloneType(type->genericArguments.front());
}

std::string SemanticAnalyzer::typeName(const TypePtr& type) const {
    return type ? type->toString() : "Void";
}

}  // namespace smush
