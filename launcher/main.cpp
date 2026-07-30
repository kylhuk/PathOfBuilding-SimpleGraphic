#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "config.h"
#include "simplegraphic.h"

#ifndef SIMPLEGRAPHIC_VERSION
#define SIMPLEGRAPHIC_VERSION CFG_VERSION_NUM
#endif

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::puts(SIMPLEGRAPHIC_VERSION);
        return 0;
    }

    if (argc == 2 && std::strcmp(argv[1], "--smoke-modules") == 0) {
        std::error_code error;
        const auto executable = std::filesystem::absolute(std::filesystem::u8path(argv[0]), error);
        if (error) {
            std::fputs("Unable to determine the runtime directory.\n", stderr);
            return 2;
        }
        return SimpleGraphicRuntimeSmoke(executable.parent_path().generic_u8string().c_str());
    }

    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <script.lua> [script arguments...]\\n", argv[0]);
        return 64;
    }

    // The runtime ABI treats argv[0] as the Lua script, rather than an
    // executable name. Resolve it before that ABI changes the process CWD to
    // the runtime directory; retain every remaining script argument verbatim.
    std::error_code error;
    const auto scriptPath = std::filesystem::absolute(
        std::filesystem::u8path(argv[1]), error);
    if (error) {
        std::fprintf(stderr, "Unable to resolve the Lua script path: %s\n", error.message().c_str());
        return 2;
    }
    auto scriptArgument = scriptPath.generic_u8string();
    std::vector<char*> runtimeArgs(argv + 1, argv + argc);
    runtimeArgs.front() = scriptArgument.data();
    return RunLuaFileAsWin(static_cast<int>(runtimeArgs.size()), runtimeArgs.data());
}
