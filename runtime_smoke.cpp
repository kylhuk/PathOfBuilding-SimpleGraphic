#include "simplegraphic.h"
#include "lua_runtime.h"

#include <filesystem>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

extern "C" int SimpleGraphicRuntimeSmoke(const char* runtimeDirectory)
{
	if (!runtimeDirectory || !*runtimeDirectory) {
		return 2;
	}

	lua_State* state = luaL_newstate();
	if (!state) {
		return 2;
	}
	luaL_openlibs(state);
	ConfigureLuaSearchPaths(state, std::filesystem::u8path(runtimeDirectory));

	constexpr char smokeScript[] = R"lua(
local modules = {
  "lua-utf8", "socket", "socket.core", "mime", "mime.core", "lzip",
  "lcurl", "lcurl.safe", "cURL", "cURL.safe",
}
for _, module in ipairs(modules) do
  assert(require(module), "failed to load " .. module)
end
if package.config:sub(1, 1) == "/" then
  assert(require("socket.unix"), "failed to load socket.unix")
end
)lua";

	const int result = luaL_loadbuffer(state, smokeScript, sizeof(smokeScript) - 1, "@runtime-module-smoke") == 0
		? lua_pcall(state, 0, 0, 0)
		: 1;
	lua_close(state);
	return result == 0 ? 0 : 1;
}
