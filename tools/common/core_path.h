#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace ayther::test {

struct CorePathResult {
    std::filesystem::path path;
    std::string diagnostic;
    int failure_exit_code{1};

    [[nodiscard]] bool available() const noexcept { return !path.empty(); }
};

/// An explicit override is authoritative, including when it is invalid.
/// Without an override, core.lock supplies the filename. Only a missing optional
/// locked binary permits CTest skip code 77; configuration errors fail the test.
[[nodiscard]] inline CorePathResult
resolve_core_path(const std::filesystem::path &source_directory,
                  std::optional<std::string_view> override_path = std::nullopt) {
    if (override_path && !override_path->empty()) {
        const std::filesystem::path path{*override_path};
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
            return {path, {}, 0};
        return {{}, "AYTHER_ABI_CORE is not a readable core file: " + path.string(), 1};
    }
    const auto lock_path = source_directory / "third_party/cores/core.lock";
    std::ifstream lock{lock_path};
    if (!lock)
        return {{}, "Cannot read core lock: " + lock_path.string(), 1};
    const auto trim = [](std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return std::string_view{};
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    };
    std::string core_filename;
    bool found_filename = false;
    for (std::string line; std::getline(lock, line);) {
        const auto content = std::string_view{line}.substr(0, line.find('#'));
        const auto separator = content.find('=');
        if (separator == std::string_view::npos ||
            trim(content.substr(0, separator)) != "vram_file")
            continue;
        if (found_filename)
            return {{}, "Duplicate vram_file in core lock: " + lock_path.string(), 1};
        found_filename = true;
        core_filename = trim(content.substr(separator + 1));
    }
    const std::filesystem::path filename{core_filename};
    if (core_filename.empty() || filename.has_parent_path() || filename == "." ||
        filename == "..") {
        return {{}, "Expected a vram_file filename in core lock: " + lock_path.string(), 1};
    }
    const auto path = lock_path.parent_path() / filename;
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error)
        return {{}, "Cannot inspect locked core: " + path.string(), 1};
    if (!exists)
        return {{}, "Optional core is absent: " + path.string(), 77};
    if (std::filesystem::is_regular_file(path, error))
        return {path, {}, 0};
    return {{}, "Locked core is not a readable file: " + path.string(), 1};
}

[[nodiscard]] inline CorePathResult
configured_core_path(const std::filesystem::path &source_directory) {
#ifdef _WIN32
    std::size_t required_size = 0;
    if (::getenv_s(&required_size, nullptr, 0, "AYTHER_ABI_CORE") != 0) {
        return {{}, "Cannot read AYTHER_ABI_CORE", 1};
    }
    if (required_size == 0)
        return resolve_core_path(source_directory);
    std::string override_path(required_size, '\0');
    if (::getenv_s(&required_size, override_path.data(), override_path.size(), "AYTHER_ABI_CORE") !=
        0) {
        return {{}, "Cannot read AYTHER_ABI_CORE", 1};
    }
    override_path.resize(required_size == 0 ? 0 : required_size - 1);
    return resolve_core_path(source_directory, override_path);
#else
    const char *override_path = std::getenv("AYTHER_ABI_CORE");
    return resolve_core_path(source_directory, override_path
                                                   ? std::optional<std::string_view>{override_path}
                                                   : std::nullopt);
#endif
}

} // namespace ayther::test
