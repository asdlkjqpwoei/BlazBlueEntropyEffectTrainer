#include "Config.h"
#include "Paths.h"
#include "PerfectGuard.h"
#include "PerfectHooks.h"
#include "PerfectProtocol.h"
#include "TrainerApi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
	struct Player;
	struct Statistics
	{
		void* Header[2]{};
		Player* Owner{};
		std::int32_t Hits{};
		std::int32_t Other{};
		std::int64_t Damage{};
	};
	struct Kills { void* Header[2]{}; int Count{7}; int Extra{3}; Player* Owner{}; };
	struct Player
	{
		void* Header[2]{};
		Statistics* Stats{};
		Kills* Streak{};
		int HP{100};
		int HitNotifications{};
		bool Dead{};
	};
	struct ScriptCall;
	void CallScript(ScriptCall&);
	struct Hit { int Damage{10}; Player* Nested{}; bool Throw{}; ScriptCall* Script{}; };
	Player* Protected{};
	bool Enabled{}, Online{}, Coop{};
	__declspec(noinline) void NativeStats(void* Object, void* Data, const void*)
	{
		auto& Stats = *static_cast<Statistics*>(Object);
		const auto& Value = *static_cast<Hit*>(Data);
		++Stats.Hits;
		Stats.Damage += Value.Damage;
		++Stats.Other;
		if (Value.Throw) throw std::runtime_error("simulated hit failure");
	}
	__declspec(noinline) void NativeReset(void* Object, const void*)
	{
		auto& Value = *static_cast<Kills*>(Object);
		Value.Count = 0;
		Value.Extra = 0;
	}
	void CallNotify(Player&, Hit&);
	__declspec(noinline) void NativeNotify(void* Object, void* Data, const void*)
	{
		auto& Target = *static_cast<Player*>(Object);
		auto& Value = *static_cast<Hit*>(Data);
		++Target.HitNotifications;
		if (Value.Nested) { Hit Nested; CallNotify(*Value.Nested, Nested); }
		if (Value.Script) CallScript(*Value.Script);
		Target.HP -= Value.Damage;
		Target.Dead = Target.HP <= 0;
		Trainer::PerfectResetFunction volatile Reset = NativeReset;
		Trainer::PerfectHitFunction volatile Statistics = NativeStats;
		Reset(Target.Streak, nullptr);
		Statistics(Target.Stats, Data, nullptr);
	}
	void CallNotify(Player& Target, Hit& Value)
	{
		Trainer::PerfectHitFunction volatile Notify = NativeNotify;
		Notify(&Target, &Value, nullptr);
	}
	void CallReset(Kills& Target)
	{
		Trainer::PerfectResetFunction volatile Reset = NativeReset;
		Reset(&Target, nullptr);
	}
	struct ScriptCall
	{
		Kills* Target{};
		const char* Name{"End_pt_behit"};
		ScriptCall* Nested{};
		bool Throw{};
		int Calls{};
	};
	__declspec(noinline) void NativeScript(void*, void*, void* Parameters, void*, const void*)
	{
		auto& Request = *static_cast<ScriptCall*>(Parameters);
		++Request.Calls;
		if (Request.Nested) CallScript(*Request.Nested);
		CallReset(*Request.Target);
		if (Request.Throw) throw std::runtime_error("simulated deferred hit failure");
	}
	void CallScript(ScriptCall& Request)
	{
		Trainer::PerfectScriptFunction volatile Script = NativeScript;
		Script(nullptr, const_cast<char*>(Request.Name), &Request, nullptr, nullptr);
	}
	bool IsHitResetScript(void* Name) { return Name && std::strcmp(static_cast<const char*>(Name), "End_pt_behit") == 0; }
	bool ShouldProtect(void* Player) { return Enabled && !Online && !Coop && Player == Protected; }
	struct Actor
	{
		Statistics Stats;
		Kills Streak;
		Player Self;
		Actor() { Self.Stats = &Stats; Self.Streak = &Streak; Stats.Owner = &Self; Streak.Owner = &Self; }
	};
}

int RunPerfectTests()
{
	int Failures{};
	const auto Require = [&](bool Condition, const char* Message)
	{
		if (!Condition) { std::cerr << Message << '\n'; ++Failures; }
	};
	std::string Error;
	Require(Trainer::KeepPerfectGuardForStatus(TrainerStateChecking, true) &&
		Trainer::KeepPerfectGuardForStatus(TrainerStateScanning, true) &&
		Trainer::KeepPerfectGuardForStatus(TrainerStateReady, true),
		"Temporary resource scans and scene transitions must not disable an independently validated native guard.");
	Require(!Trainer::KeepPerfectGuardForStatus(TrainerStateReady, false) &&
		!Trainer::KeepPerfectGuardForStatus(TrainerStateUnsupportedBuild, true) &&
		!Trainer::KeepPerfectGuardForStatus(TrainerStateAccessDenied, true) &&
		!Trainer::KeepPerfectGuardForStatus(TrainerStateWrongCopy, true) &&
		!Trainer::KeepPerfectGuardForStatus(static_cast<TrainerState>(99), true) &&
		!Trainer::KeepPerfectGuardForStatus(TrainerStateError, true) &&
		!Trainer::KeepPerfectGuardForStatus(TrainerStateStopped, true),
		"Disconnects, incompatible games and failed monitoring must disarm protection.");
	Trainer::PerfectBindings Bindings{
		NativeNotify, NativeStats, NativeReset, offsetof(Player, Stats), offsetof(Player, Streak),
		offsetof(Statistics, Owner), offsetof(Statistics, Hits), offsetof(Statistics, Damage), ShouldProtect,
		NativeScript, offsetof(Kills, Owner), IsHitResetScript};
	Require(!Trainer::InstallPerfectHooks({}, Error) && !Error.empty(), "Incomplete native bindings must be rejected.");
	auto Invalid = Bindings;
	int NonExecutable{};
	Invalid.ResetKills = reinterpret_cast<Trainer::PerfectResetFunction>(&NonExecutable);
	Require(!Trainer::InstallPerfectHooks(Invalid, Error), "A partial native installation must fail for nonexecutable targets.");
	Invalid = Bindings;
	Invalid.CallBuffScript = reinterpret_cast<Trainer::PerfectScriptFunction>(&NonExecutable);
	Require(!Trainer::InstallPerfectHooks(Invalid, Error), "Failure to install the deferred callback hook must roll back all hit hooks.");
	Actor Unhooked;
	Hit Damage;
	CallNotify(Unhooked.Self, Damage);
	Require(Unhooked.Self.HP == 90 && Unhooked.Stats.Hits == 1 && Unhooked.Streak.Count == 0,
		"Failed installation must restore the original native functions.");
	if (!Trainer::InstallPerfectHooks(Bindings, Error)) throw std::runtime_error(Error);
	struct Cleanup { ~Cleanup() { std::string Error; Trainer::RemovePerfectHooks(Error); } } Cleanup;

	Actor Hero;
	Protected = &Hero.Self;
	Enabled = false;
	CallNotify(Hero.Self, Damage);
	Require(Hero.Self.HP == 90 && Hero.Stats.Hits == 1 && Hero.Stats.Damage == 10 && Hero.Streak.Count == 0 && Hero.Streak.Extra == 0,
		"Disabled protection must preserve normal damage, statistics and streak resets.");
	Hero.Stats.Hits = 0; Hero.Stats.Damage = 0; Hero.Streak = {};
	Enabled = true;
	CallNotify(Hero.Self, Damage);
	Require(Hero.Self.HP == 80 && Hero.Self.HitNotifications == 2 && Hero.Stats.Other == 2,
		"Protected hits must still apply HP loss, hit notifications and unrelated statistics.");
	Require(Hero.Stats.Hits == 0 && Hero.Stats.Damage == 0 && Hero.Streak.Count == 7 && Hero.Streak.Extra == 3,
		"Protected hits must preserve perfect indicators, settlement damage and every attached kill counter.");
	++Hero.Streak.Count; ++Hero.Streak.Extra;
	CallNotify(Hero.Self, Damage);
	Require(Hero.Streak.Count == 8 && Hero.Streak.Extra == 4,
		"Protection must retain newly earned kills rather than freeze a captured counter value.");
	Hero.Stats.Hits = 2; Hero.Stats.Damage = 35;
	CallNotify(Hero.Self, Damage);
	Require(Hero.Stats.Hits == 2 && Hero.Stats.Damage == 35,
		"Enabling protection must not retroactively rewrite earlier damage or exploration baselines.");
	Actor Enemy;
	Hit Nested{10, &Enemy.Self};
	CallNotify(Hero.Self, Nested);
	Require(Enemy.Self.HP == 90 && Enemy.Stats.Hits == 1 && Enemy.Stats.Damage == 10 && Enemy.Streak.Count == 0,
		"Nested enemy notifications must not inherit the local player's protection scope.");
	Require(Hero.Streak.Count == 8 && Hero.Streak.Extra == 4, "Nested notifications must restore the outer local-player scope.");
	ScriptCall HitCleanup{&Hero.Streak, "OtherBuffEnd"};
	Hit ScriptedHit{10, nullptr, false, &HitCleanup};
	CallNotify(Hero.Self, ScriptedHit);
	Require(Hero.Streak.Count == 8 && Hero.Streak.Extra == 4 && HitCleanup.Calls == 1,
		"Buff callbacks inside a protected hit must retain the original notification's streak protection.");
	CallReset(Hero.Streak);
	Require(Hero.Streak.Count == 0 && Hero.Streak.Extra == 0, "Stage and lifecycle resets outside a hit notification must still run.");
	Hero.Streak = {};
	Online = true;
	CallNotify(Hero.Self, Damage);
	Require(Hero.Stats.Hits == 3 && Hero.Streak.Count == 0, "Online combat must use normal statistics.");
	Online = false; Coop = true; Hero.Streak = {};
	CallNotify(Hero.Self, Damage);
	Require(Hero.Stats.Hits == 4 && Hero.Streak.Count == 0, "Local cooperative combat must use normal statistics.");
	Coop = false;
	Hero.Self.HP = 5; Hero.Streak = {};
	CallNotify(Hero.Self, Damage);
	Require(Hero.Self.HP == -5 && Hero.Self.Dead && Hero.Stats.Hits == 4,
		"Perfect protection must allow lethal HP damage and normal death state.");
	Hit Failing{1, nullptr, true};
	try { CallNotify(Hero.Self, Failing); Require(false, "The simulated native exception must propagate."); }
	catch (const std::runtime_error&) {}
	Require(Hero.Stats.Hits == 4, "Statistics and hit scopes must restore when a native callback fails.");
	CallReset(Hero.Streak);
	Require(Hero.Streak.Count == 0, "An exception must not leave a protected scope armed for lifecycle resets.");
	Enabled = false;
	CallNotify(Hero.Self, Damage);
	Require(Hero.Stats.Hits == 5 && Hero.Streak.Count == 0, "Disabling must immediately restore normal processing for future hits.");

	Enabled = true;
	Actor FirstRun, NextRun;
	Protected = &FirstRun.Self;
	CallNotify(FirstRun.Self, Damage);
	ScriptCall Deferred{&FirstRun.Streak};
	CallScript(Deferred);
	Require(FirstRun.Streak.Count == 7 && FirstRun.Streak.Extra == 3 && Deferred.Calls == 1,
		"A delayed End_pt_behit callback must preserve every kill counter while still running its other effects.");
	Protected = &NextRun.Self;
	CallNotify(NextRun.Self, Damage);
	ScriptCall NextDeferred{&NextRun.Streak};
	CallScript(NextDeferred);
	Require(NextRun.Streak.Count == 7 && NextRun.Streak.Extra == 3 && NextRun.Stats.Hits == 0 && NextRun.Self.HP == 90,
		"The next run's replacement player must keep perfect status and streaks across immediate and delayed hit processing.");
	++NextRun.Streak.Count; ++NextRun.Streak.Extra;
	CallScript(NextDeferred);
	Require(NextRun.Streak.Count == 8 && NextRun.Streak.Extra == 4,
		"Delayed protection must preserve newly earned kills instead of freezing a previous run's counter.");
	CallScript(Deferred);
	Require(FirstRun.Streak.Count == 0 && FirstRun.Streak.Extra == 0,
		"A previous run's player must no longer inherit delayed-reset protection.");
	Actor ScriptEnemy;
	ScriptCall NestedEnemy{&ScriptEnemy.Streak};
	NextDeferred.Nested = &NestedEnemy;
	CallScript(NextDeferred);
	Require(ScriptEnemy.Streak.Count == 0 && NextRun.Streak.Count == 8,
		"Nested enemy resets must run normally and restore the outer local-player callback scope.");
	ScriptCall Unrelated{&NextRun.Streak, "OtherBuffEnd"};
	NextDeferred.Nested = &Unrelated;
	CallScript(NextDeferred);
	Require(NextRun.Streak.Count == 0 && NextRun.Streak.Extra == 0 && Unrelated.Calls == 1,
		"An unrelated nested buff callback must not inherit the protected hit-reset scope.");
	NextDeferred.Nested = nullptr;
	NextRun.Streak.Count = 9; NextRun.Streak.Extra = 5;
	NextDeferred.Throw = true;
	try { CallScript(NextDeferred); Require(false, "The deferred native exception must propagate."); }
	catch (const std::runtime_error&) {}
	Require(NextRun.Streak.Count == 9 && NextRun.Streak.Extra == 5, "Deferred protection must survive a failing buff callback.");
	CallReset(NextRun.Streak);
	Require(NextRun.Streak.Count == 0 && NextRun.Streak.Extra == 0,
		"Stage and lifecycle resets must still run after a delayed callback, including exceptional exits.");
	NextDeferred.Throw = false;
	Kills Replaced;
	Replaced.Owner = &NextRun.Self;
	ScriptCall StaleCounter{&Replaced};
	CallScript(StaleCounter);
	Require(Replaced.Count == 0, "A detached counter must not be protected just because its owner is still active.");
	for (int Mode = 0; Mode < 3; ++Mode)
	{
		NextRun.Streak.Count = 9; NextRun.Streak.Extra = 5;
		Enabled = Mode != 0; Online = Mode == 1; Coop = Mode == 2;
		CallScript(NextDeferred);
		Require(NextRun.Streak.Count == 0 && NextRun.Streak.Extra == 0,
			"Disabled, online and cooperative modes must run delayed streak resets normally.");
	}
	Enabled = Online = Coop = false;
	Require(Trainer::RemovePerfectHooks(Error), "Native test hooks must detach successfully.");
	NextRun.Streak.Count = 9;
	CallScript(NextDeferred);
	Require(NextRun.Streak.Count == 0, "Removing hooks must restore the original buff callback.");

	Trainer::PerfectCommand Command;
	Command.RequesterProcessId = 12; Command.TargetProcessId = 34; Command.Nonce = 56;
	Require(Trainer::ValidPerfectCommand(Command, 12, 34, 56) &&
		!Trainer::ValidPerfectCommand(Command, 13, 34, 56) && !Trainer::ValidPerfectCommand(Command, 12, 35, 56) &&
		!Trainer::ValidPerfectCommand(Command, 12, 34, 57), "Perfect sessions must validate caller, target and request identity.");
	++Command.Version;
	Require(!Trainer::ValidPerfectCommand(Command, 12, 34, 56), "Incompatible perfect session versions must be rejected.");
	Require(Trainer::PerfectHeartbeatAlive(10000, 9999) &&
		!Trainer::PerfectHeartbeatAlive(10000, 5000) && !Trainer::PerfectHeartbeatAlive(10000, 10001),
		"Lost or invalid heartbeats must expire protection.");
	Trainer::PerfectGuard Guard;
	std::stop_source Stop;
	Stop.request_stop();
	Require(!Guard.Enable(GetCurrentProcessId(), Error, Stop.get_token()) && !Guard.Enabled(GetCurrentProcessId()),
		"Cancellation must happen before dispatching a native perfect request.");
	Guard.Disable();

	std::array<char, 2048> Message{};
	const auto Engine = TrainerCreate(Trainer::DefaultConfigPath().c_str(), Message.data(), Message.size());
	if (!Engine) throw std::runtime_error(Message.data());
	TrainerSetLanguage(Engine, TrainerLanguageEnglish, Message.data(), Message.size());
	Require(!TrainerSetPerfectGuard(Engine, 1, Message.data(), Message.size()) &&
		std::string(Message.data()) == "A compatible game process is not connected.", "Disconnected engines must not arm perfect protection.");
	Require(TrainerSetPerfectGuard(Engine, 0, Message.data(), Message.size()), "Disabling must always work even after disconnection.");
	Require(!TrainerSetPerfectGuard(Engine, 2, Message.data(), Message.size()) &&
		!TrainerSetPerfectGuard(nullptr, 1, Message.data(), Message.size()), "Invalid public switch requests must fail.");
	TrainerSnapshot Snapshot{};
	TrainerGetSnapshot(Engine, &Snapshot, Message.data(), Message.size());
	Require(!Snapshot.PerfectGuardEnabled, "Perfect protection must be off by default.");
	TrainerDestroy(Engine);
	return Failures != 0;
}
