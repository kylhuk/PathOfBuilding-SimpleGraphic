// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// Module: UI Sub Script
//

#include "ui_local.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

// =======
// Classes
// =======

struct ssTweenData_s {
	ssTweenData_s* next = nullptr;
	enum {
		NIL,
		BOOLEAN,
		NUM,
		STRING
	} type = NIL;
	union {
		bool boolean;
		double num;
		char* string;
	};
};

struct ssCall_s {
	ssCall_s* next = nullptr;
	std::string name;
	ssTweenData_s* data = nullptr;
};

// =======================
// ui_ISubScript Interface
// =======================

class ui_subscript_c: public ui_ISubScript {
public:
	bool Start() override;
	std::string StartError() override;
	void SubScriptFrame() override;
	bool IsRunning() override;
	size_t GetScriptMemory() override;

	ui_subscript_c(ui_main_c* ui, dword id);
	~ui_subscript_c();

	void Stop() override;
	void ThreadProc();
	bool StopRequested();
	void RecordPanic(char const* message);
	void CacheScriptMemory(lua_State* state);
	ssTweenData_s* SubmitFunctionCall(std::string name, ssTweenData_s* request);
	bool QueueSubCall(ssCall_s* call);

	void LAssert(int cond, const char* fmt, ...);

	ui_main_c* ui = nullptr;
	dword id = 0;
	lua_State* L = nullptr;

private:
	std::thread worker;
	std::mutex stateMutex;
	std::condition_variable stateChanged;
	std::atomic_size_t luaMemoryKilobytes = 0;
	bool running = false;
	bool finished = false;
	bool stopRequested = false;
	bool funcWaiting = false;
	bool funcProcessing = false;
	ssCall_s* subCalls = nullptr;
	ssCall_s funcCall;
	std::string errorText;

	ssCall_s* TakeSubCalls();
	bool TakeFunctionRequest(ssCall_s& call);
	void CompleteFunctionRequest(ssTweenData_s* response);
	void JoinWorker();
};

ui_ISubScript* ui_ISubScript::GetHandle(ui_main_c* ui, dword id)
{
	return new ui_subscript_c(ui, id);
}

void ui_ISubScript::FreeHandle(ui_ISubScript* hnd)
{
	delete static_cast<ui_subscript_c*>(hnd);
}

ui_subscript_c::ui_subscript_c(ui_main_c* ui, dword id)
	: ui(ui), id(id)
{
}

ui_subscript_c::~ui_subscript_c()
{
	Stop();
	if (L) {
		lua_close(L);
		L = nullptr;
	}
}

// =======================
// Lua Interface Utilities
// =======================

static ui_subscript_c* GetSSPtr(lua_State* L)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, 0);
	auto* ss = static_cast<ui_subscript_c*>(lua_touserdata(L, -1));
	lua_pop(L, 1);
	return ss;
}

void ui_subscript_c::LAssert(int cond, const char* fmt, ...)
{
	if (!cond) {
		va_list va;
		va_start(va, fmt);
		lua_pushvfstring(L, fmt, va);
		va_end(va);
		lua_error(L);
	}
}

// From lua.c
static int traceback(lua_State* L) {
	if (!lua_isstring(L, 1))
		return 1;
	lua_getglobal(L, "debug");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 1;
	}
	lua_getfield(L, -1, "traceback");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return 1;
	}
	lua_pushvalue(L, 1);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

static int l_panicFunc(lua_State* L)
{
	auto* ss = GetSSPtr(L);
	if (ss) {
		// This callback may execute on the subscript worker. Never call into
		// the UI or system objects from a foreign Lua-owning thread.
		ss->RecordPanic(lua_tostring(L, -1));
	}
	return 0;
}

// ==================
// Tween Data Helpers
// ==================

static ssTweenData_s* ssBuildData(lua_State* L, int start)
{
	ssTweenData_s* ret = nullptr;
	ssTweenData_s* last = nullptr;
	const int n = lua_gettop(L);
	for (int a = start; a <= n; ++a) {
		auto* data = new ssTweenData_s;
		switch (lua_type(L, a)) {
		case LUA_TNIL:
			data->type = ssTweenData_s::NIL;
			break;
		case LUA_TBOOLEAN:
			data->type = ssTweenData_s::BOOLEAN;
			data->boolean = lua_toboolean(L, a) != 0;
			break;
		case LUA_TNUMBER:
			data->type = ssTweenData_s::NUM;
			data->num = lua_tonumber(L, a);
			break;
		case LUA_TSTRING:
			data->type = ssTweenData_s::STRING;
			data->string = AllocString(lua_tostring(L, a));
			break;
		default:
			delete data;
			continue;
		}
		if (last) {
			last->next = data;
		}
		else {
			ret = data;
		}
		last = data;
	}
	lua_settop(L, start - 1);
	return ret;
}

static void ssWipeData(ssTweenData_s* list)
{
	while (list) {
		auto* data = list;
		list = data->next;
		if (data->type == ssTweenData_s::STRING) {
			FreeString(data->string);
		}
		delete data;
	}
}

static int ssPushData(lua_State* L, ssTweenData_s* list)
{
	int numdat = 0;
	while (list) {
		auto* data = list;
		list = data->next;
		lua_checkstack(L, 1);
		switch (data->type) {
		case ssTweenData_s::NIL:
			lua_pushnil(L);
			break;
		case ssTweenData_s::BOOLEAN:
			lua_pushboolean(L, data->boolean);
			break;
		case ssTweenData_s::NUM:
			lua_pushnumber(L, data->num);
			break;
		case ssTweenData_s::STRING:
			lua_pushstring(L, data->string);
			FreeString(data->string);
			break;
		}
		delete data;
		++numdat;
	}
	return numdat;
}

// ============================
// Sub Script API and Utilities
// ============================

static int l_SubScriptFunc(lua_State* L)
{
	auto* ss = GetSSPtr(L);
	const int n = lua_gettop(L);
	const char* funcName = lua_tostring(L, lua_upvalueindex(1));
	for (int i = 1; i <= n; ++i) {
		ss->LAssert(lua_isnil(L, i) || lua_isboolean(L, i) || lua_isnumber(L, i) || lua_isstring(L, i),
			"%s() argument %d: only nil, boolean, number and string can be passed to the main script", funcName, i);
	}

	ssTweenData_s* response = ss->SubmitFunctionCall(
		funcName ? funcName : "", ssBuildData(L, 1));
	return ssPushData(L, response);
}

static int l_SubScriptSub(lua_State* L)
{
	auto* ss = GetSSPtr(L);
	const int n = lua_gettop(L);
	const char* subName = lua_tostring(L, lua_upvalueindex(1));
	for (int i = 1; i <= n; ++i) {
		ss->LAssert(lua_isnil(L, i) || lua_isboolean(L, i) || lua_isnumber(L, i) || lua_isstring(L, i),
			"%s() argument %d: only nil, boolean, number and string can be passed to the main script", subName, i);
	}

	auto* call = new ssCall_s;
	call->name = subName ? subName : "";
	call->data = ssBuildData(L, 1);

	if (!ss->QueueSubCall(call)) {
		ssWipeData(call->data);
		delete call;
		return 0;
	}
	return 0;
}

static int l_os_exit(lua_State*)
{
	return 0;
}

static void l_hookStop(lua_State* L, lua_Debug*)
{
	auto* ss = GetSSPtr(L);
	if (ss) {
		ss->CacheScriptMemory(L);
		if (ss->StopRequested()) {
			luaL_error(L, "Sub script stopped");
		}
	}
}

static void ssWipeCalls(ssCall_s* calls)
{
	while (calls) {
		auto* call = calls;
		calls = call->next;
		ssWipeData(call->data);
		delete call;
	}
}

static void parseSubScriptList(lua_State* L, const char* clist, lua_CFunction func)
{
	char* list = AllocString(clist ? clist : "");
	char* tok = strtok(list, ",");
	while (tok) {
		lua_pushstring(L, tok);
		lua_pushcclosure(L, func, 1);
		lua_setglobal(L, tok);
		tok = strtok(nullptr, ",");
	}
	FreeString(list);
}

// ===================
// UI Sub Script Class
// ===================

bool ui_subscript_c::Start()
{
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		errorText.clear();
		running = false;
		finished = false;
		stopRequested = false;
	}

	luaMemoryKilobytes.store(0, std::memory_order_relaxed);
	L = luaL_newstate();
	if (!L) {
		std::lock_guard<std::mutex> lock(stateMutex);
		errorText = "could not create a Lua state";
		return false;
	}
	lua_atpanic(L, l_panicFunc);
	lua_pushlightuserdata(L, this);
	lua_rawseti(L, LUA_REGISTRYINDEX, 0);
	lua_pushcfunction(L, traceback);

#ifdef _WIN32
	lua_pushboolean(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
#endif

	lua_gc(L, LUA_GCSTOP, 0);
	luaL_openlibs(L);
	ConfigureLuaSearchPaths(L, ui->sys->basePath);
	lua_getglobal(L, "os");
	lua_pushcfunction(L, l_os_exit);
	lua_setfield(L, -2, "exit");
	lua_pop(L, 1);
	parseSubScriptList(L, lua_tostring(ui->L, 2), l_SubScriptFunc);
	parseSubScriptList(L, lua_tostring(ui->L, 3), l_SubScriptSub);
	lua_gc(L, LUA_GCRESTART, -1);

	if (luaL_loadstring(L, lua_tostring(ui->L, 1))) {
		const char* message = lua_tostring(L, -1);
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			errorText = message ? message : "could not compile the sub script";
		}
		lua_close(L);
		L = nullptr;
		return false;
	}

	lua_pushinteger(L, ssPushData(L, ssBuildData(ui->L, 4)));
	CacheScriptMemory(L);
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		finished = false;
		stopRequested = false;
		funcWaiting = false;
		funcProcessing = false;
		errorText.clear();
		running = true;
	}
	try {
		worker = std::thread(&ui_subscript_c::ThreadProc, this);
	}
	catch (std::exception const& exception) {
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			errorText = exception.what();
			running = false;
		}
		lua_close(L);
		L = nullptr;
		return false;
	}
	return true;
}

std::string ui_subscript_c::StartError()
{
	std::lock_guard<std::mutex> lock(stateMutex);
	return errorText.empty() ? "could not initialize the sub script" : errorText;
}

void ui_subscript_c::JoinWorker()
{
	if (worker.joinable()) {
		worker.join();
	}
}

void ui_subscript_c::Stop()
{
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		if (!running && !worker.joinable()) {
			return;
		}
		stopRequested = true;
		if (funcWaiting) {
			ssWipeData(funcCall.data);
			funcCall.data = nullptr;
			funcWaiting = false;
			funcProcessing = false;
		}
		stateChanged.notify_all();
	}
	JoinWorker();

	ssCall_s* calls = nullptr;
	ssTweenData_s* pendingResponse = nullptr;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		calls = subCalls;
		subCalls = nullptr;
		pendingResponse = funcCall.data;
		funcCall.data = nullptr;
		funcWaiting = false;
		funcProcessing = false;
		running = false;
		finished = false;
	}
	ssWipeCalls(calls);
	ssWipeData(pendingResponse);
}

bool ui_subscript_c::StopRequested()
{
	std::lock_guard<std::mutex> lock(stateMutex);
	return stopRequested;
}

void ui_subscript_c::RecordPanic(char const* message)
{
	std::lock_guard<std::mutex> lock(stateMutex);
	errorText = message ? message : "Unprotected sub script Lua error";
	stopRequested = true;
	stateChanged.notify_all();
}

void ui_subscript_c::CacheScriptMemory(lua_State* state)
{
	// This is called only by the thread currently owning the Lua state. The UI
	// thread reads the atomic cache instead of concurrently entering Lua.
	luaMemoryKilobytes.store(static_cast<size_t>(lua_gc(state, LUA_GCCOUNT, 0)),
		std::memory_order_relaxed);
}

ssTweenData_s* ui_subscript_c::SubmitFunctionCall(std::string name, ssTweenData_s* request)
{
	CacheScriptMemory(L);
	std::unique_lock<std::mutex> lock(stateMutex);
	if (stopRequested) {
		ssWipeData(request);
		return nullptr;
	}
	funcCall.name = std::move(name);
	funcCall.data = request;
	funcWaiting = true;
	stateChanged.notify_all();
	stateChanged.wait(lock, [this] {
		return !funcWaiting || stopRequested;
	});
	auto* response = funcCall.data;
	funcCall.data = nullptr;
	return response;
}

bool ui_subscript_c::QueueSubCall(ssCall_s* call)
{
	std::lock_guard<std::mutex> lock(stateMutex);
	if (stopRequested) {
		return false;
	}
	if (subCalls) {
		auto* tail = subCalls;
		while (tail->next) {
			tail = tail->next;
		}
		tail->next = call;
	}
	else {
		subCalls = call;
	}
	stateChanged.notify_all();
	return true;
}

void ui_subscript_c::ThreadProc()
{
	int result = 0;
	std::string exceptionText;
	try {
		lua_sethook(L, l_hookStop, LUA_MASKCOUNT, 1000);
		const int numarg = static_cast<int>(lua_tointeger(L, -1));
		lua_pop(L, 1);
		result = lua_pcall(L, numarg, LUA_MULTRET, 1);
		lua_sethook(L, nullptr, 0, 0);
	}
	catch (std::exception const& exception) {
		exceptionText = exception.what();
	}
	catch (...) {
		exceptionText = "Unknown C++ exception in sub script";
	}
	CacheScriptMemory(L);

	std::lock_guard<std::mutex> lock(stateMutex);
	if (!exceptionText.empty()) {
		errorText = exceptionText;
	}
	else if (result && !stopRequested) {
		char const* text = lua_tostring(L, -1);
		errorText = text ? text : "Unknown sub script error";
	}
	finished = true;
	stateChanged.notify_all();
}

ssCall_s* ui_subscript_c::TakeSubCalls()
{
	std::lock_guard<std::mutex> lock(stateMutex);
	auto* calls = subCalls;
	subCalls = nullptr;
	return calls;
}

bool ui_subscript_c::TakeFunctionRequest(ssCall_s& call)
{
	std::lock_guard<std::mutex> lock(stateMutex);
	if (!funcWaiting || funcProcessing || stopRequested) {
		return false;
	}
	funcProcessing = true;
	call.name = funcCall.name;
	call.data = funcCall.data;
	funcCall.data = nullptr;
	return true;
}

void ui_subscript_c::CompleteFunctionRequest(ssTweenData_s* response)
{
	std::lock_guard<std::mutex> lock(stateMutex);
	if (stopRequested) {
		ssWipeData(response);
	}
	else {
		funcCall.data = response;
	}
	funcWaiting = false;
	funcProcessing = false;
	stateChanged.notify_all();
}

void ui_subscript_c::SubScriptFrame()
{
	for (auto* calls = TakeSubCalls(); calls;) {
		if (StopRequested()) {
			ssWipeCalls(calls);
			return;
		}
		auto* call = calls;
		calls = calls->next;
		const int extraArgs = ui->PushCallback("OnSubCall");
		if (extraArgs >= 0) {
			lua_pushstring(ui->L, call->name.c_str());
			const int numdat = ssPushData(ui->L, call->data);
			ui->PCall(extraArgs + numdat + 1, 0);
		}
		else {
			ssWipeData(call->data);
		}
		delete call;
		if (StopRequested()) {
			ssWipeCalls(calls);
			return;
		}
	}

	ssCall_s request;
	if (TakeFunctionRequest(request)) {
		const int retStart = lua_gettop(ui->L) + 1;
		bool doReturn = false;
		const int extraArgs = ui->PushCallback("OnSubCall");
		if (extraArgs >= 0) {
			lua_pushstring(ui->L, request.name.c_str());
			const int numdat = ssPushData(ui->L, request.data);
			request.data = nullptr;
			ui->PCall(extraArgs + numdat + 1, LUA_MULTRET);
			doReturn = true;

			for (int i = retStart; i <= lua_gettop(ui->L); ++i) {
				if (!(lua_isnil(ui->L, i) || lua_isboolean(ui->L, i) || lua_isnumber(ui->L, i) || lua_isstring(ui->L, i))) {
					char* msg = AllocStringLen(128);
					snprintf(msg, 128, "OnSubCall() return %d: only nil, boolean, number and string can be returned to sub script", i - retStart + 1);
					ui->DoError("Runtime error in", msg);
					FreeString(msg);
					doReturn = false;
					break;
				}
			}
		}
		else {
			ssWipeData(request.data);
			request.data = nullptr;
		}

		ssTweenData_s* response = doReturn ? ssBuildData(ui->L, retStart) : nullptr;
		if (!doReturn) {
			lua_settop(ui->L, retStart - 1);
		}
		CompleteFunctionRequest(response);
	}

	bool didFinish = false;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		didFinish = finished;
	}
	if (!didFinish) {
		return;
	}

	JoinWorker();
	std::string error;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		running = false;
		finished = false;
		error = errorText;
		errorText.clear();
	}

	if (!error.empty()) {
		const int extraArgs = ui->PushCallback("OnSubError");
		if (extraArgs >= 0) {
			lua_pushlightuserdata(ui->L, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
			lua_pushstring(ui->L, error.c_str());
			ui->PCall(extraArgs + 2, 0);
		}
		return;
	}

	const int extraArgs = ui->PushCallback("OnSubFinished");
	if (extraArgs < 0) {
		return;
	}
	for (int i = 2; i <= lua_gettop(L); ++i) {
		if (!(lua_isnil(L, i) || lua_isboolean(L, i) || lua_isnumber(L, i) || lua_isstring(L, i))) {
			char* msg = AllocStringLen(128);
			snprintf(msg, 128, "Subscript return %d: only nil, boolean, number and string can be returned from sub script", i - 1);
			ui->DoError("Runtime error in", msg);
			FreeString(msg);
			lua_settop(L, 1);
			break;
		}
	}
	lua_pushlightuserdata(ui->L, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
	ui->PCall(extraArgs + 1 + ssPushData(ui->L, ssBuildData(L, 2)), 0);
}

bool ui_subscript_c::IsRunning()
{
	std::lock_guard<std::mutex> lock(stateMutex);
	return running;
}

size_t ui_subscript_c::GetScriptMemory()
{
	return luaMemoryKilobytes.load(std::memory_order_relaxed);
}
