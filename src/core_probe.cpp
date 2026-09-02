#include <ayther/engine/core_probe.hpp>

#include "libretro_host/libretro.h"

#include <array>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ayther::engine {
namespace {

#if defined(_WIN32)
using NativeLibrary = HMODULE;
#else
using NativeLibrary = void*;
#endif

std::string display_path(const std::filesystem::path& path) {
    return path.generic_string();
}

std::string json_escape(std::string_view value) {
    constexpr std::array<char, 16> kHexDigits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char character : value) {
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (character < 0x20U) {
                escaped += "\\u00";
                escaped.push_back(kHexDigits[character >> 4U]);
                escaped.push_back(kHexDigits[character & 0x0fU]);
            } else {
                escaped.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return escaped;
}

class DynamicLibrary final {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary() { close(); }

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    DynamicLibrary(DynamicLibrary&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] std::string open(const std::filesystem::path& path) {
#if defined(_WIN32)
        handle_ = LoadLibraryW(path.c_str());
        if (handle_ == nullptr) {
            const DWORD error_code = GetLastError();
            return "LoadLibraryW failed for '" + display_path(path) +
                "' with Windows error " + std::to_string(error_code);
        }
#else
        dlerror();
        handle_ = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (handle_ == nullptr) {
            const char* error = dlerror();
            return "dlopen failed for '" + display_path(path) + "': " +
                (error != nullptr ? error : "unknown platform error");
        }
#endif
        return {};
    }

    template <typename FunctionPointer>
    [[nodiscard]] FunctionPointer symbol(const char* name) const noexcept {
#if defined(_WIN32)
        return reinterpret_cast<FunctionPointer>(GetProcAddress(handle_, name));
#else
        return reinterpret_cast<FunctionPointer>(dlsym(handle_, name));
#endif
    }

private:
    void close() noexcept {
        if (handle_ == nullptr) {
            return;
        }
#if defined(_WIN32)
        static_cast<void>(FreeLibrary(handle_));
#else
        static_cast<void>(dlclose(handle_));
#endif
        handle_ = nullptr;
    }

    NativeLibrary handle_{};
};

}  // namespace

class CoreProbe::Impl final {
public:
    explicit Impl(DynamicLibrary library) noexcept
        : library_(std::move(library)) {}

private:
    DynamicLibrary library_;
};

std::string CoreInfo::serialize() const {
    std::string json = "{\"api\":" + std::to_string(api_version);
    json += ",\"library_name\":\"" + json_escape(library_name) + '"';
    json += ",\"library_version\":\"" + json_escape(library_version) + '"';
    json += ",\"valid_extensions\":\"" + json_escape(valid_extensions) + '"';
    json += ",\"need_fullpath\":";
    json += need_fullpath ? "true" : "false";
    json += ",\"block_extract\":";
    json += block_extract ? "true" : "false";
    json += '}';
    return json;
}

CoreProbe::CoreProbe(std::unique_ptr<Impl> impl, CoreInfo info) noexcept
    : impl_(std::move(impl)), info_(std::move(info)) {}

CoreProbe::~CoreProbe() = default;
CoreProbe::CoreProbe(CoreProbe&&) noexcept = default;
CoreProbe& CoreProbe::operator=(CoreProbe&&) noexcept = default;

const CoreInfo& CoreProbe::info() const noexcept {
    return info_;
}

std::string CoreProbe::serialize() const {
    return info_.serialize();
}

Result<CoreProbe> probe_core(const std::filesystem::path& core_path) {
    DynamicLibrary library;
    if (const std::string load_error = library.open(core_path);
        !load_error.empty()) {
        return Error{ErrorCode::Io, load_error};
    }

    using ApiVersionFunction = unsigned (*)(void);
    using SystemInfoFunction = void (*)(retro_system_info*);
    const auto api_version =
        library.symbol<ApiVersionFunction>("retro_api_version");
    const auto system_info =
        library.symbol<SystemInfoFunction>("retro_get_system_info");
    if (api_version == nullptr || system_info == nullptr) {
        std::string missing;
        if (api_version == nullptr) {
            missing = "retro_api_version";
        }
        if (system_info == nullptr) {
            if (!missing.empty()) {
                missing += ", ";
            }
            missing += "retro_get_system_info";
        }
        return Error{
            ErrorCode::BadFormat,
            "The loaded library is not a Libretro core; missing symbol(s): " +
                missing,
        };
    }

    retro_system_info raw_info{};
    system_info(&raw_info);
    CoreInfo info{
        .api_version = api_version(),
        .library_name = raw_info.library_name != nullptr
            ? raw_info.library_name
            : "",
        .library_version = raw_info.library_version != nullptr
            ? raw_info.library_version
            : "",
        .valid_extensions = raw_info.valid_extensions != nullptr
            ? raw_info.valid_extensions
            : "",
        .need_fullpath = raw_info.need_fullpath,
        .block_extract = raw_info.block_extract,
    };

    return CoreProbe{
        std::make_unique<CoreProbe::Impl>(std::move(library)),
        std::move(info),
    };
}

}  // namespace ayther::engine
