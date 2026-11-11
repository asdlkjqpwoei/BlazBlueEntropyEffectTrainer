#include "EventAmplificationCallback.h"
#include "EventAmplificationScript.h"
#include "Paths.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	constexpr char Fixture[] = R"ProbeScript(
(() => {
	const reward = (type, num, probability = 0) => ({type, num, probability});
	const event = (id, rewards, condition = 0) => ({id, type: 0, rewards, conditions: [{type: condition}]});
	const rows = [event(1, [reward(11, 1)]), event(10, [reward(1, 100)]),
		event(20, [reward(8, -4000), reward(11, 3)], 8), event(30, [reward(12, 1)]),
		event(40, [reward(11, 3, 5000)]), {...event(99, []), type: 2}];
	const table = {All: rows, Get(id) { return rows.find(row => row.id === id); }};
	const state = {online: false, coop: false, fail: false};
	const U = {RandomChoice(values) { return values[values.length - 1]; }};
	const random = U.RandomChoice;
	class Model {
		constructor() {
			this.initialized = true; this.IsMulti = false; this.originData = {finished: false};
			this.pool = [20, 10, 30, 40]; this.must = [99]; this.ineligible = new Set();
		}
		checkEventCondition(id) { return !this.ineligible.has(id); }
		checkEliteEvent() {}
		checkRoomEvents(room) {
			room.events.push(...this.must);
			let candidates = this.pool.filter(id => this.checkEventCondition(id));
			for (let i = 0; i < 3 && candidates.length; ++i) {
				const id = U.RandomChoice(candidates);
				room.events.unshift(id);
				if (state.fail) throw new Error("simulated generator failure");
				const kind = table.Get(id).conditions[0].type;
				candidates = candidates.filter(other => kind === 0 ? other !== id : table.Get(other).conditions[0].type !== kind);
			}
			if (room.roomType === 22) this.checkEliteEvent(room);
		}
	}
	const model = Model._instance = new Model();
	const modules = {
		"Server/Model/BreedDungeon": {ModelBreedDungeon: Model}, "Gen/Xlsx": {Xlsx: {DungeonRoomEventConf: table}},
		"Gen/pbdef": {EBreedDungeonRoomType: {Event: 16}, EDungeonRoomEventType: {Immediate: 0},
			EDungeonRoomEventRewardType: {SanBuff: 11}, EDungeonRoomEventConditionType: {None: 0}},
		"Core/Util": {U}, "csharp": {PvpDefine: {IsOnlineMode: () => state.online}},
		"GamePlay/Battle/BattleJs": {default: {get Instance() { return {Battle: {IsLocalCoopMode: state.coop}}; }}}
	};
	globalThis.require = name => modules[name];
	globalThis.probe = {state, U, random, model, table, Model, generate(type = 16) {
		const room = {roomType: type, events: [], selectEvent: 0}; model.checkRoomEvents(room); return room;
	}};
	return "ready";
})()
)ProbeScript";
}

int RunEventAmplificationRuntimeProbe(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount != 2)
	{
		std::cerr << "Usage: --event-amplification-runtime-probe <puerts.dll path>\n";
		return 1;
	}
	const auto Path = Trainer::PathFromUtf8(Arguments[1]);
	const auto Library = LoadLibraryExW(Path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!Library) throw std::runtime_error("Could not load the selected Puerts runtime for isolated verification.");
	struct LibraryOwner { HMODULE Value; ~LibraryOwner() { FreeLibrary(Value); } } LibraryOwner{Library};
	using Trainer::Bridge::Export;
	const auto Create = Export<void* (*)(int)>(Library, "CreateJSEngine");
	const auto Destroy = Export<void (*)(void*)>(Library, "DestroyJSEngine");
	const auto Version = Export<int (*)()>(Library, "GetLibVersion");
	const auto Eval = Export<void* (*)(void*, const char*, const char*)>(Library, "Eval");
	const auto Type = Export<int (*)(void*)>(Library, "GetResultType");
	const auto String = Export<const char* (*)(void*, int*)>(Library, "GetStringFromResult");
	const auto Reset = Export<void (*)(void*)>(Library, "ResetResult");
	const auto Exception = Export<const char* (*)(void*, int*)>(Library, "GetLastExceptionInfo");
	bool Enabled = true;
	Trainer::Bridge::EventAmplificationCallback Native(Library,
		[](void* State) noexcept { return *static_cast<bool*>(State); }, &Enabled);
	// Destroy the engine before its bound native callback, then release the library last.
	std::unique_ptr<void, decltype(Destroy)> Engine(Create(-1), Destroy);
	if (!Engine) throw std::runtime_error("Could not create an isolated Puerts engine.");
	Native.Bind(Engine.get());
	const auto Execute = [&](const std::string& Script) {
		const auto Value = Eval(Engine.get(), Script.c_str(), "BlazblueTrainer/EventAmplificationVerification");
		if (!Value)
		{
			int Length{};
			const auto Detail = Exception(Engine.get(), &Length);
			throw std::runtime_error("Puerts verification failed: " +
				(Detail && Length > 0 && Length < 16384 ? std::string(Detail, Length) : "no exception details"));
		}
		std::unique_ptr<void, decltype(Reset)> Result(Value, Reset);
		int Length{};
		const auto Text = Type(Value) == 8 ? String(Value, &Length) : nullptr;
		if (!Text || Length < 0 || Length > 16384) throw std::runtime_error("Invalid Puerts verification result.");
		return std::string(Text, Length);
	};
	int Groups{};
	const auto Check = [&](const char* Name, const std::string& Body) {
		const auto Result = Execute("(() => { const assert = (value) => { if (!value) throw new Error('verification assertion failed'); }; " +
			Body + "; return 'passed'; })()");
		if (Result != "passed") throw std::runtime_error(Name);
		++Groups;
		std::cout << "Passed: " << Name << '\n';
	};
	Check("real V8 calls the C++ session gate and reward classifier", R"JS(
		const n = globalThis.__BlazblueTrainerEventAmplificationV1;
		assert(n() === true); assert(n(0, 11, 1, 0) === true); assert(n(0, 11, 3, 10000) === true);
		assert(n(0, 12, 1, 0) === false); assert(n(0, 11, 3, 5000) === false);
		assert(n(3, 11, 1, 0) === false); assert(n(0, 11, -1, 0) === false);
	)JS");
	Check("native callback rejects invalid types, argument counts and numeric bounds", R"JS(
		const n = globalThis.__BlazblueTrainerEventAmplificationV1;
		for (const amount of [NaN, Infinity, -Infinity, 1.5, 2147483648, -2147483649, '1', true, null, undefined, {}])
			assert(n(0, 11, amount, 0) === false);
		assert(n('0', 11, 1, 0) === false); assert(n(0, 11, 1) === false); assert(n(0, 11, 1, 0, 1) === false);
	)JS");
	if (Execute(Fixture) != "ready" || Execute(Trainer::Bridge::EventAmplificationScript) != "installed")
		throw std::runtime_error("The real-runtime adapter installation failed.");
	Check("native policy guarantees eligible choices over 100 generations", R"JS(
		for (let i = 0; i < 100; ++i) {
			const r = probe.generate(); assert(r.events.includes(20)); assert(r.events.includes(99));
			assert(r.events.length === 4); assert(probe.U.RandomChoice === probe.random);
		}
	)JS");
	Check("conditions remain authoritative and fallback is unconditional", R"JS(
		probe.model.ineligible.add(20); const r = probe.generate();
		assert(r.events.includes(1)); assert(!r.events.includes(20)); probe.model.ineligible.clear();
	)JS");
	Enabled = false;
	Check("disabling native state immediately restores original generation", R"JS(
		assert(__BlazblueTrainerEventAmplificationV1() === false);
		assert(__BlazblueTrainerEventAmplificationV1(0, 11, 1, 0) === false);
		const r = probe.generate(); assert(r.events.join(',') === '10,30,40,99');
	)JS");
	Enabled = true;
	Check("re-enabling the same native callback resumes the guarantee", "assert(probe.generate().events.includes(20))");
	Check("online, local coop and elite events retain original generation", R"JS(
		probe.state.online = true; assert(!probe.generate().events.includes(20)); probe.state.online = false;
		probe.state.coop = true; assert(!probe.generate().events.includes(20)); probe.state.coop = false;
		assert(!probe.generate(22).events.includes(20));
	)JS");
	Check("the native-backed adapter restores the random function on exceptions", R"JS(
		probe.state.fail = true; let caught = false; try { probe.generate(); } catch (_) { caught = true; }
		probe.state.fail = false; assert(caught); assert(probe.U.RandomChoice === probe.random);
		assert(probe.generate().events.includes(20));
	)JS");
	Check("empty pools keep mandatory options and add the fallback", R"JS(
		const pool = probe.model.pool; probe.model.pool = []; assert(probe.generate().events.join(',') === '1,99');
		probe.model.pool = pool;
	)JS");
	Check("existing amplification is preserved without duplicates", R"JS(
		probe.model.must = [1, 99]; const r = probe.generate(); assert(r.events.filter(id => id === 1).length === 1);
		probe.model.must = [99];
	)JS");
	Execute("globalThis.savedEventWrapper = probe.Model.prototype.checkRoomEvents; 'saved'");
	if (Execute(Trainer::Bridge::EventAmplificationScript) != "installed") throw std::runtime_error("Repeated installation failed.");
	Check("repeated installation retains exactly one wrapper", "assert(savedEventWrapper === probe.Model.prototype.checkRoomEvents)");
	std::cout << Groups << " native Puerts callback/adapter groups passed; library_version=" << Version()
		<< "; isolated_engine=true; game_process_used=false\n";
	return 0;
}
