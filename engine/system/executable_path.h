// SimpleGraphic Engine
//
// Resolve the image that is executing this process.  Both the runtime and the
// standalone host need this independently of argv[0], which may be only a
// PATH lookup or a symlink name.

#pragma once

#include <cerrno>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__) && defined(__MACH__)
#include <libproc.h>
#include <unistd.h>
#endif

inline std::filesystem::path SimpleGraphicExecutablePath(std::error_code& error)
{
    error.clear();
    std::filesystem::path executable;

#ifdef _WIN32
    // Windows can return a path larger than MAX_PATH when long paths are
    // enabled. Grow until the API confirms that the result was not truncated.
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            return {};
        }
        if (length < buffer.size()) {
            executable = std::filesystem::path(std::wstring(buffer.data(), length));
            break;
        }
        if (buffer.size() >= 32768) {
            error = std::make_error_code(std::errc::filename_too_long);
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__linux__)
    // readlink deliberately does not append a NUL and signals truncation by
    // returning the supplied capacity. Do not rely on PATH_MAX here.
    std::vector<char> buffer(512);
    for (;;) {
        const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            error = std::error_code(errno, std::generic_category());
            return {};
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            executable = std::filesystem::u8path(
                std::string(buffer.data(), static_cast<std::size_t>(length)));
            break;
        }
        if (buffer.size() >= 1024 * 1024) {
            error = std::make_error_code(std::errc::filename_too_long);
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__) && defined(__MACH__)
    char buffer[PROC_PIDPATHINFO_MAXSIZE]{};
    if (::proc_pidpath(::getpid(), buffer, sizeof(buffer)) <= 0) {
        error = std::error_code(errno, std::generic_category());
        if (!error) {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
        }
        return {};
    }
    executable = std::filesystem::u8path(buffer);
#else
    error = std::make_error_code(std::errc::operation_not_supported);
    return {};
#endif

    executable = std::filesystem::weakly_canonical(executable, error);
    if (error || executable.empty()) {
        if (!error) {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
        }
        return {};
    }
    return executable;
}
