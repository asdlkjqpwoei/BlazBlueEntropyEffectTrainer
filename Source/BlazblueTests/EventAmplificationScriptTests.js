"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const header = fs.readFileSync(process.argv[2], "utf8");
const source = header.split('R"EventScript(')[1]?.split(')EventScript";')[0];
assert.ok(source, "The embedded event adapter is present");
let tests = 0;
function test(name, run) {
	run();
	console.log("Passed: " + name);
	++tests;
}

function fixture(options = {}) {
	const state = {enabled: true, online: false, coop: false, throw: false, nested: false, unrelated: false};
	const reward = (type, num, probability = 0) => ({type, num, probability});
	const row = (id, rewards, condition = 0) => ({id, type: 0, rewards, conditions: [{type: condition}]});
	const catalog = [
		row(1, [reward(11, 1)]),
		row(10, [reward(1, 100)]),
		row(20, [reward(8, -4000), reward(11, 3)], 8),
		row(21, [reward(11, 1, 10000)], 8),
		row(30, [reward(12, 1)]),
		row(40, [reward(11, 3, 5000)]),
		row(50, [reward(11, -1)]),
		{...row(60, [reward(11, 1)]), type: 3},
		{...row(99, []), type: 2}
	];
	const table = {All: catalog, Get(id) { return catalog.find(row => row.id === id); }};
	const randomCalls = [];
	const utilities = {RandomChoice(values) { randomCalls.push([...values]); return values[values.length - 1]; }};
	const initialRandom = utilities.RandomChoice;
	class Model {
		constructor() {
			this.IsMulti = false;
			this.initialized = true;
			this.originData = {finished: false};
			this.pool = [20, 10, 30, 40, 50, 60];
			this.must = [99];
			this.ineligible = new Set();
		}
		checkEventCondition(id) { return !this.ineligible.has(id); }
		checkEliteEvent() {}
		checkRoomEvents(room) {
			if (state.nested && !room.inner) this.checkRoomEvents({roomType: 22, events: [], inner: true});
			if (state.unrelated) utilities.RandomChoice(["unrelated"]);
			room.events.push(...this.must);
			let candidates = this.pool.filter(id => this.checkEventCondition(id));
			for (let draw = 0; draw < 3 && candidates.length; ++draw) {
				const id = utilities.RandomChoice(candidates);
				room.events.unshift(id);
				if (state.throw) throw new Error("Simulated generation failure");
				const condition = table.Get(id).conditions[0].type;
				candidates = candidates.filter(other => condition === 0 ? other !== id : table.Get(other).conditions[0].type !== condition);
			}
			if (room.roomType === 22) this.checkEliteEvent(room);
			return "original result";
		}
	}
	const model = Model._instance = new Model();
	const original = Model.prototype.checkRoomEvents;
	const modules = {
		"Server/Model/BreedDungeon": {ModelBreedDungeon: Model},
		"Gen/Xlsx": {Xlsx: {DungeonRoomEventConf: table}},
		"Gen/pbdef": {
			EBreedDungeonRoomType: {Event: 16}, EDungeonRoomEventType: {Immediate: 0},
			EDungeonRoomEventRewardType: {SanBuff: 11}, EDungeonRoomEventConditionType: {None: 0}
		},
		"Core/Util": {U: utilities},
		"csharp": {PvpDefine: {IsOnlineMode: () => state.online}},
		"GamePlay/Battle/BattleJs": {default: {get Instance() { return {Battle: {IsLocalCoopMode: state.coop}}; }}}
	};
	const load = name => { assert.ok(name in modules, name); return modules[name]; };
	// Stand-in for the C++ callback's contract; native policy has separate C++ tests.
	const accepted = new Set(["0/11/1/0", "0/11/3/0", "0/11/1/10000"]);
	const sandbox = {__BlazblueTrainerEventAmplificationV1: (...args) => state.enabled && (!args.length || accepted.has(args.join("/")))};
	if (options.loader === "puer") sandbox.puer = {module: {createRequire: () => load}};
	else sandbox.require = load;
	const context = vm.createContext(sandbox);
	const install = () => vm.runInContext(source, context);
	const room = () => ({roomType: 16, selectEvent: 0, events: []});
	return {state, Model, model, table, modules, utilities, initialRandom, randomCalls, original, install, room};
}

test("Forces an eligible pool reward and preserves mandatory choices and draw count", () => {
	const f = fixture();
	assert.equal(f.install(), "installed");
	const room = f.room();
	assert.equal(f.model.checkRoomEvents(room), "original result");
	assert.ok(room.events.includes(20));
	assert.ok(room.events.includes(99));
	assert.equal(room.events.length, 4);
	assert.equal(f.randomCalls.length, 3);
	assert.equal(f.utilities.RandomChoice, f.initialRandom);
});

test("Uses an existing unconditional option when the pool has no eligible amplification", () => {
	const f = fixture(); f.install(); f.model.ineligible.add(20);
	const room = f.room(); f.model.checkRoomEvents(room);
	assert.ok(room.events.includes(1));
	assert.ok(!room.events.includes(20));
	assert.equal(room.events.length, 4);
	assert.deepEqual(f.model.pool, [20, 10, 30, 40, 50, 60]);
});

test("Distinguishes guaranteed amplification from debuffs and probabilistic rewards", () => {
	const f = fixture(); f.install(); f.model.pool = [30, 40, 50, 60];
	const before = JSON.stringify(f.table.All);
	const room = f.room(); f.model.checkRoomEvents(room);
	assert.ok(room.events.includes(1));
	assert.equal(JSON.stringify(f.table.All), before);
});

test("An empty pool retains its mandatory skip option and gains one amplification option", () => {
	const f = fixture(); f.install(); f.model.pool = [];
	const room = f.room(); f.model.checkRoomEvents(room);
	assert.deepEqual(room.events, [1, 99]);
	assert.equal(f.randomCalls.length, 0);
});

test("Existing mandatory amplification does not bias or duplicate a random draw", () => {
	const f = fixture(); f.install(); f.model.must = [1, 99];
	const room = f.room(); f.model.checkRoomEvents(room);
	assert.deepEqual(f.randomCalls[0], f.model.pool);
	assert.equal(room.events.filter(id => id === 1).length, 1);
});

test("An unavailable mandatory reward does not suppress the usable amplification guarantee", () => {
	const f = fixture(); f.install(); f.model.must = [20, 99]; f.model.ineligible.add(20);
	const room = f.room(); f.model.checkRoomEvents(room);
	assert.ok(room.events.includes(1));
	assert.ok(room.events.includes(20));
	assert.ok(room.events.includes(99));
});

for (const mode of ["disabled", "online", "coop", "multi", "foreign", "uninitialized", "finished", "elite", "selected", "existing"]) {
	test("Preserves original generation for " + mode, () => {
		const f = fixture(); f.install();
		const room = f.room();
		if (mode === "disabled") f.state.enabled = false;
		if (mode === "online") f.state.online = true;
		if (mode === "coop") f.state.coop = true;
		if (mode === "multi") f.model.IsMulti = true;
		if (mode === "foreign") f.Model._instance = new f.Model();
		if (mode === "uninitialized") f.model.initialized = false;
		if (mode === "finished") f.model.originData.finished = true;
		if (mode === "elite") room.roomType = 22;
		if (mode === "selected") room.selectEvent = 10;
		if (mode === "existing") room.events = [10];
		const baseline = structuredClone(room);
		f.original.call(f.model, baseline);
		f.model.checkRoomEvents(room);
		assert.deepEqual(room, baseline);
		assert.equal(f.utilities.RandomChoice, f.initialRandom);
	});
}

test("Restores the shared random function when original generation throws", () => {
	const f = fixture(); f.install(); f.state.throw = true;
	assert.throws(() => f.model.checkRoomEvents(f.room()), /Simulated generation failure/);
	assert.equal(f.utilities.RandomChoice, f.initialRandom);
	f.state.throw = false;
	const room = f.room(); f.model.checkRoomEvents(room);
	assert.ok(room.events.includes(20));
});

test("Nested calls and unrelated random draws do not consume the forced draw", () => {
	const f = fixture(); f.install(); f.state.nested = f.state.unrelated = true;
	const room = f.room(); f.model.checkRoomEvents(room);
	assert.ok(room.events.includes(20));
	assert.equal(f.utilities.RandomChoice, f.initialRandom);
});

test("Repeated enable reuses one wrapper and disable/re-enable uses the native state", () => {
	const f = fixture(); f.install();
	const wrapped = f.Model.prototype.checkRoomEvents;
	f.install(); assert.equal(f.Model.prototype.checkRoomEvents, wrapped);
	f.state.enabled = false;
	const off = f.room(); f.model.checkRoomEvents(off); assert.ok(!off.events.includes(20));
	f.state.enabled = true;
	const on = f.room(); f.model.checkRoomEvents(on); assert.ok(on.events.includes(20));
});

test("Supports Puerts createRequire when global require is absent", () => {
	const f = fixture({loader: "puer"}); f.install();
	const room = f.room(); f.model.checkRoomEvents(room); assert.ok(room.events.includes(20));
});

for (const change of ["enum", "fallback", "method", "async method"]) {
	test("Rejects incompatible " + change + " before installing a wrapper", () => {
		const f = fixture();
		if (change === "enum") f.modules["Gen/pbdef"].EDungeonRoomEventRewardType.SanBuff = 999;
		if (change === "fallback") f.table.All.splice(0, 1);
		if (change === "method") f.Model.prototype.checkRoomEvents = function() {};
		if (change === "async method") f.Model.prototype.checkRoomEvents = async function() {
			// Even an asynchronous replacement with matching names must fail compatibility validation.
			return ["RandomChoice", "checkEventCondition", "unshift", "checkEliteEvent"];
		};
		const before = f.Model.prototype.checkRoomEvents;
		assert.throws(f.install, /compatible event-amplification API/);
		assert.equal(f.Model.prototype.checkRoomEvents, before);
		assert.equal(f.utilities.RandomChoice, f.initialRandom);
	});
}

console.log(`${tests} event-amplification adapter tests passed.`);
