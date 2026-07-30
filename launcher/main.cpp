#include <cstdio>
#include <cstring>
#include <filesystem>

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
    // executable name.  Preserve that convention for the standalone host.
    return RunLuaFileAsWin(argc - 1, argv + 1);
}
