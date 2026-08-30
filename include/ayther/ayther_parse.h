#pragma once

#include <charconv>
#include <cctype>
#include <cstddef>
#include <span>
#include <string_view>
#include <system_error>

namespace ayther {

[[nodiscard]] inline bool parse_comma_separated_ints(
    std::string_view input, std::span<int> values) noexcept {
    const char* cursor = input.data();
    const char* const end = cursor + input.size();
    const auto skip_spaces = [&cursor, end] {
        while (cursor != end &&
               std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
            ++cursor;
        }
    };

    for (std::size_t index = 0; index < values.size(); ++index) {
        skip_spaces();
        const auto [next, error] = std::from_chars(cursor, end, values[index]);
        if (error != std::errc{}) {
            return false;
        }
        cursor = next;
        skip_spaces();
        if (index + 1 < values.size()) {
            if (cursor == end || *cursor != ',') {
                return false;
            }
            ++cursor;
        }
    }
    skip_spaces();
    return cursor == end;
}

}  // namespace ayther
