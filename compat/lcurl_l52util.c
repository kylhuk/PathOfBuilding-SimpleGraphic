/******************************************************************************
* Compatibility implementation derived from Lua-cURLv3's src/l52util.c.
*
* Lua-cURLv3 is MIT licensed. This local copy keeps its Lua 5.2 helper
* implementation while avoiding duplicate helper exports when the runtime and
* extensions share the dynamically linked LuaJIT 2.1 library.
******************************************************************************/

#include "l52util.h"

#include <assert.h>
#include <memory.h>
#include <string.h>

#if LUA_VERSION_NUM >= 502

int luaL_typerror(lua_State* L, int narg, const char* tname)
{
	const char* message = lua_pushfstring(L, "%s expected, got %s", tname,
		luaL_typename(L, narg));
	return luaL_argerror(L, narg, message);
}

#ifndef luaL_register

void luaL_register(lua_State* L, const char* libname, const luaL_Reg* functions)
{
	if (libname) {
		lua_newtable(L);
	}
	luaL_setfuncs(L, functions, 0);
}

#endif

#else

/* LuaJIT exports luaL_setfuncs as part of its Lua 5.2 compatibility API.
 * lua_rawgetp and lua_rawsetp are Lua 5.2 helpers that LuaJIT does not
 * export, so this module supplies those two functions in all Lua 5.1 builds. */
#ifndef LCURL_LUAJIT_HAS_LUA52_AUX

void luaL_setfuncs(lua_State* L, const luaL_Reg* functions, int nup)
{
	luaL_checkstack(L, nup, "too many upvalues");
	for (; functions->name != NULL; ++functions) {
		for (int index = 0; index < nup; ++index) {
			lua_pushvalue(L, -nup);
		}
		lua_pushcclosure(L, functions->func, nup);
		lua_setfield(L, -(nup + 2), functions->name);
	}
	lua_pop(L, nup);
}

#endif

void lua_rawgetp(lua_State* L, int index, const void* pointer)
{
	index = lua_absindex(L, index);
	lua_pushlightuserdata(L, (void*)pointer);
	lua_rawget(L, index);
}

void lua_rawsetp(lua_State* L, int index, const void* pointer)
{
	index = lua_absindex(L, index);
	lua_pushlightuserdata(L, (void*)pointer);
	lua_insert(L, -2);
	lua_rawset(L, index);
}
#endif

int lutil_newmetatablep(lua_State* L, const void* pointer)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX, pointer);
	if (!lua_isnil(L, -1)) {
		return 0;
	}
	lua_pop(L, 1);

	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_pushliteral(L, "__type");
	lua_pushstring(L, (const char*)pointer);
	lua_settable(L, -3);
	lua_rawsetp(L, LUA_REGISTRYINDEX, pointer);
	return 1;
}

void lutil_getmetatablep(lua_State* L, const void* pointer)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX, pointer);
}

void lutil_setmetatablep(lua_State* L, const void* pointer)
{
	lutil_getmetatablep(L, pointer);
	assert(lua_istable(L, -1));
	lua_setmetatable(L, -2);
}

int lutil_isudatap(lua_State* L, int userdataIndex, const void* pointer)
{
	if (lua_isuserdata(L, userdataIndex) && lua_getmetatable(L, userdataIndex)) {
		lutil_getmetatablep(L, pointer);
		const int result = lua_rawequal(L, -1, -2);
		lua_pop(L, 2);
		return result;
	}
	return 0;
}

void* lutil_checkudatap(lua_State* L, int userdataIndex, const void* pointer)
{
	void* userdata = lua_touserdata(L, userdataIndex);
	if (userdata && lua_getmetatable(L, userdataIndex)) {
		lutil_getmetatablep(L, pointer);
		if (lua_rawequal(L, -1, -2)) {
			lua_pop(L, 2);
			return userdata;
		}
	}
	luaL_typerror(L, userdataIndex, (const char*)pointer);
	return NULL;
}

int lutil_createmetap(lua_State* L, const void* pointer, const luaL_Reg* methods, int nup)
{
	if (!lutil_newmetatablep(L, pointer)) {
		lua_insert(L, -1 - nup);
		return 0;
	}

	lua_insert(L, -1 - nup);
	luaL_setfuncs(L, methods, nup);
	lua_pushliteral(L, "__index");
	lua_pushvalue(L, -2);
	lua_settable(L, -3);
	return 1;
}

void* lutil_newudatap_impl(lua_State* L, size_t size, const void* pointer)
{
	void* object = lua_newuserdata(L, size);
	memset(object, 0, size);
	lutil_setmetatablep(L, pointer);
	return object;
}

void lutil_pushint64(lua_State* L, int64_t value)
{
	if (sizeof(lua_Integer) >= sizeof(int64_t)) {
		lua_pushinteger(L, (lua_Integer)value);
		return;
	}
	lua_pushnumber(L, (lua_Number)value);
}

void lutil_pushuint(lua_State* L, unsigned int value)
{
#if LUA_VERSION_NUM >= 503
	lua_pushinteger(L, (lua_Integer)value);
#else
	lua_pushnumber(L, (lua_Number)value);
#endif
}

int64_t lutil_checkint64(lua_State* L, int index)
{
	if (sizeof(lua_Integer) >= sizeof(int64_t)) {
		return luaL_checkinteger(L, index);
	}
	return (int64_t)luaL_checknumber(L, index);
}

int64_t lutil_optint64(lua_State* L, int index, int64_t value)
{
	if (sizeof(lua_Integer) >= sizeof(int64_t)) {
		return luaL_optinteger(L, index, value);
	}
	return (int64_t)luaL_optnumber(L, index, value);
}

void lutil_pushnvalues(lua_State* L, int count)
{
	for (; count; --count) {
		lua_pushvalue(L, -count);
	}
}

int lutil_is_null(lua_State* L, int index)
{
	return lua_islightuserdata(L, index) && lua_touserdata(L, index) == NULL;
}

void lutil_push_null(lua_State* L)
{
	lua_pushlightuserdata(L, NULL);
}
