#pragma once

#include <string>
#include <string_view>

namespace miko {

inline bool isEntryFunctionName(std::string_view name) {
    return name == "main";
}

inline std::string canonicalEntryFunctionName(std::string_view name) {
    return isEntryFunctionName(name) ? "main" : std::string(name);
}

inline bool isBuiltinFunctionName(std::string_view name) {
    return name == "print"
        || name == "len"
        || name == "input"
        || name == "arg_count"
        || name == "arg_at"
        || name == "char_at"
        || name == "substring"
        || name == "read_file"
        || name == "write_file"
        || name == "run_command"
        || name == "seed_i64"
        || name == "rand_i64"
        || name == "vec_new"
        || name == "vec_push"
        || name == "vec_pop"
        || name == "vec_set"
        || name == "vec_clear"
        || name == "vec_reserve"
        || name == "to_string_i64"
        || name == "to_string_f64"
        || name == "to_string_bool"
        || name == "to_string_char"
        || name == "str_to_i64"
        || name == "str_to_f64"
        || name == "str_index_of"
        || name == "str_contains"
        || name == "append_str"
        || name == "sqrt_f64"
        || name == "floor_f64"
        || name == "ceil_f64"
        || name == "round_f64"
        || name == "f64_to_i64"
        || name == "exit"
        || name == "eprint"
        || name == "file_exists"
        || name == "clock_ms"
        || name == "mono_ms";
}

inline std::string canonicalBuiltinName(std::string_view name) {
    return isBuiltinFunctionName(name) ? std::string(name) : std::string(name);
}

} // namespace miko
