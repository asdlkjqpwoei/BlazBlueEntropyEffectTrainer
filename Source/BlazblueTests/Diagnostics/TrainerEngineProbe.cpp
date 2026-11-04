#include "Config.h"
#include "Paths.h"
#include "TrainerEngine.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

int RunEngineProbe(int ArgumentCount, char** Arguments)
{
	const bool ApOnly = ArgumentCount == 2 && std::strcmp(Arguments[1], "--ap") == 0;
	if (ArgumentCount != 1 && !ApOnly)
	{
		std::cerr << "Usage: trainer_engine_probe [--ap]\n";
		return 1;
	}
	try
	{
		const Trainer::Config Configuration = Trainer::LoadConfig(
			Trainer::DefaultConfigPath());
		if (ApOnly && !Configuration.PuertsAp)
		{
			std::cerr << "An AP runtime profile is required.\n";
			return 1;
		}
		Trainer::Engine Engine(Configuration);
		std::string Error;
		if (!Engine.Start(Error))
		{
			std::cerr << "Engine start failed: " << Error << '\n';
			return 1;
		}

		const auto Deadline = std::chrono::steady_clock::now() +
			std::chrono::minutes(20);
		TrainerSnapshot Previous{};
		bool HavePrevious = false;
		while (std::chrono::steady_clock::now() < Deadline)
		{
			const TrainerSnapshot Snapshot = Engine.Snapshot();
			if (!HavePrevious ||
				Snapshot.State != Previous.State ||
				Snapshot.ProcessDetected != Previous.ProcessDetected ||
				Snapshot.Refresh.AddressCount != Previous.Refresh.AddressCount ||
				Snapshot.Refresh.WriteEnabled != Previous.Refresh.WriteEnabled ||
				Snapshot.Refresh.Minimum != Previous.Refresh.Minimum ||
				Snapshot.Exchange.AddressCount != Previous.Exchange.AddressCount ||
				Snapshot.Exchange.WriteEnabled != Previous.Exchange.WriteEnabled ||
				Snapshot.Exchange.Minimum != Previous.Exchange.Minimum ||
				Snapshot.Ap.AddressCount != Previous.Ap.AddressCount ||
				Snapshot.Ap.WriteEnabled != Previous.Ap.WriteEnabled ||
				Snapshot.Ap.Minimum != Previous.Ap.Minimum ||
				std::strncmp(
					Snapshot.StatusMessage,
					Previous.StatusMessage,
					sizeof(Snapshot.StatusMessage)) != 0 ||
				std::strncmp(
					Snapshot.DetailMessage,
					Previous.DetailMessage,
					sizeof(Snapshot.DetailMessage)) != 0)
			{
				std::cout << "state=" << Snapshot.State
					<< " process=" << Snapshot.ProcessDetected
					<< " build=" << Snapshot.BuildVerified
					<< " refresh_count=" << Snapshot.Refresh.AddressCount
					<< " refresh_writable=" << Snapshot.Refresh.WriteEnabled
					<< " refresh_range=" << Snapshot.Refresh.Minimum
					<< '-' << Snapshot.Refresh.Maximum
					<< " exchange_count=" << Snapshot.Exchange.AddressCount
					<< " exchange_writable=" << Snapshot.Exchange.WriteEnabled
					<< " exchange_range=" << Snapshot.Exchange.Minimum
					<< '-' << Snapshot.Exchange.Maximum
					<< " ap_count=" << Snapshot.Ap.AddressCount
					<< " ap_writable=" << Snapshot.Ap.WriteEnabled
					<< " ap_range=" << Snapshot.Ap.Minimum
					<< '-' << Snapshot.Ap.Maximum
					<< "\nstatus=" << Snapshot.StatusMessage
					<< "\ndetail=" << Snapshot.DetailMessage
					<< "\n" << std::flush;
				Previous = Snapshot;
				HavePrevious = true;
			}

			if (ApOnly ? Snapshot.Ap.WriteEnabled != 0 :
				Snapshot.Refresh.WriteEnabled != 0 && Snapshot.Exchange.WriteEnabled != 0)
			{
				Engine.Stop();
				return 0;
			}
			if (Snapshot.State == TrainerStateError ||
				Snapshot.State == TrainerStateUnsupportedBuild ||
				Snapshot.State == TrainerStateAccessDenied)
			{
				Engine.Stop();
				return 2;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}

		Engine.Stop();
		return 3;
	}
	catch (const std::exception& Exception)
	{
		std::cerr << "Engine probe failed: " << Exception.what() << '\n';
		return 1;
	}
}
