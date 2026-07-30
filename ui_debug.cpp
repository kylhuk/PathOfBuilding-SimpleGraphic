// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// Module: UI Debug
//

#include "ui_local.h"

#include <algorithm>
#include <string>
#include <vector>

// =======
// Classes
// =======

struct d_lineHit_s {
	std::string source;
	std::string name;
	int line = 0;
	int count = 0;
};

struct d_callHit_s {
	std::string source;
	std::string name;
	int count = 0;
	std::vector<d_lineHit_s> lineHits;
};

// ===================
// ui_IDebug Interface
// ===================

class ui_debug_c : public ui_IDebug {
public:
	void SetProfiling(bool enable) override;
	void ToggleProfiling() override;

	explicit ui_debug_c(ui_main_c* ui);
	~ui_debug_c();

	void RecordHook(lua_State* state, lua_Debug* activation);

private:
	ui_main_c* ui = nullptr;
	bool profiling = false;
	std::vector<d_lineHit_s> lineHits;
	std::vector<d_callHit_s> callHits;

	void AddLineHit(std::vector<d_lineHit_s>& hits, lua_Debug const& debug);
	void ReportAndReset();
};

ui_IDebug* ui_IDebug::GetHandle(ui_main_c* ui)
{
	return new ui_debug_c(ui);
}

void ui_IDebug::FreeHandle(ui_IDebug* hnd)
{
	delete static_cast<ui_debug_c*>(hnd);
}

ui_debug_c::ui_debug_c(ui_main_c* ui)
	: ui(ui)
{
}

ui_debug_c::~ui_debug_c()
{
	SetProfiling(false);
}

// ==============
// UI Debug Class
// ==============

static ui_debug_c* GetDebugPtr(lua_State* state)
{
	lua_rawgeti(state, LUA_REGISTRYINDEX, ui_main_c::REGISTRY_KEY);
	auto* ui = static_cast<ui_main_c*>(lua_touserdata(state, -1));
	lua_pop(state, 1);
	return ui ? static_cast<ui_debug_c*>(ui->debug) : nullptr;
}

static void debugHook(lua_State* state, lua_Debug* activation)
{
	if (auto* debug = GetDebugPtr(state)) {
		debug->RecordHook(state, activation);
	}
}

void ui_debug_c::AddLineHit(std::vector<d_lineHit_s>& hits, lua_Debug const& debug)
{
	char const* source = debug.source ? debug.source : "?";
	char const* name = debug.name ? debug.name : "?";
	for (auto& hit : hits) {
		if (hit.line == debug.currentline && hit.source == source) {
			if (hit.name == "?" && debug.name) {
				hit.name = debug.name;
			}
			++hit.count;
			return;
		}
	}
	hits.push_back({ source, name, debug.currentline, 1 });
}

void ui_debug_c::RecordHook(lua_State* state, lua_Debug* activation)
{
	if (!profiling || !activation) {
		return;
	}

	// Hooks run synchronously on the Lua-owning thread. Unlike the previous
	// detached worker, this never touches LuaJIT from a foreign thread.
	lua_Debug current = *activation;
	if (!lua_getinfo(state, "Sln", &current) || !current.source) {
		return;
	}
	AddLineHit(lineHits, current);

	char const* source = current.source;
	char const* name = current.name ? current.name : "?";
	auto call = std::find_if(callHits.begin(), callHits.end(),
		[source, name](d_callHit_s const& hit) {
			return hit.source == source && hit.name == name;
		});
	if (call == callHits.end()) {
		callHits.push_back({ source, name, 0, {} });
		call = std::prev(callHits.end());
	}
	++call->count;
	AddLineHit(call->lineHits, current);
}

void ui_debug_c::ReportAndReset()
{
	auto byCount = [](auto const& a, auto const& b) {
		return a.count > b.count;
	};

	std::sort(lineHits.begin(), lineHits.end(), byCount);
	ui->sys->con->Printf("Hot lines:\n");
	for (size_t index = 0; index < lineHits.size() && index < 20; ++index) {
		auto const& hit = lineHits[index];
		ui->sys->con->Printf("%s(%d) in '%s': %d\n",
			hit.source.c_str(), hit.line, hit.name.c_str(), hit.count);
	}

	std::sort(callHits.begin(), callHits.end(), byCount);
	ui->sys->con->Printf("Hot calls:\n");
	for (size_t callIndex = 0; callIndex < callHits.size(); ++callIndex) {
		auto& call = callHits[callIndex];
		std::sort(call.lineHits.begin(), call.lineHits.end(), byCount);
		if (callIndex < 10) {
			ui->sys->con->Printf("%s in '%s': %d\n",
				call.source.c_str(), call.name.c_str(), call.count);
		}
		for (size_t lineIndex = 0; callIndex < 10 && lineIndex < call.lineHits.size() && lineIndex < 5; ++lineIndex) {
			auto const& hit = call.lineHits[lineIndex];
			ui->sys->con->Printf("\t%s(%d) in '%s': %d\n",
				hit.source.c_str(), hit.line, hit.name.c_str(), hit.count);
		}
	}

	lineHits.clear();
	callHits.clear();
}

void ui_debug_c::SetProfiling(bool enable)
{
	if (enable == profiling) {
		return;
	}

	if (enable) {
		lineHits.clear();
		callHits.clear();
		profiling = true;
		lua_sethook(ui->L, debugHook, LUA_MASKLINE, 0);
		ui->sys->con->Printf("Profiling enabled.\n");
	}
	else {
		if (ui && ui->L) {
			lua_sethook(ui->L, nullptr, 0, 0);
		}
		if (profiling) {
			profiling = false;
			ui->sys->con->Printf("Profiling finished:\n");
			ReportAndReset();
		}
	}
}

void ui_debug_c::ToggleProfiling()
{
	SetProfiling(!profiling);
}
