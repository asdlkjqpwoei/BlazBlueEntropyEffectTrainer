#include "DataBodyCatalog.h"
#include "DataBodySave.h"
#include "Paths.h"
#include "RemoteProcess.h"
#include "GamePresenceMonitor.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	template <typename Action> double Measure(Action &&Work)
	{
		const auto Started = Clock::now();
		Work();
		return std::chrono::duration<double, std::milli>(Clock::now() - Started).count();
	}

	void Describe(const char *Name, std::vector<double> Measurements)
	{
		if (Measurements.empty())
			return;
		std::sort(Measurements.begin(), Measurements.end());
		std::cout << Name << " samples=" << Measurements.size()
				  << " median_ms=" << Measurements[Measurements.size() / 2] << " max_ms=" << Measurements.back()
				  << '\n';
	}
} // namespace

int RunEditorPerformance(int ArgumentCount, char **Arguments)
{
	if (ArgumentCount != 3)
	{
		std::cerr << "Usage: BlazblueTests --editor-performance <game-directory> <save-copy>\n";
		return 1;
	}
	const auto Configuration = Trainer::LoadConfig(Trainer::DefaultConfigPath());
	std::cout << "read_only=true game_writes=false save_writes=false\n";
	std::vector<double> PresenceTimes;
	for (int Index = 0; Index < 12; ++Index)
	{
		PresenceTimes.push_back(Measure([&] {
			const auto Presence = Trainer::RemoteProcess::ObserveGamePresence(Configuration);
			if (Presence.State == Trainer::GameRunState::Unknown)
				throw std::runtime_error(Presence.Error);
		}));
	}
	Describe("fresh_game_presence", PresenceTimes);
	Trainer::GamePresenceMonitor Monitor(Configuration);
	std::vector<double> UiStateTimes;
	for (int Index = 0; Index < 10000; ++Index)
		UiStateTimes.push_back(Measure([&] { Monitor.Snapshot(); }));
	Describe("cached_ui_game_presence", UiStateTimes);
	Trainer::DataBodyCatalog Catalog;
	std::cout << "catalog_load_ms=" << Measure([&] {
		Catalog = Trainer::DataBodyCatalog::Load(Trainer::PathFromUtf8(Arguments[1]));
	}) << '\n';
	std::vector<double> CompletionTimes;
	for (const auto &[Id, Row] : Catalog.Strategies)
	{
		if (Row.Qualities.empty())
			continue;
		CompletionTimes.push_back(Measure([&] { Catalog.CompleteStrategies({{Id, *Row.Qualities.rbegin()}}, {}); }));
	}
	Describe("strategy_completion", CompletionTimes);
	double LoadTime = 0;
	std::vector<double> DecodeTimes;
	std::shared_ptr<Trainer::DataBodySave> Save;
	LoadTime = Measure([&] {
		Save = std::make_shared<Trainer::DataBodySave>(Trainer::PathFromUtf8(Arguments[2]), [&] {
			const auto Presence = Trainer::RemoteProcess::ObserveGamePresence(Configuration);
			if (Presence.State != Trainer::GameRunState::Stopped)
				throw std::runtime_error("The game must be stopped for copied-save performance validation.");
		});
	});
	std::cout << "save_load_with_fresh_guards_ms=" << LoadTime << '\n';
	for (int Index = 0; Index < 10; ++Index)
		DecodeTimes.push_back(Measure([&] { Save->Bodies(); }));
	Describe("saved_body_decode", DecodeTimes);
	const auto Bodies = Save->Bodies();
	if (!Bodies.empty())
	{
		Trainer::DataBodyRequest Edit;
		const auto &Body = Bodies.back();
		Edit.ExistingUid = Body.Uid;
		Edit.ActorId = Body.ActorId;
		Edit.Score = Body.Score;
		Edit.Inherited = Body.Inherited;
		std::cout << "save_prepare_with_fresh_guard_ms=" << Measure([&] { Save->Prepare(Catalog, Edit); }) << '\n';
	}
	return 0;
}
