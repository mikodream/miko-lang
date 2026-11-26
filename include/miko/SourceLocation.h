#pragma once

#include <cstddef>
#include <string>

namespace miko {

struct SourceLocation {
    std::string file;
    std::size_t line = 1;
    std::size_t column = 1;
    std::size_t offset = 0;
};

} // namespace miko
