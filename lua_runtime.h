#pragma once

#include <filesystem>

struct lua_State;

// Configure deterministic, relocatable search paths for the native Lua
// modules bundled beside the runtime.
void ConfigureLuaSearchPaths(lua_State* state, std::filesystem::path const& runtimePath);
