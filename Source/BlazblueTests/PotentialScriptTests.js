"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

const source = fs.readFileSync(process.argv[2], "utf8");
let tests = 0;
const test = (name, action) => {
    action();
    ++tests;
    console.log("Passed: " + name);
};

function fixture(kind = "BattleBreed", loader = "commonjs") {
    const actor = {
        id: 37, uid: 812, score: 100,
        potentials: [{id: 101, history: [{unknown: 73}], extra: "preserve"}],
        baseAttrs: [{type: 1, value: 0}, {type: 2, value: 50}],
        activeAttrs: [{type: 3, value: 0}],
        talents: [{id: 71}], skills: [{id: 81}], blesses: [{id: 91}],
        unrelated: {preserve: true}
    };
    const client = structuredClone(actor);
    const rows = [
        {id: 103, actorId: 37, score: 30, preLabels: [999]},
        {id: 101, actorId: 37, score: 10, isInitial: true},
        {id: 102, actorId: 37, score: 20},
        {id: 104, actorId: 37, invalid: true, score: 9999},
        {id: 105, actorId: 38, score: 9999},
        {id: 106, actorId: 37, isDamageSkill: true, score: 9999}
    ];
    const calls = {dirty: 0, triggers: 0, skills: 0, ui: 0, grants: [], scored: []};
    const data = kind === "BattleBreed"
        ? {baseInfo: {actorId: 37}, actorInfo: {fesActor: actor}, finished: false}
        : {fesActor: actor, mode: 1, finalBossFinished: false};
    const model = {
        originData: data,
        _potentialRandomGenerator: {old: true},
        dirtyChecker: {forceSetDirty() { ++calls.dirty; }},
        get FesActor() { return actor; }
    };
    const player = {
        BaseID: 37, Attr: {IsHPZero: false},
        SkillMgr: {MarkSkillChainNeedRebuild() { ++calls.skills; }}
    };
    const battle = {
        BattleClassName: kind,
        Battle: {PlayerSelf: player, IsLocalCoopMode: false},
        GetFesActor() { return client; },
        AddPlayerTriggersFromFesActor(target, value) {
            assert.equal(target, player);
            assert.equal(value, client);
            assert.deepEqual(client.potentials.map(p => p.id), actor.potentials.map(p => p.id));
            ++calls.triggers;
        },
        UIMain: {RefreshBlessStatusUI(force) { assert.equal(force, true); ++calls.ui; }}
    };
    const utilities = {
        DeepCopy(value) {
            assert.equal(this, utilities);
            return structuredClone(value);
        }
    };
    const potentialType = {
        create(value) {
            assert.equal(this, potentialType);
            return {...value};
        }
    };
    const actorUtils = {
        AddPotential(target, potential) {
            assert.equal(target, actor);
            calls.grants.push(potential.id);
            potential.active = potential.timestamp = potential.order = null;
            if (!target.potentials.some(p => p.id === potential.id)) target.potentials.push(potential);
        },
        CalcFesActorScoreNoAttrModify(target) {
            assert.notEqual(target, actor);
            calls.scored.push(target);
            target.baseAttrs = target.baseAttrs.filter(attr => attr.value);
            target.activeAttrs = target.activeAttrs.filter(attr => attr.value);
            target.score = target.potentials.reduce((total, potential) =>
                total + (rows.find(row => row.id === potential.id)?.score ?? 0), 0);
        }
    };
    const modelType = {_instance: model};
    Object.defineProperty(modelType, "Instance", {get() { throw new Error("Must not create a singleton."); }});
    const modules = {
        csharp: {PvpDefine: {IsOnlineMode: () => false}},
        "GamePlay/Battle/BattleJs": {default: {Instance: battle}},
        "Server/Model/BreedDungeon": {ModelBreedDungeon: kind === "BattleBreed" ? modelType : {_instance: null}},
        "Server/Model/ExploreDungeon": {ModelExploreDungeon: kind === "BattleExplore" ? modelType : {_instance: null}},
        "Gen/Xlsx": {Xlsx: {BaseActorPotentialConf: {All: rows}}},
        "Server/Module/Actor": {FesActorUtils: actorUtils},
        "Gen/pbdef": {STPotential: potentialType},
        "Core/Util": {U: utilities}
    };
    const gameRequire = name => {
        assert.ok(Object.hasOwn(modules, name), "Unexpected game module: " + name);
        return modules[name];
    };
    const globals = {};
    if (loader === "commonjs") {
        globals.require = gameRequire;
    } else {
        globals[loader] = {module: {createRequire(referer) {
            assert.equal(referer, "");
            return (name, immediate) => {
                assert.equal(immediate, true);
                return gameRequire(name);
            };
        }}};
    }
    const context = vm.createContext(globals);
    const grant = vm.runInContext("(" + source + ")", context);
    return {actor, client, rows, calls, data, model, player, battle, modules, utilities, actorUtils, globals,
        run: () => JSON.parse(grant())};
}

function unchanged(f, before) {
    assert.deepEqual(f.actor, before);
    assert.equal(f.calls.dirty, 0);
    assert.equal(f.calls.triggers, 0);
    assert.equal(f.calls.skills, 0);
    assert.equal(f.calls.ui, 0);
}

test("breed grants only missing current-character potentials and synchronizes combat", () => {
    const f = fixture();
    const before = structuredClone(f.actor);
    const existing = f.actor.potentials[0];
    assert.deepEqual(f.run(), {ok: true, actorId: 37, added: 2, total: 3});
    assert.deepEqual(f.calls.grants, [102, 103]);
    assert.deepEqual(f.actor.potentials.map(p => p.id), [101, 102, 103]);
    assert.equal(f.actor.potentials[0], existing);
    assert.deepEqual(existing, before.potentials[0]);
    assert.deepEqual(f.client.potentials, f.actor.potentials);
    assert.notEqual(f.client.potentials, f.actor.potentials);
    assert.equal(f.actor.score, 60);
    assert.equal(f.client.score, 60);
    assert.equal(f.model._potentialRandomGenerator, null);
    assert.deepEqual([f.calls.dirty, f.calls.triggers, f.calls.skills, f.calls.ui], [1, 1, 1, 1]);
    for (const key of ["baseAttrs", "activeAttrs", "talents", "skills", "blesses", "unrelated"]) {
        assert.deepEqual(f.actor[key], before[key], key + " must not change.");
        assert.deepEqual(f.client[key], before[key], key + " must not change on the client.");
    }
});

test("repeated clicks are a no-op, including existing repeatable damage stacks", () => {
    const f = fixture();
    f.actor.potentials.push({id: 106}, {id: 106});
    f.client.potentials.push({id: 106}, {id: 106});
    assert.equal(f.run().added, 2);
    const before = structuredClone(f.actor);
    const cache = f.model._potentialRandomGenerator = {rebuilt: true};
    const calls = {...f.calls, grants: [...f.calls.grants], scored: [...f.calls.scored]};
    assert.deepEqual(f.run(), {ok: true, actorId: 37, added: 0, total: 3});
    assert.deepEqual(f.actor, before);
    assert.equal(f.model._potentialRandomGenerator, cache);
    assert.deepEqual(f.calls, calls);
    assert.equal(f.actor.potentials.filter(p => p.id === 106).length, 2);
});

test("exploration uses its own authoritative model, not an old breed run", () => {
    const f = fixture("BattleExplore");
    const old = fixture().model;
    f.modules["Server/Model/BreedDungeon"].ModelBreedDungeon._instance = old;
    const oldData = structuredClone(old.originData);
    assert.deepEqual(f.run(), {ok: true, actorId: 37, added: 2, total: 3});
    assert.deepEqual(old.originData, oldData);
    assert.deepEqual(f.client.potentials.map(p => p.id), [101, 102, 103]);
    assert.equal(f.calls.triggers, 1);
});

for (const alias of ["puer", "puerts"]) {
    test(alias + " module loader works without a global CommonJS require", () => {
        const f = fixture("BattleBreed", alias);
        assert.deepEqual(f.run(), {ok: true, actorId: 37, added: 2, total: 3});
        assert.equal(f.calls.triggers, 1);
    });
}

for (const [name, configure] of [
    ["missing module loader", f => { delete f.globals.require; }],
    ["online", f => { f.modules.csharp.PvpDefine.IsOnlineMode = () => true; }],
    ["local coop", f => { f.battle.Battle.IsLocalCoopMode = true; }],
    ["lobby", f => { f.modules["GamePlay/Battle/BattleJs"].default.Instance = null; }],
    ["training", f => { f.battle.BattleClassName = "BattlePotentialTest"; }],
    ["missing player", f => { f.battle.Battle.PlayerSelf = null; }],
    ["dead character", f => { f.player.Attr.IsHPZero = true; }],
    ["finished run", f => { f.data.finished = true; }],
    ["missing model", f => { f.modules["Server/Model/BreedDungeon"].ModelBreedDungeon._instance = null; }],
    ["different character", f => { f.client.id = 38; }],
    ["different body uid", f => { f.client.uid = 813; }],
    ["different native player", f => { f.player.BaseID = 38; }],
    ["different base info", f => { f.data.baseInfo.actorId = 38; }],
    ["unsynchronized potentials", f => { f.client.potentials.push({id: 102}); }],
    ["invalid owned id", f => { f.actor.potentials[0].id = 0; }],
    ["missing save-dirty marker", f => { f.model.dirtyChecker = null; }],
    ["missing trigger API", f => { f.battle.AddPlayerTriggersFromFesActor = null; }],
    ["missing skill-chain API", f => { f.player.SkillMgr.MarkSkillChainNeedRebuild = null; }],
    ["missing score API", f => { f.actorUtils.CalcFesActorScoreNoAttrModify = null; }],
    ["empty catalog", f => { f.rows.length = 0; }],
    ["duplicate catalog ids", f => { f.rows.push({...f.rows[0]}); }],
    ["invalid catalog id", f => { f.rows.push({id: -1, actorId: 37}); }],
    ["out-of-range catalog id", f => { f.rows.push({id: 0x100000000, actorId: 37}); }],
    ["bad potential factory", f => { f.modules["Gen/pbdef"].STPotential.create = () => ({id: 0}); }],
    ["broken copy", f => { f.utilities.DeepCopy = () => null; }],
    ["score failure", f => { f.actorUtils.CalcFesActorScoreNoAttrModify = () => { throw new Error("score failed"); }; }]
]) {
    test(name + " is blocked before mutation", () => {
        const f = fixture();
        configure(f);
        const before = structuredClone(f.actor);
        const result = f.run();
        assert.equal(result.ok, false);
        assert.equal(result.added, 0);
        assert.ok(result.error.length > 0);
        unchanged(f, before);
    });
}

test("finished exploration is blocked", () => {
    const f = fixture("BattleExplore");
    f.data.finalBossFinished = true;
    const before = structuredClone(f.actor);
    assert.equal(f.run().ok, false);
    unchanged(f, before);
});

test("partial grant errors report the retained count instead of success", () => {
    const f = fixture();
    const add = f.actorUtils.AddPotential;
    f.actorUtils.AddPotential = (actor, potential) => {
        add(actor, potential);
        if (potential.id === 102) throw new Error("grant failed after append");
    };
    const result = f.run();
    assert.equal(result.ok, false);
    assert.equal(result.added, 1);
    assert.match(result.error, /some changes may already have applied/);
    assert.match(result.error, /grant failed after append/);
    assert.deepEqual(f.actor.potentials.map(p => p.id), [101, 102]);
    assert.equal(f.calls.triggers, 0);
});

test("game rejection cannot become a successful grant", () => {
    const f = fixture();
    f.actorUtils.AddPotential = () => {};
    const result = f.run();
    assert.equal(result.ok, false);
    assert.equal(result.added, 0);
    assert.match(result.error, /did not retain/);
    assert.equal(f.calls.triggers, 0);
});

test("combat synchronization failures remain explicit after model changes", () => {
    const f = fixture();
    f.battle.AddPlayerTriggersFromFesActor = () => { throw new Error("trigger failed"); };
    const result = f.run();
    assert.equal(result.ok, false);
    assert.equal(result.added, 2);
    assert.match(result.error, /some changes may already have applied/);
    assert.match(result.error, /trigger failed/);
    assert.equal(f.calls.ui, 0);
});

console.log("All " + tests + " potential-script tests passed.");
