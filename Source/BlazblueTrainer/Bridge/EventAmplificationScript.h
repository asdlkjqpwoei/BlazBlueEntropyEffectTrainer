#pragma once

namespace Trainer::Bridge
{
	// Only adapts the game's JavaScript model to the native policy and session.
	// Reward classification and the live enable/heartbeat check stay in C++.
	inline constexpr char EventAmplificationScript[] = R"EventScript(
(function installEventAmplification() {
	const incompatible = () => { throw new Error("The current game does not expose a compatible event-amplification API."); };
	let load = typeof require === "function" ? require : null;
	if (!load) {
		const runtime = typeof puerts !== "undefined" ? puerts : typeof puer !== "undefined" ? puer : null;
		if (typeof runtime?.module?.createRequire !== "function") incompatible();
		load = runtime.module.createRequire("");
	}
	const native = globalThis.__BlazblueTrainerEventAmplificationV1;
	const model = load("Server/Model/BreedDungeon", true).ModelBreedDungeon;
	const table = load("Gen/Xlsx", true).Xlsx?.DungeonRoomEventConf;
	const pb = load("Gen/pbdef", true);
	const utilities = load("Core/Util", true).U;
	const cs = load("csharp", true);
	const battleClass = load("GamePlay/Battle/BattleJs", true).default;
	const prototype = model?.prototype;
	const original = prototype?.checkRoomEvents;
	const marker = "__blazblueTrainerEventAmplificationV1";
	const validId = id => Number.isInteger(id) && id > 0 && id <= 0xffffffff;
	if (typeof native !== "function" || typeof original !== "function" ||
		Object.prototype.toString.call(original) !== "[object Function]" ||
		typeof prototype.checkEventCondition !== "function" || typeof table?.Get !== "function" ||
		!Array.isArray(table.All) || table.All.length > 4096 ||
		typeof utilities?.RandomChoice !== "function" || typeof cs.PvpDefine?.IsOnlineMode !== "function" ||
		pb.EBreedDungeonRoomType?.Event !== 16 || pb.EDungeonRoomEventType?.Immediate !== 0 ||
		pb.EDungeonRoomEventRewardType?.SanBuff !== 11 || pb.EDungeonRoomEventConditionType?.None !== 0) incompatible();
	const guaranteed = row => row && validId(row.id) && Array.isArray(row.rewards) && row.rewards.some(
		reward => native(row.type, reward.type, reward.num, reward.probability));
	// A game-defined, unconditional +1 option is the fallback for empty or unsuitable pools.
	// Never fabricate an event ID, bypass a cost, or force a probabilistic reward to succeed.
	const fallback = table.All.find(row => guaranteed(row) && Array.isArray(row.conditions) &&
		row.conditions.every(condition => condition.type === 0) && row.rewards.length === 1 && row.rewards[0].num === 1);
	if (!fallback) incompatible();
	if (original[marker] === 1) return "installed";
	const source = Function.prototype.toString.call(original);
	if (!["RandomChoice", "checkEventCondition", "unshift", "checkEliteEvent"].every(name => source.includes(name))) incompatible();
	let depth = 0;
	function checkRoomEvents(room, ...args) {
		// The original method is synchronous. Nested calls must not consume this room's forced draw.
		if (depth) {
			++depth;
			try { return original.call(this, room, ...args); } finally { --depth; }
		}
		if (!native() || this !== model._instance || !this.initialized || this.originData?.finished ||
			this.IsMulti !== false || cs.PvpDefine.IsOnlineMode() || battleClass?.Instance?.Battle?.IsLocalCoopMode ||
			room?.roomType !== 16 || room.selectEvent || !Array.isArray(room.events) || room.events.length !== 0) {
			return original.call(this, room, ...args);
		}
		const random = utilities.RandomChoice;
		const thisModel = this;
		let drew = false;
		utilities.RandomChoice = function(values, ...randomArgs) {
			if (depth !== 1 || drew || !native() || !Array.isArray(values) || !values.length ||
				values.length > 4096 || !values.every(id => validId(id) && table.Get(id))) {
				return random.call(this, values, ...randomArgs);
			}
			drew = true;
			// Mandatory options have already been appended by the game before its first draw.
			if (room.events.some(id => guaranteed(table.Get(id)) && thisModel.checkEventCondition(id))) return random.call(this, values, ...randomArgs);
			const preferred = values.filter(id => guaranteed(table.Get(id)));
			const choices = preferred.length ? preferred : thisModel.checkEventCondition(fallback.id) ? [fallback.id] : values;
			return random.call(this, choices, ...randomArgs);
		};
		++depth;
		let result;
		try { result = original.call(this, room, ...args); }
		finally { --depth; utilities.RandomChoice = random; }
		// No draw occurs when the original pool has no eligible random events. Keep its mandatory options.
		if (!drew && native() && !room.events.some(id => guaranteed(table.Get(id)) && this.checkEventCondition(id)) && this.checkEventCondition(fallback.id)) {
			room.events.unshift(fallback.id);
		}
		return result;
	}
	Object.defineProperty(checkRoomEvents, marker, {value: 1});
	prototype.checkRoomEvents = checkRoomEvents;
	if (prototype.checkRoomEvents !== checkRoomEvents) incompatible();
	return "installed";
})()
)EventScript";
}
